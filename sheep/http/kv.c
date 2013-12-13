/*
 * Copyright (C) 2013 MORITA Kazutaka <morita.kazutaka@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements backend kv functions for object storage. */

#include "sheep_priv.h"
#include "kv.h"

#define FOR_EACH_VDI(nr, vdis) FOR_EACH_BIT(nr, vdis, SD_NR_VDIS)

struct bucket_inode_hdr {
	char bucket_name[SD_MAX_BUCKET_NAME];
	uint64_t obj_count;
	uint64_t bytes_used;
	uint32_t onode_vid;
	uint32_t data_vid;		/* data of objects store in this vdi */
};

struct bucket_inode {
	union {
		struct bucket_inode_hdr hdr;
		uint8_t data[SD_MAX_BUCKET_NAME << 1];
	};
};

#define MAX_BUCKETS (SD_MAX_VDI_SIZE / sizeof(struct bucket_inode))
#define BUCKETS_PER_SD_OBJ (SD_DATA_OBJ_SIZE / sizeof(struct bucket_inode))

static int lookup_vdi(const char *name, uint32_t *vid)
{
	int ret;
	struct vdi_info info = {};
	struct vdi_iocb iocb = {
		.name = name,
		.data_len = strlen(name),
	};

	ret = vdi_lookup(&iocb, &info);
	switch (ret) {
	case SD_RES_SUCCESS:
		*vid = info.vid;
		break;
	case SD_RES_NO_VDI:
		sd_info("no such vdi %s", name);
		break;
	default:
		sd_err("Failed to find vdi %s %s", name, sd_strerror(ret));
	}

	return ret;
}

static int kv_create_hyper_volume(const char *name, uint32_t *vdi_id)
{
	struct sd_req hdr;
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	int ret;
	char buf[SD_MAX_VDI_LEN] = {0};

	pstrcpy(buf, SD_MAX_VDI_LEN, name);

	sd_init_req(&hdr, SD_OP_NEW_VDI);
	hdr.flags = SD_FLAG_CMD_WRITE;
	hdr.data_length = SD_MAX_VDI_LEN;

	hdr.vdi.vdi_size = SD_MAX_VDI_SIZE;
	hdr.vdi.copies = sys->cinfo.nr_copies;
	hdr.vdi.copy_policy = sys->cinfo.copy_policy;
	hdr.vdi.store_policy = 1;

	ret = exec_local_req(&hdr, buf);
	if (rsp->result != SD_RES_SUCCESS)
		sd_err("Failed to create VDI %s: %s", name,
		       sd_strerror(rsp->result));

	if (vdi_id)
		*vdi_id = rsp->vdi.vdi_id;

	return ret;
}

static int discard_data_obj(uint64_t oid)
{
	int ret;
	struct sd_req hdr;

	sd_init_req(&hdr, SD_OP_DISCARD_OBJ);
	hdr.obj.oid = oid;

	ret = exec_local_req(&hdr, NULL);
	if (ret != SD_RES_SUCCESS)
		sd_err("Failed to discard data obj %lu %s", oid,
		       sd_strerror(ret));

	return ret;
}

static int kv_delete_vdi(const char *name)
{
	int ret;
	struct sd_req hdr;
	char data[SD_MAX_VDI_LEN] = {0};
	uint32_t vid;

	ret = lookup_vdi(name, &vid);
	if (ret != SD_RES_SUCCESS)
		return ret;

	sd_init_req(&hdr, SD_OP_DEL_VDI);
	hdr.flags = SD_FLAG_CMD_WRITE;
	hdr.data_length = sizeof(data);
	pstrcpy(data, SD_MAX_VDI_LEN, name);

	ret = exec_local_req(&hdr, data);
	if (ret != SD_RES_SUCCESS)
		sd_err("Failed to delete vdi %s %s", name, sd_strerror(ret));

	return ret;
}

/*
 * An account is actually a hyper volume vdi (up to 16PB),
 * all the buckets (or containers, identified by 'struct bucket_inode') are
 * stores in this hyper vdi using hashing algorithm.
 * The bucket also has a hyper vdi named "account/bucket" which stores
 * 'struct kv_onodes'.
 *
 * For example: account "coly" has two buckets "jetta" and "volvo"
 *
 *
 * account vdi
 * +-----------+---+--------------------------+---+--------------------------+--
 * |name: coly |...|bucket_inode (name: jetta)|...|bucket_inode (name: volvo)|..
 * +-----------+---+--------------------------+---+--------------------------+--
 *                                  |                             |
 *                                 /                              |
 * bucket vdi                     /                               |
 * +-----------------+-------+ <--                                |
 * |name: coly/jetta |.......|                                    |
 * +-----------------+-------+                                   /
 *                              bucket vdi                      /
 *                              +-----------------+------+ <----
 *                              | name: coly/volvo|......|
 *                              +-----------------+------+
 */

/* Account operations */

int kv_create_account(const char *account)
{
	uint32_t vdi_id;
	return kv_create_hyper_volume(account, &vdi_id);
}

typedef void (*list_bucket_cb)(struct http_request *req, const char *bucket,
			       void *opaque);

