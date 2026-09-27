// SPDX-License-Identifier: GPL-2.0-only OR Linux-OpenIB

/*
 * Copyright (c) 2026 Broadcom. All rights reserved. The term Broadcom
 * refers to Broadcom Inc. and/or its subsidiaries.
 */

/*
 * The job object of a reliable unordered transport.
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>

#include <rdma/ib_marshall.h>
#include <rdma/uverbs_std_types.h>

#include "rdma_core.h"
#include "restrack.h"
#include "uverbs.h"

static void ib_job_release(struct kref *ref)
{
	struct ib_job *job = container_of(ref, struct ib_job, ref);

	job->device->ops.dealloc_job(job);
	rdma_restrack_del(&job->res);
	kfree(job);
}

static int ib_job_fd_release(struct inode *inode, struct file *filp)
{
	kref_put(&((struct ib_job *)filp->private_data)->ref, ib_job_release);

	return 0;
}

static const struct file_operations ib_job_fops = {
	.owner = THIS_MODULE,
	.release = ib_job_fd_release,
};

static int UVERBS_HANDLER(UVERBS_METHOD_JOB_EXPORT)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_job *job =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_EXPORT_JOB_HANDLE);
	struct file *filp;
	int fd, ret;

	if (IS_ERR(job))
		return PTR_ERR(job);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	/* an exported job is an anonymous file holding one reference */
	kref_get(&job->ref);

	filp = anon_inode_getfile("[ib_job]", &ib_job_fops, job,
				  (O_RDWR | O_CLOEXEC));
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		goto err;
	}

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_EXPORT_JOB_FD, &fd,
			     sizeof(fd));
	if (ret) {
		fput(filp);
		put_unused_fd(fd);
		return ret;
	}

	fd_install(fd, filp);

	return 0;

err:
	kref_put(&job->ref, ib_job_release);
	put_unused_fd(fd);

	return ret;
}

static int UVERBS_HANDLER(UVERBS_METHOD_JOB_IMPORT)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj =
		uverbs_attr_get_uobject(attrs, UVERBS_ATTR_IMPORT_JOB_HANDLE);
	struct ib_device *ib_dev = attrs->context->device;
	struct ib_job *job;
	struct fd f;
	s32 fd;
	int ret;

	ret = uverbs_copy_from(&fd, attrs, UVERBS_ATTR_IMPORT_JOB_FD);
	if (ret)
		return ret;

	f = fdget(fd);
	if (!fd_file(f))
		return -EBADF;

	/* refuse anything that is not an exported job */
	if (fd_file(f)->f_op != &ib_job_fops) {
		fdput(f);
		return -EINVAL;
	}

	job = fd_file(f)->private_data;

	/* a handle names a job on the device it was allocated on */
	if (job->device != ib_dev) {
		fdput(f);
		return -EINVAL;
	}

	/* an imported job holds one reference */
	kref_get(&job->ref);
	fdput(f);

	uobj->object = job;

	return 0;
}

static int uverbs_free_job(struct ib_uobject *uobject,
			   enum rdma_remove_reason why,
			   struct uverbs_attr_bundle *attrs)
{
	struct ib_ujob_object *ujob =
		container_of(uobject, struct ib_ujob_object, uobject);
	struct ib_job *job = uobject->object;

	/* don't free if there are still references to this job */
	if (atomic_read(&ujob->refcnt))
		return -EBUSY;

	kref_put(&job->ref, ib_job_release);

	return 0;
}