struct list_buckets_arg {
	struct http_request *req;
	void *opaque;
	list_bucket_cb cb;
	uint32_t bucket_counter;
};

static void list_buckets_cb(void *data, enum btree_node_type type, void *arg)
{
	struct sd_extent *ext;
	struct list_buckets_arg *lbarg = arg;
	struct bucket_inode *bnode;
	uint64_t oid;
	char *buf = NULL;
	int ret;

	if (type == BTREE_EXT) {
		ext = (struct sd_extent *)data;
		if (!ext->vdi_id)
			return;

		buf = xzalloc(SD_DATA_OBJ_SIZE);

		oid = vid_to_data_oid(ext->vdi_id, ext->idx);
		ret = sd_read_object(oid, buf, SD_DATA_OBJ_SIZE, 0);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to read data object %lx", oid);
			goto out;
		}
		/* loop all bucket_inodes in this data-object */
		for (int i = 0; i < BUCKETS_PER_SD_OBJ; i++) {
			bnode = (struct bucket_inode *)
				(buf + i * sizeof(struct bucket_inode));
			if (bnode->hdr.onode_vid == 0)
				continue;
			if (lbarg->cb)
				lbarg->cb(lbarg->req, bnode->hdr.bucket_name,
					  (void *)lbarg->opaque);
			lbarg->bucket_counter++;
		}
	}
out:
	free(buf);
}

/* get number of buckets in this account */
static int kv_get_account(const char *account, uint32_t *nr_buckets)
{
	struct sd_inode inode;
	uint64_t oid;
	uint32_t account_vid;
	int ret;

	ret = lookup_vdi(account, &account_vid);
	if (ret != SD_RES_SUCCESS)
		return ret;

	/* read account vdi out */
	oid = vid_to_vdi_oid(account_vid);
	ret = sd_read_object(oid, (char *)&inode, sizeof(struct sd_inode), 0);
	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to read inode header %lx", oid);
		return ret;
	}

	struct list_buckets_arg arg = {NULL, NULL, NULL, 0};
	traverse_btree(sheep_bnode_reader, &inode, list_buckets_cb, &arg);
	if (nr_buckets)
		*nr_buckets = arg.bucket_counter;

	return SD_RES_SUCCESS;
}

int kv_read_account(const char *account, uint32_t *nr_buckets)
{
	int ret;

	ret = kv_get_account(account, nr_buckets);
	if (ret != SD_RES_SUCCESS)
		sd_err("Failed to get number of buckets in %s", account);
	return ret;
}

int kv_update_account(const char *account)
{
	/* TODO: update metadata of the account */
	return -1;
}

int kv_delete_account(const char *account)
{
	int ret;

	ret = kv_delete_vdi(account);
	if (ret != SD_RES_SUCCESS)
		sd_err("Failed to delete vdi %s", account);

	return ret;
}

/* Bucket operations */

static int lookup_bucket(struct http_request *req, const char *bucket,
			 uint32_t *vid)
{
	int ret;
	struct vdi_info info = {};
	struct vdi_iocb iocb = {
		.name = bucket,
		.data_len = strlen(bucket),
	};

	ret = vdi_lookup(&iocb, &info);
	switch (ret) {
	case SD_RES_SUCCESS:
		*vid = info.vid;
		break;
	case SD_RES_NO_VDI:
		sd_info("no such bucket %s", bucket);
		http_response_header(req, NOT_FOUND);
		break;
	default:
		sd_err("Failed to find bucket %s %s", bucket, sd_strerror(ret));
		http_response_header(req, INTERNAL_SERVER_ERROR);
	}

	return ret;
}

/*
 * Delete bucket(container) inode in account vdi.
 * idx: the target hash positon of bucket
 * Return the position of bucket_inode in sd-data-object if success
 * Return BUCKETS_PER_SD_OBJ if bucket_inode is not found
 * Return -1 if some errors happend
 */