static int UVERBS_HANDLER(UVERBS_METHOD_JOB_ALLOC)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj =
		uverbs_attr_get_uobject(attrs, UVERBS_ATTR_ALLOC_JOB_HANDLE);
	struct ib_device *ib_dev = attrs->context->device;
	struct ib_job_attr attr = {};
	struct ib_job *job;
	int ret;

	if (!ib_dev->ops.alloc_job)
		return -EOPNOTSUPP;

	/* A JobID names a domain across the fabric rather than anything
	 * local to this process, so claiming one is an administrative act.
	 */
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	ret = uverbs_copy_from(&attr.job_id, attrs, UVERBS_ATTR_ALLOC_JOB_ID);
	if (ret)
		return ret;

	/* The rest are optional and an absent attribute reads as -ENOENT
	 * which IS_UVERBS_COPY_ERR() checks.
	 */
	ret = uverbs_copy_from(&attr.max_addr_entries, attrs,
			       UVERBS_ATTR_ALLOC_JOB_MAX_ADDR_ENTRIES);
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	ret = uverbs_copy_from(&attr.flags, attrs,
			       UVERBS_ATTR_ALLOC_JOB_FLAGS);
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	ret = uverbs_copy_from(&attr.port_num, attrs,
			       UVERBS_ATTR_ALLOC_JOB_PORT_NUM);
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	ret = uverbs_copy_from(&attr.sgid_index, attrs,
			       UVERBS_ATTR_ALLOC_JOB_SGID_INDEX);
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	/* the port carries the job's traffic, so it has to be one we have */
	if (!attr.port_num)
		attr.port_num = rdma_start_port(ib_dev);
	if (!rdma_is_port_valid(ib_dev, attr.port_num))
		return -EINVAL;

	job = rdma_zalloc_drv_obj(ib_dev, ib_job);
	if (!job)
		return -ENOMEM;

	job->device = ib_dev;
	job->job_id = attr.job_id;
	kref_init(&job->ref);
	uobj->object = job;

	rdma_restrack_new(&job->res, RDMA_RESTRACK_JOB);
	rdma_restrack_set_name(&job->res, NULL);

	ret = ib_dev->ops.alloc_job(job, &attr, attrs);
	if (ret) {
		rdma_restrack_put(&job->res);
		kfree(job);
		return ret;
	}

	rdma_restrack_add(&job->res);

	return 0;
}

static int UVERBS_HANDLER(UVERBS_METHOD_JOB_QUERY)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_job *job =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_QUERY_JOB_HANDLE);
	struct ib_job_attr attr = {};
	int ret;

	if (!job->device->ops.query_job)
		return -EOPNOTSUPP;

	ret = job->device->ops.query_job(job, &attr);
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_JOB_ID, &attr.job_id,
			     sizeof(attr.job_id));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_JOB_MAX_ADDR_ENTRIES,
			     &attr.max_addr_entries,
			     sizeof(attr.max_addr_entries));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_JOB_FLAGS, &attr.flags,
			     sizeof(attr.flags));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_JOB_PORT_NUM,
			     &attr.port_num, sizeof(attr.port_num));
	if (ret)
		return ret;

	return uverbs_copy_to(attrs, UVERBS_ATTR_QUERY_JOB_SGID_INDEX,
			      &attr.sgid_index, sizeof(attr.sgid_index));
}

static void job_ah_attr_from_uverbs(struct ib_device *dev,
				    struct rdma_ah_attr *out,
				    struct ib_uverbs_ah_attr *in)
{
	memset(out, 0, sizeof(*out));
	out->type = rdma_ah_find_type(dev, in->port_num);

	if (in->is_global) {
		rdma_ah_set_grh(out, NULL, in->grh.flow_label,
				in->grh.sgid_index, in->grh.hop_limit,
				in->grh.traffic_class);
		rdma_ah_set_dgid_raw(out, in->grh.dgid);
	} else {
		rdma_ah_set_ah_flags(out, 0);
	}

	rdma_ah_set_dlid(out, in->dlid);
	rdma_ah_set_sl(out, in->sl);
	rdma_ah_set_path_bits(out, in->src_path_bits);
	rdma_ah_set_static_rate(out, in->static_rate);
	rdma_ah_set_port_num(out, in->port_num);
}