static int delete_bucket(struct sd_inode *account_inode, uint64_t idx,
			 const char *bucket)
{
	struct bucket_inode *bnode;
	char *buf = NULL;
	uint32_t vdi_id;
	uint64_t oid;
	uint64_t data_index = idx / BUCKETS_PER_SD_OBJ;
	int offset = idx % BUCKETS_PER_SD_OBJ;
	int ret, i, empty_buckets = 0, found = 0;

	vdi_id = INODE_GET_VID(account_inode, data_index);
	if (!vdi_id) {
		sd_err("the %lu in vdi %s is not exists", data_index,
		       account_inode->name);
		ret = -1;
		goto out;
	}

	oid = vid_to_data_oid(account_inode->vdi_id, data_index);
	buf = xzalloc(SD_DATA_OBJ_SIZE);
	ret = sd_read_object(oid, buf, SD_DATA_OBJ_SIZE, 0);
	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to read inode header %lx", oid);
		ret = -1;
		goto out;
	}

	for (i = 0; i < BUCKETS_PER_SD_OBJ; i++) {
		char vdi_name[SD_MAX_VDI_LEN];
		bnode = (struct bucket_inode *)
			(buf + i * sizeof(struct bucket_inode));
		/* count all empty buckets in this sd-data-obj */
		if (bnode->hdr.onode_vid == 0) {
			empty_buckets++;
			continue;
		}
		if (strncmp(bnode->hdr.bucket_name, bucket, SD_MAX_BUCKET_NAME))
			continue;

		if (i < offset)
			panic("postion of bucket inode %d is smaller than %d",
			      i, offset);

		found = i;
		/* find the bnode */
		bnode->hdr.onode_vid = 0;
		snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s",
			 account_inode->name, bucket);
		/* delete vdi which store kv_onode */
		ret = kv_delete_vdi(vdi_name);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to delete vdi %s", vdi_name);
			ret = -1;
			goto out;
		}
		/* delete vdi which store object data */
		snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s/allocator",
			 account_inode->name, bucket);
		ret = kv_delete_vdi(vdi_name);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to delete vdi %s", vdi_name);
			ret = -1;
			goto out;
		}
		sd_debug("delete vdi %s success", vdi_name);
	}

	if (!found) {
		ret = BUCKETS_PER_SD_OBJ;
		goto out;
	}

	/*
	 * if only this bucket_inode is in the sd-data-obj,
	 * then delete this sd-data-obj
	 */
	if (empty_buckets == BUCKETS_PER_SD_OBJ - 1) {
		ret = discard_data_obj(oid);
		if (ret != SD_RES_SUCCESS) {
			ret = -1;
			goto out;
		}
		INODE_SET_VID(account_inode, data_index, 0);
		ret = sd_inode_write_vid(sheep_bnode_writer, account_inode,
					 data_index, vdi_id, vdi_id, 0, false,
					 false);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to write inode %x", vdi_id);
			ret = -1;
			goto out;
		}
		sd_debug("discard obj %lx and update vdi %x success",
			 oid, vdi_id);
	} else {
		ret = sd_write_object(oid, buf, sizeof(struct bucket_inode),
				   i * sizeof(struct bucket_inode), false);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to write object %lx", oid);
			ret = -1;
			goto out;
		}
	}

	sd_debug("write object oid %lx success", oid);
	ret = found;
out:
	free(buf);
	return ret;
}

/*
 * Add bucket(container) inode into account vdi.
 * idx: the target hash positon of bucket
 * Return the position of bucket_inode in sd-data-object if success
 * Return BUCKETS_PER_SD_OBJ if the data-object is full of bucket_inode
 * Return -1 if some error happend
 */
static int add_bucket(struct sd_inode *account_inode, uint64_t idx,
		      const char *bucket)
{
	struct bucket_inode *bnode;
	char *buf = NULL;
	uint32_t vdi_id;
	uint64_t oid;
	uint64_t data_index = idx / BUCKETS_PER_SD_OBJ;
	int offset = idx % BUCKETS_PER_SD_OBJ;
	int ret, i;
	bool create = false;

	buf = xzalloc(SD_DATA_OBJ_SIZE);

	vdi_id = INODE_GET_VID(account_inode, data_index);
	oid = vid_to_data_oid(account_inode->vdi_id, data_index);
	sd_debug("oid %x %lx %lx", account_inode->vdi_id, data_index, oid);
	/* the data object is exists */
	if (vdi_id) {
		ret = sd_read_object(oid, buf, SD_DATA_OBJ_SIZE, 0);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to read inode header %lx", oid);
			ret = -1;
			goto out;
		}
	} else
		create = true;

	sd_debug("bucket_inode offset %d %lu", offset, BUCKETS_PER_SD_OBJ);
	for (i = offset; i < BUCKETS_PER_SD_OBJ; i++) {
		char vdi_name[SD_MAX_VDI_LEN];
		bnode = (struct bucket_inode *)
			(buf + i * sizeof(struct bucket_inode));
		if (bnode->hdr.onode_vid != 0)
			continue;

		/* the bnode not used */
		strncpy(bnode->hdr.bucket_name, bucket, SD_MAX_BUCKET_NAME);
		bnode->hdr.obj_count = 0;
		bnode->hdr.bytes_used = 0;
		snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s",
			 account_inode->name, bucket);
		/* create vdi to store kv_onode */
		ret = kv_create_hyper_volume(vdi_name, &(bnode->hdr.onode_vid));
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to create hyper volume %d", ret);
			ret = -1;
			goto out;
		}
		snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s/allocator",
			 account_inode->name, bucket);
		/* create vdi to store objects */
		ret = kv_create_hyper_volume(vdi_name, &(bnode->hdr.data_vid));
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to create hyper volume %d", ret);
			ret = -1;
			goto out;
		}
		ret = oalloc_init(bnode->hdr.data_vid);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to init allocator on %x",
			       bnode->hdr.data_vid);
			ret = -1;
			goto out;
		}
		sd_debug("create hyper volume %s success", vdi_name);
		break;
	}

	if (i >= BUCKETS_PER_SD_OBJ) {
		ret = BUCKETS_PER_SD_OBJ;
		goto out;
	}

	/* write bnode back to account-vdi */
	if (create)
		ret = sd_write_object(oid, buf, SD_DATA_OBJ_SIZE, 0, create);
	else
		ret = sd_write_object(oid, buf, sizeof(struct bucket_inode),
				   i * sizeof(struct bucket_inode), create);

	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to write object %lx", oid);
		ret = -1;
		goto out;
	}

	sd_debug("write object oid %lx success", oid);

	/* update index of vdi */
	if (create) {
		vdi_id = account_inode->vdi_id;
		INODE_SET_VID(account_inode, data_index, vdi_id);
		ret = sd_inode_write_vid(sheep_bnode_writer, account_inode,
					 data_index, vdi_id, vdi_id, 0, false,
					 false);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to write inode %x", vdi_id);
			ret = -1;
			goto out;
		}
		sd_debug("write account inode success");
	}

	ret = i;
out:
	free(buf);
	return ret;
}

static int kv_get_bucket(struct sd_inode *account_inode, uint32_t account_vid,
			 const char *account, const char *bucket)
{
	char vdi_name[SD_MAX_VDI_LEN];
	uint64_t oid;
	uint32_t bucket_vid;
	int ret;

	/* read account vdi out */
	oid = vid_to_vdi_oid(account_vid);
	ret = sd_read_object(oid, (char *)account_inode,
			  sizeof(struct sd_inode), 0);
	if (ret != SD_RES_SUCCESS)
		goto out;

	/* find bucket vdi */
	snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s",
		 account_inode->name, bucket);

	ret = lookup_vdi(vdi_name, &bucket_vid);
out:
	return ret;
}

int kv_create_bucket(const char *account, const char *bucket)
{
	struct sd_inode inode;
	uint64_t hval, i;
	uint32_t account_vid;
	int ret;

	ret = lookup_vdi(account, &account_vid);
	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to find account %s", account);
		return ret;
	}

	ret = kv_get_bucket(&inode, account_vid, account, bucket);
	/*
	 * if lookup bucket success, kv_get_bucket will return SD_RES_SUCCESS,
	 * which means the bucket is already exists.
	 */
	if (ret == SD_RES_SUCCESS) {
		sd_err("bucket %s is exists.", bucket);
		ret = SD_RES_VDI_EXIST;
		return ret;
	} else if (ret != SD_RES_NO_VDI)
		return ret;

	/*
	 * if kv_get_bucket() return SD_RES_NO_VDI, it means we can
	 * create bucket normally now.
	 */

	sd_debug("read account inode success");

	hval = sd_hash(bucket, strlen(bucket));
	for (i = 0; i < MAX_BUCKETS; i++) {
		uint64_t idx = (hval + i) % MAX_BUCKETS;
		ret = add_bucket(&inode, idx, bucket);
		/* data-object is full */
		if (ret == BUCKETS_PER_SD_OBJ) {
			i += BUCKETS_PER_SD_OBJ;
			continue;
		} else if (ret < 0) {
			sd_err("Failed to add bucket");
			return ret;
		}
		/* add bucket success */
		sd_debug("add bucket success");
		break;
	}

	if (i >= MAX_BUCKETS) {
		sd_err("Containers in vdi %s is full!", account);
		return -1;
	}
	return 0;
}

int kv_read_bucket(const char *account, const char *bucket)
{
	/* TODO: read metadata of the bucket */
	return -1;
}

int kv_update_bucket(const char *account, const char *bucket)
{
	/* TODO: update metadata of the bucket */
	return -1;
}

/* return SD_RES_NO_VDI if bucket is not existss */
int kv_delete_bucket(const char *account, const char *bucket)
{
	struct sd_inode inode;
	uint64_t hval, i;
	uint32_t account_vid;
	int ret;

	ret = lookup_vdi(account, &account_vid);
	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to find account %s", account);
		return ret;
	}

	ret = kv_get_bucket(&inode, account_vid, account, bucket);
	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to get bucket");
		return ret;
	}

	hval = sd_hash(bucket, strlen(bucket));
	for (i = 0; i < MAX_BUCKETS; i++) {
		uint64_t idx = (hval + i) % MAX_BUCKETS;
		ret = delete_bucket(&inode, idx, bucket);
		if (ret == BUCKETS_PER_SD_OBJ) {
			i += BUCKETS_PER_SD_OBJ;
			continue;
		} else if (ret < 0) {
			sd_err("Failed to delete bucket %d", ret);
			return ret;
		}
		/* delete bucket success */
		sd_debug("delete bucket success");
		break;
	}

	if (i >= MAX_BUCKETS) {
		sd_err("Can't find bucket %s", bucket);
		return SD_RES_NO_VDI;
	}
	return SD_RES_SUCCESS;
}

int kv_list_buckets(struct http_request *req, const char *account,
		    list_bucket_cb cb, void *opaque)
{
	struct sd_inode account_inode;
	uint32_t account_vid;
	uint64_t oid;
	int ret;

	ret = lookup_vdi(account, &account_vid);
	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to find account %s", account);
		return ret;
	}

	/* read account vdi out */
	oid = vid_to_vdi_oid(account_vid);
	ret = sd_read_object(oid, (char *)&account_inode,
			  sizeof(struct sd_inode), 0);
	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to read account inode header %lx", oid);
		return ret;
	}

	struct list_buckets_arg arg = {req, opaque, cb, 0};
	traverse_btree(sheep_bnode_reader, &account_inode,
		       list_buckets_cb, &arg);
	return SD_RES_SUCCESS;
}