static int UVERBS_HANDLER(UVERBS_METHOD_JOB_ADDR_INSERT)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_job *job = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_JOB_ADDR_INSERT_HANDLE);
	struct ib_uverbs_ah_attr_ex uah;
	struct rdma_ah_attr_ex ah_attr = {};
	u32 index, flags = 0;
	int ret;

	if (IS_ERR(job))
		return PTR_ERR(job);

	if (!job->device->ops.job_addr_insert)
		return -EOPNOTSUPP;

	/* an address table entry names a peer on the fabric */
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	ret = uverbs_copy_from(&index, attrs,
			       UVERBS_ATTR_JOB_ADDR_INSERT_INDEX);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&uah, attrs,
			       UVERBS_ATTR_JOB_ADDR_INSERT_AH_ATTR_EX);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&flags, attrs,
			       UVERBS_ATTR_JOB_ADDR_INSERT_FLAGS);
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	job_ah_attr_from_uverbs(job->device, &ah_attr.ah_attr, &uah.ah_attr);
	ah_attr.remote_qpn = uah.remote_qpn;

	return job->device->ops.job_addr_insert(job, index, &ah_attr, flags);
}

static int UVERBS_HANDLER(UVERBS_METHOD_JOB_ADDR_REMOVE)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_job *job = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_JOB_ADDR_REMOVE_HANDLE);
	u32 index, flags = 0;
	int ret;

	if (IS_ERR(job))
		return PTR_ERR(job);

	if (!job->device->ops.job_addr_remove)
		return -EOPNOTSUPP;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	ret = uverbs_copy_from(&index, attrs,
			       UVERBS_ATTR_JOB_ADDR_REMOVE_INDEX);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&flags, attrs,
			       UVERBS_ATTR_JOB_ADDR_REMOVE_FLAGS);
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	return job->device->ops.job_addr_remove(job, index, flags);
}