/*
 * A bucket contains two vdi: one (vdi_id)  stores 'struct kv_onode' by hash
 * algorithm and another one (data_vid) stores data of objects.
 * The first vdi names "account/bucket" and the second vdi names
 * "account/bucket/allocator".
 *
 * It manage space in data vdi by algorithm in oalloc.c.
 *
 * For example: bucket "fruit" with account 'coly' has two objects "banana"
 *              and "apple"
 *
 *
 *                       --------------------- kv_onode -----------------------
 *                      |                                                      |
 * bucket vdi           v                                                      v
 * +-----------------+--+---------------------------+--------------------------+
 * |name: coly/fruit |..|kv_onode_hdr (name: banana)|onode_extent: start, count|
 * +-----------------+--+---------------------------+--------------------------+
 *                                                                  /
 *                                                                 /
 *                                                     ------------
 *                                                    /
 *		     data_vid                        v
 *                   +---------------------------+---+-----------------+
 *                   |name: coly/fruit/allocator |...|       data      |
 *                   +---------------------------+---+-----------------+
 */

/* Object operations */

/* 4 KB header of kv object index node */
struct kv_onode_hdr {
	union {
		struct {
			char name[SD_MAX_OBJECT_NAME];
			/* a hash value for etag */
			uint8_t sha1[round_up(SHA1_DIGEST_SIZE, 8)];
			uint64_t size;
			uint64_t ctime;
			uint64_t mtime;
			uint32_t data_vid;
			uint32_t nr_extent;
			uint8_t inlined;
			uint8_t pad[5];
		};

		uint8_t __pad[BLOCK_SIZE];
	};
};

struct onode_extent {
	uint64_t start;
	uint64_t count;
};

struct kv_onode {
	struct kv_onode_hdr hdr;
	union {
		uint8_t data[SD_DATA_OBJ_SIZE - sizeof(struct kv_onode_hdr)];
		struct onode_extent o_extent[0];
	};
};

typedef void (*list_object_cb)(struct http_request *req, const char *bucket,
			       const char *object, void *opaque);

struct list_objects_arg {
	struct http_request *req;
	void *opaque;
	const char *bucket;
	list_object_cb cb;
	uint32_t object_counter;
};

static void list_objects_cb(void *data, enum btree_node_type type, void *arg)
{
	struct sd_extent *ext;
	struct list_objects_arg *loarg = arg;
	struct kv_onode *onode = NULL;
	uint64_t oid;
	int ret;

	if (type == BTREE_EXT) {
		ext = (struct sd_extent *)data;
		if (!ext->vdi_id)
			goto out;

		onode = xmalloc(SD_DATA_OBJ_SIZE);

		oid = vid_to_data_oid(ext->vdi_id, ext->idx);
		ret = sd_read_object(oid, (char *)onode, SD_DATA_OBJ_SIZE, 0);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to read data object %lx", oid);
			goto out;
		}

		if (onode->hdr.name[0] == '\0')
			goto out;
		if (loarg->cb)
			loarg->cb(loarg->req, loarg->bucket, onode->hdr.name,
				  loarg->opaque);
		loarg->object_counter++;
	}
out:
	free(onode);
}

#define KV_ONODE_INLINE_SIZE (SD_DATA_OBJ_SIZE - sizeof(struct kv_onode_hdr))

static int kv_write_onode(struct sd_inode *inode, struct kv_onode *onode,
			  uint32_t vid, uint32_t idx, bool overwrite)
{
	uint64_t oid = vid_to_data_oid(vid, idx), len;
	int ret;

	if (onode->hdr.inlined)
		len = onode->hdr.size;
	else
		len = sizeof(struct onode_extent) * onode->hdr.nr_extent;

	if (overwrite) {
		sd_info("overwrite object %s", onode->hdr.name);
		ret = sd_write_object(oid, (char *)onode,
				      sizeof(onode->hdr) + len,
				      0, false);
		if (ret != SD_RES_SUCCESS) {
			sd_err("failed to write object, %" PRIx64, oid);
			goto out;
		}
	} else {
		ret = sd_write_object(oid, (char *)onode,
				      sizeof(onode->hdr) + len,
				      0, true);
		if (ret != SD_RES_SUCCESS) {
			sd_err("failed to create object, %" PRIx64, oid);
			goto out;
		}
		INODE_SET_VID(inode, idx, vid);
		ret = sd_inode_write_vid(sheep_bnode_writer, inode, idx,
					 vid, vid, 0, false, false);
		if (ret != SD_RES_SUCCESS) {
			sd_err("failed to update inode, %" PRIx64,
			       vid_to_vdi_oid(vid));
			goto out;
		}
	}
out:
	return ret;
}

/*
 * Create the object if the index isn't taken. Overwrite the object if it exists
 * Return SD_RES_OBJ_TAKEN if the index is taken by other object.
 */
static int do_kv_create_object(struct http_request *req,
			       struct kv_onode *onode,
			       uint32_t vid, uint32_t idx)
{
	struct sd_inode *inode = xmalloc(sizeof(struct sd_inode));
	uint64_t oid = vid_to_data_oid(vid, idx);
	struct kv_onode_hdr hdr;
	uint32_t tmp_vid;
	int ret;

	ret = sd_read_object(vid_to_vdi_oid(vid), (char *)inode,
			     sizeof(*inode), 0);
	if (ret != SD_RES_SUCCESS) {
		sd_err("failed to read inode, %" PRIx64,
		       vid_to_vdi_oid(vid));
		goto out;
	}
	tmp_vid = INODE_GET_VID(inode, idx);
	if (tmp_vid) {
		ret = sd_read_object(oid, (char *)&hdr, sizeof(hdr), 0);
		if (ret != SD_RES_SUCCESS) {
			sd_err("failed to read object, %" PRIx64, oid);
			goto out;
		}

		if (hdr.name[0] != '\0' &&
		    strcmp(hdr.name, onode->hdr.name) != 0) {
			sd_debug("index %d is already used", idx);
			ret = SD_RES_OBJ_TAKEN;
			goto out;
		}
	}

	ret = kv_write_onode(inode, onode, vid, idx, !!tmp_vid);
	if (ret != SD_RES_SUCCESS)
		sd_err("Failed to write onode");
out:
	free(inode);
	return ret;
}

int kv_create_object(struct http_request *req, const char *account,
		     const char *bucket, const char *name)
{
	struct kv_onode *onode;
	ssize_t size, total_size = 0;
	int ret;
	uint64_t hval, start = 0, count, block, limit;
	uint32_t vid, data_vid;
	struct timeval tv;
	char vdi_name[SD_MAX_VDI_LEN];
	char *data_buf = NULL;

	snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s", account, bucket);
	ret = lookup_bucket(req, vdi_name, &vid);
	if (ret < 0)
		return ret;

	snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s/allocator", account, bucket);
	ret = lookup_bucket(req, vdi_name, &data_vid);
	if (ret < 0)
		return ret;

	onode = xzalloc(sizeof(*onode));

	/* for inlined onode */
	if (req->data_length <= KV_ONODE_INLINE_SIZE) {
		onode->hdr.inlined = 1;
		size = http_request_read(req, onode->data, sizeof(onode->data));
		if (size < 0) {
			sd_err("%s: bucket %s, object %s", sd_strerror(ret),
			       bucket, name);
			http_response_header(req, INTERNAL_SERVER_ERROR);
			ret = -1;
			goto out;
		}
		total_size = size;
	} else {
		sd_debug("data_length: %lu, %lu", req->data_length,
			 SD_DATA_OBJ_SIZE);
		count = (req->data_length + SD_DATA_OBJ_SIZE + 1) /
			SD_DATA_OBJ_SIZE;
		ret = oalloc_new_prepare(data_vid, &start, count);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to prepare allocation of %lu bytes!",
			       req->data_length);
			ret = -1;
			goto out;
		}

		/* receive and write data at first, then write onode */
		data_buf = xmalloc(SD_DATA_OBJ_SIZE);

		sd_debug("start: %lu, count: %lu", start, count);
		for (block = start, limit = start + count;
		     block < limit; block++) {
			sd_debug("block: %lu, limit: %lu", block, limit);
			size = http_request_read(req, data_buf,
						 SD_DATA_OBJ_SIZE);
			total_size += size;
			ret = sd_write_object(vid_to_data_oid(data_vid, block),
					      data_buf, size, 0, true);
			if (ret != SD_RES_SUCCESS) {
				sd_err("Failed to write data object for %"
				       PRIx32" %s", data_vid, sd_strerror(ret));
				goto out;
			}
			if (size < SD_DATA_OBJ_SIZE)
				break;
		}

		sd_debug("DATA_LENGTH: %lu, total size: %lu, last blocks: %lu",
			 req->data_length, total_size, start);

		sd_debug("finish start: %lu, count: %lu", start, count);
		ret = oalloc_new_finish(data_vid, start, count);
		if (ret != SD_RES_SUCCESS) {
			sd_err("Failed to finish allocation of %lu bytes!",
			       req->data_length);
			ret = -1;
			goto out;
		}

		onode->o_extent[0].start = start;
		onode->o_extent[0].count = count;
		onode->hdr.nr_extent = 1;
	}

	if (req->data_length != total_size) {
		sd_err("Failed to receive whole object: %lu %lu",
		       req->data_length, total_size);
		ret = SD_RES_INVALID_PARMS;
		goto out;
	}

	/* after write data, we write onode now */

	gettimeofday(&tv, NULL);
	pstrcpy(onode->hdr.name, sizeof(onode->hdr.name), name);
	onode->hdr.ctime = (uint64_t) tv.tv_sec << 32 | tv.tv_usec * 1000;
	onode->hdr.mtime = onode->hdr.ctime;
	onode->hdr.size = total_size;
	onode->hdr.data_vid = data_vid;

	hval = sd_hash(name, strlen(name));
	for (int i = 0; i < MAX_DATA_OBJS; i++) {
		uint32_t idx = (hval + i) % MAX_DATA_OBJS;

		ret = do_kv_create_object(req, onode, vid, idx);
		switch (ret) {
		case SD_RES_SUCCESS:
			http_response_header(req, CREATED);
			goto out;
		case SD_RES_OBJ_TAKEN:
			break;
		default:
			http_response_header(req, INTERNAL_SERVER_ERROR);
			goto out;
		}
	}
	/* no free space to create a object */
	http_response_header(req, SERVICE_UNAVAILABLE);