static int UVERBS_HANDLER(UVERBS_METHOD_JOB_ADDR_QUERY)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_job *job = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_JOB_ADDR_QUERY_HANDLE);
	struct ib_uverbs_ah_attr_ex uah = {};
	struct rdma_ah_attr_ex ah_attr = {};
	u32 index, flags = 0;
	int ret;

	if (IS_ERR(job))
		return PTR_ERR(job);

	if (!job->device->ops.job_addr_query)
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&index, attrs,
			       UVERBS_ATTR_JOB_ADDR_QUERY_INDEX);
	if (ret)
		return ret;

	ret = uverbs_copy_from(&flags, attrs,
			       UVERBS_ATTR_JOB_ADDR_QUERY_FLAGS);
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	ret = job->device->ops.job_addr_query(job, index, &ah_attr, flags);
	if (ret)
		return ret;

	ib_copy_ah_attr_to_user(job->device, &uah.ah_attr, &ah_attr.ah_attr);
	uah.remote_qpn = ah_attr.remote_qpn;

	return uverbs_copy_to(attrs, UVERBS_ATTR_JOB_ADDR_QUERY_AH_ATTR_EX,
			      &uah, sizeof(uah));
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_JOB_ADDR_INSERT,
	UVERBS_ATTR_IDR(UVERBS_ATTR_JOB_ADDR_INSERT_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_JOB_ADDR_INSERT_INDEX,
			   UVERBS_ATTR_TYPE(u32),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_JOB_ADDR_INSERT_AH_ATTR_EX,
			   UVERBS_ATTR_STRUCT(struct ib_uverbs_ah_attr_ex,
					      remote_qpn),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_JOB_ADDR_INSERT_FLAGS,
			   UVERBS_ATTR_TYPE(u32),
			   UA_OPTIONAL));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_JOB_ADDR_REMOVE,
	UVERBS_ATTR_IDR(UVERBS_ATTR_JOB_ADDR_REMOVE_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_JOB_ADDR_REMOVE_INDEX,
			   UVERBS_ATTR_TYPE(u32),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_JOB_ADDR_REMOVE_FLAGS,
			   UVERBS_ATTR_TYPE(u32),
			   UA_OPTIONAL));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_JOB_ADDR_QUERY,
	UVERBS_ATTR_IDR(UVERBS_ATTR_JOB_ADDR_QUERY_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_JOB_ADDR_QUERY_INDEX,
			   UVERBS_ATTR_TYPE(u32),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_JOB_ADDR_QUERY_AH_ATTR_EX,
			    UVERBS_ATTR_STRUCT(struct ib_uverbs_ah_attr_ex,
					       remote_qpn),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_JOB_ADDR_QUERY_FLAGS,
			   UVERBS_ATTR_TYPE(u32),
			   UA_OPTIONAL));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_JOB_ALLOC,
	UVERBS_ATTR_IDR(UVERBS_ATTR_ALLOC_JOB_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_NEW,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_ALLOC_JOB_ID,
			   UVERBS_ATTR_TYPE(u32),
			   UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_ALLOC_JOB_MAX_ADDR_ENTRIES,
			   UVERBS_ATTR_TYPE(u32),
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_ALLOC_JOB_FLAGS,
			   UVERBS_ATTR_TYPE(u32),
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_ALLOC_JOB_PORT_NUM,
			   UVERBS_ATTR_TYPE(u8),
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_ALLOC_JOB_SGID_INDEX,
			   UVERBS_ATTR_TYPE(u8),
			   UA_OPTIONAL));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_JOB_EXPORT,
	UVERBS_ATTR_IDR(UVERBS_ATTR_EXPORT_JOB_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_EXPORT_JOB_FD,
			    UVERBS_ATTR_TYPE(s32),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_JOB_IMPORT,
	UVERBS_ATTR_IDR(UVERBS_ATTR_IMPORT_JOB_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_NEW,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_IMPORT_JOB_FD,
			   UVERBS_ATTR_TYPE(s32),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD_DESTROY(
	UVERBS_METHOD_JOB_DEALLOC,
	UVERBS_ATTR_IDR(UVERBS_ATTR_DEALLOC_JOB_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_DESTROY,
			UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_JOB_QUERY,
	UVERBS_ATTR_IDR(UVERBS_ATTR_QUERY_JOB_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_JOB_ID,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_JOB_MAX_ADDR_ENTRIES,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_JOB_FLAGS,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_JOB_PORT_NUM,
			    UVERBS_ATTR_TYPE(u8),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_QUERY_JOB_SGID_INDEX,
			    UVERBS_ATTR_TYPE(u8),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(UVERBS_OBJECT_JOB,
			    UVERBS_TYPE_ALLOC_IDR_SZ(
				    sizeof(struct ib_ujob_object),
				    uverbs_free_job),
			    &UVERBS_METHOD(UVERBS_METHOD_JOB_ALLOC),
			    &UVERBS_METHOD(UVERBS_METHOD_JOB_DEALLOC),
			    &UVERBS_METHOD(UVERBS_METHOD_JOB_QUERY),
			    &UVERBS_METHOD(UVERBS_METHOD_JOB_ADDR_INSERT),
			    &UVERBS_METHOD(UVERBS_METHOD_JOB_ADDR_REMOVE),
			    &UVERBS_METHOD(UVERBS_METHOD_JOB_ADDR_QUERY),
			    &UVERBS_METHOD(UVERBS_METHOD_JOB_EXPORT),
			    &UVERBS_METHOD(UVERBS_METHOD_JOB_IMPORT));

const struct uapi_definition uverbs_def_obj_job[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(UVERBS_OBJECT_JOB,
				      UAPI_DEF_OBJ_NEEDS_FN(dealloc_job)),
	{}
};

static int uverbs_free_jkey(struct ib_uobject *uobject,
			    enum rdma_remove_reason why,
			    struct uverbs_attr_bundle *attrs)
{
	struct ib_jkey *jkey = uobject->object;
	int ret;

	/* queue pairs created with this key still hold it */
	if (atomic_read(&jkey->usecnt))
		return -EBUSY;

	ret = jkey->device->ops.destroy_jkey(jkey, attrs);
	if (ret)
		return ret;

	atomic_dec(&container_of(jkey->job_uobject, struct ib_ujob_object,
				 uobject)->refcnt);
	kref_put(&jkey->job->ref, ib_job_release);
	rdma_restrack_del(&jkey->res);
	kfree(jkey);

	return 0;
}

static int UVERBS_HANDLER(UVERBS_METHOD_JKEY_CREATE)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj =
		uverbs_attr_get_uobject(attrs, UVERBS_ATTR_CREATE_JKEY_HANDLE);
	struct ib_uobject *job_uobj = uverbs_attr_get_uobject(
		attrs, UVERBS_ATTR_CREATE_JKEY_JOB_HANDLE);
	struct ib_pd *pd =
		uverbs_attr_get_obj(attrs, UVERBS_ATTR_CREATE_JKEY_PD_HANDLE);
	struct ib_job *job;
	struct ib_device *ib_dev = attrs->context->device;
	struct ib_jkey *jkey;
	u32 flags = 0;
	int ret;

	if (!ib_dev->ops.create_jkey)
		return -EOPNOTSUPP;

	if (IS_ERR(job_uobj))
		return PTR_ERR(job_uobj);

	if (IS_ERR(pd))
		return PTR_ERR(pd);

	job = job_uobj->object;

	ret = uverbs_copy_from(&flags, attrs, UVERBS_ATTR_CREATE_JKEY_FLAGS);
	if (IS_UVERBS_COPY_ERR(ret))
		return ret;

	jkey = rdma_zalloc_drv_obj(ib_dev, ib_jkey);
	if (!jkey)
		return -ENOMEM;

	jkey->device = ib_dev;
	jkey->uobject = uobj;
	jkey->job = job;
	jkey->job_uobject = job_uobj;
	jkey->pd = pd;
	atomic_set(&jkey->usecnt, 0);
	uobj->object = jkey;

	rdma_restrack_new(&jkey->res, RDMA_RESTRACK_JKEY);
	rdma_restrack_set_name(&jkey->res, NULL);

	ret = ib_dev->ops.create_jkey(jkey, flags, attrs);
	if (ret) {
		rdma_restrack_put(&jkey->res);
		kfree(jkey);
		return ret;
	}

	rdma_restrack_add(&jkey->res);

	/* from here the job cannot be freed while this key references it */
	kref_get(&job->ref);
	atomic_inc(&container_of(job_uobj, struct ib_ujob_object,
				 uobject)->refcnt);

	ret = uverbs_copy_to(attrs, UVERBS_ATTR_CREATE_JKEY_JKEY, &jkey->jkey,
			     sizeof(jkey->jkey));
	if (ret) {
		atomic_dec(&container_of(job_uobj, struct ib_ujob_object,
					 uobject)->refcnt);
		kref_put(&job->ref, ib_job_release);
		ib_dev->ops.destroy_jkey(jkey, attrs);
		kfree(jkey);
		return ret;
	}

	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_JKEY_CREATE,
	UVERBS_ATTR_IDR(UVERBS_ATTR_CREATE_JKEY_HANDLE,
			UVERBS_OBJECT_JKEY,
			UVERBS_ACCESS_NEW,
			UA_MANDATORY),
	UVERBS_ATTR_IDR(UVERBS_ATTR_CREATE_JKEY_JOB_HANDLE,
			UVERBS_OBJECT_JOB,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_IDR(UVERBS_ATTR_CREATE_JKEY_PD_HANDLE,
			UVERBS_OBJECT_PD,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_CREATE_JKEY_FLAGS,
			   UVERBS_ATTR_TYPE(u32),
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_CREATE_JKEY_JKEY,
			    UVERBS_ATTR_TYPE(u32),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD_DESTROY(
	UVERBS_METHOD_JKEY_DESTROY,
	UVERBS_ATTR_IDR(UVERBS_ATTR_DESTROY_JKEY_HANDLE,
			UVERBS_OBJECT_JKEY,
			UVERBS_ACCESS_DESTROY,
			UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(UVERBS_OBJECT_JKEY,
			    UVERBS_TYPE_ALLOC_IDR(uverbs_free_jkey),
			    &UVERBS_METHOD(UVERBS_METHOD_JKEY_CREATE),
			    &UVERBS_METHOD(UVERBS_METHOD_JKEY_DESTROY));

const struct uapi_definition uverbs_def_obj_jkey[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(UVERBS_OBJECT_JKEY,
				      UAPI_DEF_OBJ_NEEDS_FN(destroy_jkey)),
	{}
};