out:
	free(onode);
	free(data_buf);
	return ret;
}

static int kv_read_extent_onode(struct http_request *req,
				struct kv_onode *onode)
{
	struct onode_extent *ext;
	uint64_t oid, block, size, total_size, limit;
	uint32_t i;
	int ret;
	char *data_buf = NULL;

	data_buf = xmalloc(SD_DATA_OBJ_SIZE);

	total_size = onode->hdr.size;
	ext = onode->o_extent;
	for (i = 0; i < onode->hdr.nr_extent; i++) {
		limit = ext->count + ext->start;
		for (block = ext->start; block < limit; block++) {
			oid = vid_to_data_oid(onode->hdr.data_vid, block);
			if (total_size < SD_DATA_OBJ_SIZE)
				size = total_size;
			else
				size = SD_DATA_OBJ_SIZE;
			ret = sd_read_object(oid, data_buf, size, 0);
			if (ret != SD_RES_SUCCESS) {
				sd_err("Failed to read oid %lx", oid);
				goto out;
			}
			http_request_write(req, data_buf, size);
			total_size -= size;
			sd_debug("read extented block %lu, size %lu",
				 block, size);
		}
	}
out:
	free(data_buf);
	return ret;
}

static int do_kv_read_object(struct http_request *req, const char *obj_name,
			     struct kv_onode *onode, uint32_t vid, uint32_t idx)
{
	uint64_t oid = vid_to_data_oid(vid, idx);
	int ret;

	ret = sd_read_object(oid, (char *)onode, sizeof(*onode), 0);
	switch (ret) {
	case SD_RES_SUCCESS:
		break;
	case SD_RES_NO_OBJ:
		sd_info("object %s doesn't exist", obj_name);
		http_response_header(req, NOT_FOUND);
		return -1;
	default:
		sd_err("failed to read %s, %s", req->uri, sd_strerror(ret));
		http_response_header(req, INTERNAL_SERVER_ERROR);
		return -1;
	}

	if (strcmp(onode->hdr.name, obj_name) == 0) {
		http_response_header(req, OK);
		/* for inlined onode */
		if (onode->hdr.inlined)
			http_request_write(req, onode->data, onode->hdr.size);
		else {
			ret = kv_read_extent_onode(req, onode);
			if (ret) {
				sd_err("Failed to read extent onode");
				return -1;
			}
		}
	}

	return 0;
}

int kv_read_object(struct http_request *req, const char *account,
		   const char *bucket, const char *object)
{
	struct kv_onode *onode;
	int ret;
	uint64_t hval;
	uint32_t vid;
	char vdi_name[SD_MAX_VDI_LEN];

	snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s", account, bucket);
	ret = lookup_bucket(req, vdi_name, &vid);
	if (ret < 0)
		return ret;

	onode = xzalloc(sizeof(*onode));

	hval = sd_hash(object, strlen(object));
	for (int i = 0; i < MAX_DATA_OBJS; i++) {
		uint32_t idx = (hval + i) % MAX_DATA_OBJS;

		do_kv_read_object(req, object, onode, vid, idx);
		if (req->status != UNKNOWN) {
			free(onode);
			return 0;
		}
	}

	free(onode);

	http_response_header(req, NOT_FOUND);
	return -1;
}

static int do_kv_update_object(struct http_request *req, const char *obj_name,
			       struct kv_onode *obj, uint32_t vid,
			       uint32_t idx, size_t size)
{
	uint64_t oid = vid_to_data_oid(vid, idx);
	int ret;

	ret = sd_read_object(oid, (char *)&obj->hdr, sizeof(obj->hdr), 0);
	switch (ret) {
	case SD_RES_SUCCESS:
		break;
	case SD_RES_NO_VDI:
		sd_info("object %s doesn't exist", obj_name);
		http_response_header(req, NOT_FOUND);
		return -1;
	default:
		sd_err("failed to read %s, %s", req->uri, sd_strerror(ret));
		http_response_header(req, INTERNAL_SERVER_ERROR);
		return -1;
	}

	if (strcmp(obj->hdr.name, obj_name) == 0) {
		struct timeval tv;

		gettimeofday(&tv, NULL);
		obj->hdr.mtime = (uint64_t) tv.tv_sec << 32 | tv.tv_usec * 1000;
		obj->hdr.size = size;

		ret = sd_write_object(oid, (char *)obj,
				      sizeof(obj->hdr) + obj->hdr.size,
				      0, false);
		if (ret == SD_RES_SUCCESS)
			http_response_header(req, ACCEPTED);
		else {
			sd_err("failed to update object, %" PRIx64, oid);
			http_response_header(req, INTERNAL_SERVER_ERROR);
			return -1;
		}
	}

	return 0;
}

int kv_update_object(struct http_request *req, const char *bucket,
		     const char *object)
{
	struct kv_onode *obj;
	int ret;
	uint64_t hval;
	uint32_t vid;
	ssize_t size;

	ret = lookup_bucket(req, bucket, &vid);
	if (ret < 0)
		return ret;

	obj = xzalloc(sizeof(*obj));

	/* TODO: support multi parted object for large object */
	size = http_request_read(req, obj->data, sizeof(obj->data));
	if (size < 0) {
		sd_err("%s: bucket %s, object %s", sd_strerror(ret),
		       bucket, object);
		http_response_header(req, INTERNAL_SERVER_ERROR);
		return -1;
	}

	hval = sd_hash(object, strlen(object));
	for (int i = 0; i < MAX_DATA_OBJS; i++) {
		uint32_t idx = (hval + i) % MAX_DATA_OBJS;

		do_kv_update_object(req, object, obj, vid, idx, size);
		if (req->status != UNKNOWN) {
			free(obj);
			return 0;
		}
	}

	free(obj);

	http_response_header(req, NOT_FOUND);
	return -1;
}

static int do_kv_delete_object(struct http_request *req, const char *obj_name,
			       uint32_t vid, uint32_t idx)
{
	struct kv_onode *onode = NULL;
	struct onode_extent *ext = NULL;
	uint64_t oid = vid_to_data_oid(vid, idx);
	char name[SD_MAX_OBJECT_NAME];
	int ret = 0, len, i;

	ret = sd_read_object(oid, name, sizeof(name), 0);
	switch (ret) {
	case SD_RES_SUCCESS:
		break;
	case SD_RES_NO_OBJ:
		sd_info("object %s doesn't exist", obj_name);
		http_response_header(req, NOT_FOUND);
		return -1;
	default:
		sd_err("failed to read %s, %s", req->uri, sd_strerror(ret));
		http_response_header(req, INTERNAL_SERVER_ERROR);
		return -1;
	}

	if (strcmp(name, obj_name) == 0) {
		/* delete onode at first */
		memset(name, 0, sizeof(name));
		ret = sd_write_object(oid, name, sizeof(name), 0, false);
		if (ret == SD_RES_SUCCESS)
			http_response_header(req, NO_CONTENT);
		else {
			sd_err("failed to update object, %" PRIx64,
			       oid);
			http_response_header(req, INTERNAL_SERVER_ERROR);
			goto out;
		}
		/* then free data space */
		onode = xmalloc(sizeof(struct kv_onode_hdr));
		ret = sd_read_object(oid, (char *)onode,
				     sizeof(struct kv_onode_hdr), 0);
		if (ret != SD_RES_SUCCESS) {
			sd_err("failed to read onode hdr %" PRIx64, oid);
			goto out;
		}
		len = sizeof(struct onode_extent) * onode->hdr.nr_extent;
		ext = xmalloc(len);
		ret = sd_read_object(oid, (char *)ext, len,
				     sizeof(struct kv_onode_hdr));
		if (ret != SD_RES_SUCCESS) {
			sd_err("failed to read onode extent %" PRIx64, oid);
			goto out;
		}
		for (i = 0; i < onode->hdr.nr_extent; i++) {
			ret = oalloc_free(onode->hdr.data_vid, ext[i].start,
					  ext[i].count);
			if (ret != SD_RES_SUCCESS)
				sd_err("failed to free start %lu count %lu",
				       ext[i].start, ext[i].count);
		}
	}
out:
	free(ext);
	free(onode);
	return ret;
}

int kv_delete_object(struct http_request *req, const char *account,
		     const char *bucket, const char *object)
{
	int ret;
	uint64_t hval;
	uint32_t vid;
	char vdi_name[SD_MAX_VDI_LEN];

	snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s", account, bucket);
	ret = lookup_bucket(req, vdi_name, &vid);
	if (ret < 0)
		return ret;

	hval = sd_hash(object, strlen(object));
	for (int i = 0; i < MAX_DATA_OBJS; i++) {
		uint32_t idx = (hval + i) % MAX_DATA_OBJS;

		do_kv_delete_object(req, object, vid, idx);
		if (req->status != UNKNOWN)
			return 0;
	}

	http_response_header(req, NOT_FOUND);
	return -1;
}

int kv_list_objects(struct http_request *req, const char *account,
		    const char *bucket, list_object_cb cb, void *opaque)
{
	int ret;
	uint32_t vid;
	struct sd_inode *inode = NULL;
	char vdi_name[SD_MAX_VDI_LEN];

	snprintf(vdi_name, SD_MAX_VDI_LEN, "%s/%s", account, bucket);
	ret = lookup_bucket(req, vdi_name, &vid);
	if (ret < 0)
		goto out;

	inode = xmalloc(sizeof(*inode));
	ret = sd_read_object(vid_to_vdi_oid(vid), (char *)inode,
			  sizeof(struct sd_inode), 0);
	if (ret != SD_RES_SUCCESS) {
		sd_err("%s: bucket %s", sd_strerror(ret), bucket);
		http_response_header(req, INTERNAL_SERVER_ERROR);
		goto out;
	}

	struct list_objects_arg arg = {req, opaque, bucket, cb, 0};
	traverse_btree(sheep_bnode_reader, inode, list_objects_cb, &arg);
out:
	free(inode);
	return ret;
}
