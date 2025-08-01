// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2011 Novell Inc.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fileattr.h>
#include <linux/splice.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <linux/cred.h>
#include <linux/namei.h>
#include <linux/ratelimit.h>
#include <linux/exportfs.h>
#include "overlayfs.h"

#define OVL_COPY_UP_CHUNK_SIZE (1 << 20)

static int ovl_ccup_set(const char *buf, const struct kernel_param *param)
{
	pr_warn("\"check_copy_up\" module option is obsolete\n");
	return 0;
}

static int ovl_ccup_get(char *buf, const struct kernel_param *param)
{
	return sprintf(buf, "N\n");
}

module_param_call(check_copy_up, ovl_ccup_set, ovl_ccup_get, NULL, 0644);
MODULE_PARM_DESC(check_copy_up, "Obsolete; does nothing");

static bool ovl_must_copy_xattr(const char *name)
{
	return !strcmp(name, XATTR_POSIX_ACL_ACCESS) ||
	       !strcmp(name, XATTR_POSIX_ACL_DEFAULT) ||
	       !strncmp(name, XATTR_SECURITY_PREFIX, XATTR_SECURITY_PREFIX_LEN);
}

static int ovl_copy_acl(struct ovl_fs *ofs, const struct path *path,
			struct dentry *dentry, const char *acl_name)
{
	int err;
	struct posix_acl *clone, *real_acl = NULL;

	real_acl = ovl_get_acl_path(path, acl_name, false);
	if (!real_acl)
		return 0;

	if (IS_ERR(real_acl)) {
		err = PTR_ERR(real_acl);
		if (err == -ENODATA || err == -EOPNOTSUPP)
			return 0;
		return err;
	}

	clone = posix_acl_clone(real_acl, GFP_KERNEL);
	posix_acl_release(real_acl); /* release original acl */
	if (!clone)
		return -ENOMEM;

	err = ovl_do_set_acl(ofs, dentry, acl_name, clone);

	/* release cloned acl */
	posix_acl_release(clone);
	return err;
}

int ovl_copy_xattr(struct super_block *sb, const struct path *oldpath, struct dentry *new)
{
	struct dentry *old = oldpath->dentry;
	ssize_t list_size, size, value_size = 0;
	char *buf, *name, *value = NULL;
	int error = 0;
	size_t slen;

	if (!old->d_inode->i_op->listxattr || !new->d_inode->i_op->listxattr)
		return 0;

	list_size = vfs_listxattr(old, NULL, 0);
	if (list_size <= 0) {
		if (list_size == -EOPNOTSUPP)
			return 0;
		return list_size;
	}

	buf = kvzalloc(list_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	list_size = vfs_listxattr(old, buf, list_size);
	if (list_size <= 0) {
		error = list_size;
		goto out;
	}

	for (name = buf; list_size; name += slen) {
		slen = strnlen(name, list_size) + 1;

		/* underlying fs providing us with an broken xattr list? */
		if (WARN_ON(slen > list_size)) {
			error = -EIO;
			break;
		}
		list_size -= slen;

		if (ovl_is_private_xattr(sb, name))
			continue;

		error = security_inode_copy_up_xattr(old, name);
		if (error == -ECANCELED) {
			error = 0;
			continue; /* Discard */
		}
		if (error < 0 && error != -EOPNOTSUPP)
			break;

		if (is_posix_acl_xattr(name)) {
			error = ovl_copy_acl(OVL_FS(sb), oldpath, new, name);
			if (!error)
				continue;
			/* POSIX ACLs must be copied. */
			break;
		}

retry:
		size = ovl_do_getxattr(oldpath, name, value, value_size);
		if (size == -ERANGE)
			size = ovl_do_getxattr(oldpath, name, NULL, 0);

		if (size < 0) {
			error = size;
			break;
		}

		if (size > value_size) {
			void *new;

			new = kvmalloc(size, GFP_KERNEL);
			if (!new) {
				error = -ENOMEM;
				break;
			}
			kvfree(value);
			value = new;
			value_size = size;
			goto retry;
		}

		error = ovl_do_setxattr(OVL_FS(sb), new, name, value, size, 0);
		if (error) {
			if (error != -EOPNOTSUPP || ovl_must_copy_xattr(name))
				break;

			/* Ignore failure to copy unknown xattrs */
			error = 0;
		}
	}
	kvfree(value);
out:
	kvfree(buf);
	return error;
}

static int ovl_copy_fileattr(struct inode *inode, const struct path *old,
			     const struct path *new)
{
	struct file_kattr oldfa = { .flags_valid = true };
	struct file_kattr newfa = { .flags_valid = true };
	int err;

	err = ovl_real_fileattr_get(old, &oldfa);
	if (err) {
		/* Ntfs-3g returns -EINVAL for "no fileattr support" */
		if (err == -EOPNOTSUPP || err == -EINVAL)
			return 0;
		pr_warn("failed to retrieve lower fileattr (%pd2, err=%i)\n",
			old->dentry, err);
		return err;
	}

	/*
	 * We cannot set immutable and append-only flags on upper inode,
	 * because we would not be able to link upper inode to upper dir
	 * not set overlay private xattr on upper inode.
	 * Store these flags in overlay.protattr xattr instead.
	 */
	if (oldfa.flags & OVL_PROT_FS_FLAGS_MASK) {
		err = ovl_set_protattr(inode, new->dentry, &oldfa);
		if (err == -EPERM)
			pr_warn_once("copying fileattr: no xattr on upper\n");
		else if (err)
			return err;
	}

	/* Don't bother copying flags if none are set */
	if (!(oldfa.flags & OVL_COPY_FS_FLAGS_MASK))
		return 0;

	err = ovl_real_fileattr_get(new, &newfa);
	if (err) {
		/*
		 * Returning an error if upper doesn't support fileattr will
		 * result in a regression, so revert to the old behavior.
		 */
		if (err == -ENOTTY || err == -EINVAL) {
			pr_warn_once("copying fileattr: no support on upper\n");
			return 0;
		}
		pr_warn("failed to retrieve upper fileattr (%pd2, err=%i)\n",
			new->dentry, err);
		return err;
	}

	BUILD_BUG_ON(OVL_COPY_FS_FLAGS_MASK & ~FS_COMMON_FL);
	newfa.flags &= ~OVL_COPY_FS_FLAGS_MASK;
	newfa.flags |= (oldfa.flags & OVL_COPY_FS_FLAGS_MASK);

	BUILD_BUG_ON(OVL_COPY_FSX_FLAGS_MASK & ~FS_XFLAG_COMMON);
	newfa.fsx_xflags &= ~OVL_COPY_FSX_FLAGS_MASK;
	newfa.fsx_xflags |= (oldfa.fsx_xflags & OVL_COPY_FSX_FLAGS_MASK);

	return ovl_real_fileattr_set(new, &newfa);
}

static int ovl_verify_area(loff_t pos, loff_t pos2, loff_t len, loff_t totlen)
{
	loff_t tmp;

	if (pos != pos2)
		return -EIO;
	if (pos < 0 || len < 0 || totlen < 0)
		return -EIO;
	if (check_add_overflow(pos, len, &tmp))
		return -EIO;
	return 0;
}

static int ovl_sync_file(struct path *path)
{
	struct file *new_file;
	int err;

	new_file = ovl_path_open(path, O_LARGEFILE | O_RDONLY);
	if (IS_ERR(new_file))
		return PTR_ERR(new_file);

	err = vfs_fsync(new_file, 0);
	fput(new_file);

	return err;
}

static int ovl_copy_up_file(struct ovl_fs *ofs, struct dentry *dentry,
			    struct file *new_file, loff_t len,
			    bool datasync)
{
	struct path datapath;
	struct file *old_file;
	loff_t old_pos = 0;
	loff_t new_pos = 0;
	loff_t cloned;
	loff_t data_pos = -1;
	loff_t hole_len;
	bool skip_hole = false;
	int error = 0;

	ovl_path_lowerdata(dentry, &datapath);
	if (WARN_ON_ONCE(datapath.dentry == NULL) ||
	    WARN_ON_ONCE(len < 0))
		return -EIO;

	old_file = ovl_path_open(&datapath, O_LARGEFILE | O_RDONLY);
	if (IS_ERR(old_file))
		return PTR_ERR(old_file);

	/* Try to use clone_file_range to clone up within the same fs */
	cloned = vfs_clone_file_range(old_file, 0, new_file, 0, len, 0);
	if (cloned == len)
		goto out_fput;

	/* Couldn't clone, so now we try to copy the data */
	error = rw_verify_area(READ, old_file, &old_pos, len);
	if (!error)
		error = rw_verify_area(WRITE, new_file, &new_pos, len);
	if (error)
		goto out_fput;

	/* Check if lower fs supports seek operation */
	if (old_file->f_mode & FMODE_LSEEK)
		skip_hole = true;

	while (len) {
		size_t this_len = OVL_COPY_UP_CHUNK_SIZE;
		ssize_t bytes;

		if (len < this_len)
			this_len = len;

		if (signal_pending_state(TASK_KILLABLE, current)) {
			error = -EINTR;
			break;
		}

		/*
		 * Fill zero for hole will cost unnecessary disk space
		 * and meanwhile slow down the copy-up speed, so we do
		 * an optimization for hole during copy-up, it relies
		 * on SEEK_DATA implementation in lower fs so if lower
		 * fs does not support it, copy-up will behave as before.
		 *
		 * Detail logic of hole detection as below:
		 * When we detect next data position is larger than current
		 * position we will skip that hole, otherwise we copy
		 * data in the size of OVL_COPY_UP_CHUNK_SIZE. Actually,
		 * it may not recognize all kind of holes and sometimes
		 * only skips partial of hole area. However, it will be
		 * enough for most of the use cases.
		 *
		 * We do not hold upper sb_writers throughout the loop to avert
		 * lockdep warning with llseek of lower file in nested overlay:
		 * - upper sb_writers
		 * -- lower ovl_inode_lock (ovl_llseek)
		 */
		if (skip_hole && data_pos < old_pos) {
			data_pos = vfs_llseek(old_file, old_pos, SEEK_DATA);
			if (data_pos > old_pos) {
				hole_len = data_pos - old_pos;
				len -= hole_len;
				old_pos = new_pos = data_pos;
				continue;
			} else if (data_pos == -ENXIO) {
				break;
			} else if (data_pos < 0) {
				skip_hole = false;
			}
		}

		error = ovl_verify_area(old_pos, new_pos, this_len, len);
		if (error)
			break;

		bytes = do_splice_direct(old_file, &old_pos,
					 new_file, &new_pos,
					 this_len, SPLICE_F_MOVE);
		if (bytes <= 0) {
			error = bytes;
			break;
		}
		WARN_ON(old_pos != new_pos);

		len -= bytes;
	}
	/* call fsync once, either now or later along with metadata */
	if (!error && ovl_should_sync(ofs) && datasync)
		error = vfs_fsync(new_file, 0);
out_fput:
	fput(old_file);
	return error;
}

static int ovl_set_size(struct ovl_fs *ofs,
			struct dentry *upperdentry, struct kstat *stat)
{
	struct iattr attr = {
		.ia_valid = ATTR_SIZE,
		.ia_size = stat->size,
	};

	return ovl_do_notify_change(ofs, upperdentry, &attr);
}

static int ovl_set_timestamps(struct ovl_fs *ofs, struct dentry *upperdentry,
			      struct kstat *stat)
{
	struct iattr attr = {
		.ia_valid =
		     ATTR_ATIME | ATTR_MTIME | ATTR_ATIME_SET | ATTR_MTIME_SET | ATTR_CTIME,
		.ia_atime = stat->atime,
		.ia_mtime = stat->mtime,
	};

	return ovl_do_notify_change(ofs, upperdentry, &attr);
}

int ovl_set_attr(struct ovl_fs *ofs, struct dentry *upperdentry,
		 struct kstat *stat)
{
	int err = 0;

	if (!S_ISLNK(stat->mode)) {
		struct iattr attr = {
			.ia_valid = ATTR_MODE,
			.ia_mode = stat->mode,
		};
		err = ovl_do_notify_change(ofs, upperdentry, &attr);
	}
	if (!err) {
		struct iattr attr = {
			.ia_valid = ATTR_UID | ATTR_GID,
			.ia_vfsuid = VFSUIDT_INIT(stat->uid),
			.ia_vfsgid = VFSGIDT_INIT(stat->gid),
		};
		err = ovl_do_notify_change(ofs, upperdentry, &attr);
	}
	if (!err)
		ovl_set_timestamps(ofs, upperdentry, stat);

	return err;
}

struct ovl_fh *ovl_encode_real_fh(struct ovl_fs *ofs, struct inode *realinode,
				  bool is_upper)
{
	struct ovl_fh *fh;
	int fh_type, dwords;
	int buflen = MAX_HANDLE_SZ;
	uuid_t *uuid = &realinode->i_sb->s_uuid;
	int err;

	/* Make sure the real fid stays 32bit aligned */
	BUILD_BUG_ON(OVL_FH_FID_OFFSET % 4);
	BUILD_BUG_ON(MAX_HANDLE_SZ + OVL_FH_FID_OFFSET > 255);

	fh = kzalloc(buflen + OVL_FH_FID_OFFSET, GFP_KERNEL);
	if (!fh)
		return ERR_PTR(-ENOMEM);

	/*
	 * We encode a non-connectable file handle for non-dir, because we
	 * only need to find the lower inode number and we don't want to pay
	 * the price or reconnecting the dentry.
	 */
	dwords = buflen >> 2;
	fh_type = exportfs_encode_inode_fh(realinode, (void *)fh->fb.fid,
					   &dwords, NULL, 0);
	buflen = (dwords << 2);

	err = -EIO;
	if (fh_type < 0 || fh_type == FILEID_INVALID ||
	    WARN_ON(buflen > MAX_HANDLE_SZ))
		goto out_err;

	fh->fb.version = OVL_FH_VERSION;
	fh->fb.magic = OVL_FH_MAGIC;
	fh->fb.type = fh_type;
	fh->fb.flags = OVL_FH_FLAG_CPU_ENDIAN;
	/*
	 * When we will want to decode an overlay dentry from this handle
	 * and all layers are on the same fs, if we get a disconncted real
	 * dentry when we decode fid, the only way to tell if we should assign
	 * it to upperdentry or to lowerstack is by checking this flag.
	 */
	if (is_upper)
		fh->fb.flags |= OVL_FH_FLAG_PATH_UPPER;
	fh->fb.len = sizeof(fh->fb) + buflen;
	if (ovl_origin_uuid(ofs))
		fh->fb.uuid = *uuid;

	return fh;

out_err:
	kfree(fh);
	return ERR_PTR(err);
}

struct ovl_fh *ovl_get_origin_fh(struct ovl_fs *ofs, struct dentry *origin)
{
	/*
	 * When lower layer doesn't support export operations store a 'null' fh,
	 * so we can use the overlay.origin xattr to distignuish between a copy
	 * up and a pure upper inode.
	 */
	if (!ovl_can_decode_fh(origin->d_sb))
		return NULL;

	return ovl_encode_real_fh(ofs, d_inode(origin), false);
}

int ovl_set_origin_fh(struct ovl_fs *ofs, const struct ovl_fh *fh,
		      struct dentry *upper)
{
	int err;

	/*
	 * Do not fail when upper doesn't support xattrs.
	 */
	err = ovl_check_setxattr(ofs, upper, OVL_XATTR_ORIGIN, fh->buf,
				 fh ? fh->fb.len : 0, 0);

	/* Ignore -EPERM from setting "user.*" on symlink/special */
	return err == -EPERM ? 0 : err;
}

/* Store file handle of @upper dir in @index dir entry */
static int ovl_set_upper_fh(struct ovl_fs *ofs, struct dentry *upper,
			    struct dentry *index)
{
	const struct ovl_fh *fh;
	int err;

	fh = ovl_encode_real_fh(ofs, d_inode(upper), true);
	if (IS_ERR(fh))
		return PTR_ERR(fh);

	err = ovl_setxattr(ofs, index, OVL_XATTR_UPPER, fh->buf, fh->fb.len);

	kfree(fh);
	return err;
}

/*
 * Create and install index entry.
 */
static int ovl_create_index(struct dentry *dentry, const struct ovl_fh *fh,
			    struct dentry *upper)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	struct dentry *indexdir = ovl_indexdir(dentry->d_sb);
	struct dentry *index = NULL;
	struct dentry *temp = NULL;
	struct qstr name = { };
	int err;

	/*
	 * For now this is only used for creating index entry for directories,
	 * because non-dir are copied up directly to index and then hardlinked
	 * to upper dir.
	 *
	 * TODO: implement create index for non-dir, so we can call it when
	 * encoding file handle for non-dir in case index does not exist.
	 */
	if (WARN_ON(!d_is_dir(dentry)))
		return -EIO;

	/* Directory not expected to be indexed before copy up */
	if (WARN_ON(ovl_test_flag(OVL_INDEX, d_inode(dentry))))
		return -EIO;

	err = ovl_get_index_name_fh(fh, &name);
	if (err)
		return err;

	temp = ovl_create_temp(ofs, indexdir, OVL_CATTR(S_IFDIR | 0));
	err = PTR_ERR(temp);
	if (IS_ERR(temp))
		goto free_name;

	err = ovl_set_upper_fh(ofs, upper, temp);
	if (err)
		goto out;

	err = ovl_parent_lock(indexdir, temp);
	if (err)
		goto out;
	index = ovl_lookup_upper(ofs, name.name, indexdir, name.len);
	if (IS_ERR(index)) {
		err = PTR_ERR(index);
	} else {
		err = ovl_do_rename(ofs, indexdir, temp, indexdir, index, 0);
		dput(index);
	}
	ovl_parent_unlock(indexdir);
out:
	if (err)
		ovl_cleanup(ofs, indexdir, temp);
	dput(temp);
free_name:
	kfree(name.name);
	return err;
}

struct ovl_copy_up_ctx {
	struct dentry *parent;
	struct dentry *dentry;
	struct path lowerpath;
	struct kstat stat;
	struct kstat pstat;
	const char *link;
	struct dentry *destdir;
	struct qstr destname;
	struct dentry *workdir;
	const struct ovl_fh *origin_fh;
	bool origin;
	bool indexed;
	bool metacopy;
	bool metacopy_digest;
	bool metadata_fsync;
};

static int ovl_link_up(struct ovl_copy_up_ctx *c)
{
	int err;
	struct dentry *upper;
	struct dentry *upperdir = ovl_dentry_upper(c->parent);
	struct ovl_fs *ofs = OVL_FS(c->dentry->d_sb);
	struct inode *udir = d_inode(upperdir);

	ovl_start_write(c->dentry);

	/* Mark parent "impure" because it may now contain non-pure upper */
	err = ovl_set_impure(c->parent, upperdir);
	if (err)
		goto out;

	err = ovl_set_nlink_lower(c->dentry);
	if (err)
		goto out;

	inode_lock_nested(udir, I_MUTEX_PARENT);
	upper = ovl_lookup_upper(ofs, c->dentry->d_name.name, upperdir,
				 c->dentry->d_name.len);
	err = PTR_ERR(upper);
	if (!IS_ERR(upper)) {
		err = ovl_do_link(ofs, ovl_dentry_upper(c->dentry), udir, upper);

		if (!err) {
			/* Restore timestamps on parent (best effort) */
			ovl_set_timestamps(ofs, upperdir, &c->pstat);
			ovl_dentry_set_upper_alias(c->dentry);
			ovl_dentry_update_reval(c->dentry, upper);
		}
		dput(upper);
	}
	inode_unlock(udir);
	if (err)
		goto out;

	err = ovl_set_nlink_upper(c->dentry);

out:
	ovl_end_write(c->dentry);
	return err;
}

static int ovl_copy_up_data(struct ovl_copy_up_ctx *c, const struct path *temp)
{
	struct ovl_fs *ofs = OVL_FS(c->dentry->d_sb);
	struct file *new_file;
	int err;

	if (!S_ISREG(c->stat.mode) || c->metacopy || !c->stat.size)
		return 0;

	new_file = ovl_path_open(temp, O_LARGEFILE | O_WRONLY);
	if (IS_ERR(new_file))
		return PTR_ERR(new_file);

	err = ovl_copy_up_file(ofs, c->dentry, new_file, c->stat.size,
			       !c->metadata_fsync);
	fput(new_file);

	return err;
}

static int ovl_copy_up_metadata(struct ovl_copy_up_ctx *c, struct dentry *temp)
{
	struct ovl_fs *ofs = OVL_FS(c->dentry->d_sb);
	struct inode *inode = d_inode(c->dentry);
	struct path upperpath = { .mnt = ovl_upper_mnt(ofs), .dentry = temp };
	int err;

	err = ovl_copy_xattr(c->dentry->d_sb, &c->lowerpath, temp);
	if (err)
		return err;

	if (inode->i_flags & OVL_COPY_I_FLAGS_MASK &&
	    (S_ISREG(c->stat.mode) || S_ISDIR(c->stat.mode))) {
		/*
		 * Copy the fileattr inode flags that are the source of already
		 * copied i_flags
		 */
		err = ovl_copy_fileattr(inode, &c->lowerpath, &upperpath);
		if (err)
			return err;
	}

	/*
	 * Store identifier of lower inode in upper inode xattr to
	 * allow lookup of the copy up origin inode.
	 *
	 * Don't set origin when we are breaking the association with a lower
	 * hard link.
	 */
	if (c->origin) {
		err = ovl_set_origin_fh(ofs, c->origin_fh, temp);
		if (err)
			return err;
	}

	if (c->metacopy) {
		struct path lowerdatapath;
		struct ovl_metacopy metacopy_data = OVL_METACOPY_INIT;

		ovl_path_lowerdata(c->dentry, &lowerdatapath);
		if (WARN_ON_ONCE(lowerdatapath.dentry == NULL))
			return -EIO;
		err = ovl_get_verity_digest(ofs, &lowerdatapath, &metacopy_data);
		if (err)
			return err;

		if (metacopy_data.digest_algo)
			c->metacopy_digest = true;

		err = ovl_set_metacopy_xattr(ofs, temp, &metacopy_data);
		if (err)
			return err;
	}

	inode_lock(temp->d_inode);
	if (S_ISREG(c->stat.mode))
		err = ovl_set_size(ofs, temp, &c->stat);
	if (!err)
		err = ovl_set_attr(ofs, temp, &c->stat);
	inode_unlock(temp->d_inode);

	/* fsync metadata before moving it into upper dir */
	if (!err && ovl_should_sync(ofs) && c->metadata_fsync)
		err = ovl_sync_file(&upperpath);

	return err;
}

struct ovl_cu_creds {
	const struct cred *old;
	struct cred *new;
};

static int ovl_prep_cu_creds(struct dentry *dentry, struct ovl_cu_creds *cc)
{
	int err;

	cc->old = cc->new = NULL;
	err = security_inode_copy_up(dentry, &cc->new);
	if (err < 0)
		return err;

	if (cc->new)
		cc->old = override_creds(cc->new);

	return 0;
}

static void ovl_revert_cu_creds(struct ovl_cu_creds *cc)
{
	if (cc->new) {
		revert_creds(cc->old);
		put_cred(cc->new);
	}
}

/*
 * Copyup using workdir to prepare temp file.  Used when copying up directories,
 * special files or when upper fs doesn't support O_TMPFILE.
 */
static int ovl_copy_up_workdir(struct ovl_copy_up_ctx *c)
{
	struct ovl_fs *ofs = OVL_FS(c->dentry->d_sb);
	struct inode *inode;
	struct path path = { .mnt = ovl_upper_mnt(ofs) };
	struct dentry *temp, *upper, *trap;
	struct ovl_cu_creds cc;
	int err;
	struct ovl_cattr cattr = {
		/* Can't properly set mode on creation because of the umask */
		.mode = c->stat.mode & S_IFMT,
		.rdev = c->stat.rdev,
		.link = c->link
	};

	err = ovl_prep_cu_creds(c->dentry, &cc);
	if (err)
		return err;

	ovl_start_write(c->dentry);
	temp = ovl_create_temp(ofs, c->workdir, &cattr);
	ovl_end_write(c->dentry);
	ovl_revert_cu_creds(&cc);

	if (IS_ERR(temp))
		return PTR_ERR(temp);

	/*
	 * Copy up data first and then xattrs. Writing data after
	 * xattrs will remove security.capability xattr automatically.
	 */
	path.dentry = temp;
	err = ovl_copy_up_data(c, &path);
	ovl_start_write(c->dentry);
	if (err)
		goto cleanup_unlocked;

	if (S_ISDIR(c->stat.mode) && c->indexed) {
		err = ovl_create_index(c->dentry, c->origin_fh, temp);
		if (err)
			goto cleanup_unlocked;
	}

	/*
	 * We cannot hold lock_rename() throughout this helper, because of
	 * lock ordering with sb_writers, which shouldn't be held when calling
	 * ovl_copy_up_data(), so lock workdir and destdir and make sure that
	 * temp wasn't moved before copy up completion or cleanup.
	 */
	trap = lock_rename(c->workdir, c->destdir);
	if (trap || temp->d_parent != c->workdir) {
		/* temp or workdir moved underneath us? abort without cleanup */
		dput(temp);
		err = -EIO;
		if (!IS_ERR(trap))
			unlock_rename(c->workdir, c->destdir);
		goto out;
	}

	err = ovl_copy_up_metadata(c, temp);
	if (err)
		goto cleanup;

	upper = ovl_lookup_upper(ofs, c->destname.name, c->destdir,
				 c->destname.len);
	err = PTR_ERR(upper);
	if (IS_ERR(upper))
		goto cleanup;

	err = ovl_do_rename(ofs, c->workdir, temp, c->destdir, upper, 0);
	unlock_rename(c->workdir, c->destdir);
	dput(upper);
	if (err)
		goto cleanup_unlocked;

	inode = d_inode(c->dentry);
	if (c->metacopy_digest)
		ovl_set_flag(OVL_HAS_DIGEST, inode);
	else
		ovl_clear_flag(OVL_HAS_DIGEST, inode);
	ovl_clear_flag(OVL_VERIFIED_DIGEST, inode);

	if (!c->metacopy)
		ovl_set_upperdata(inode);
	ovl_inode_update(inode, temp);
	if (S_ISDIR(inode->i_mode))
		ovl_set_flag(OVL_WHITEOUTS, inode);
out:
	ovl_end_write(c->dentry);

	return err;

cleanup:
	unlock_rename(c->workdir, c->destdir);
cleanup_unlocked:
	ovl_cleanup(ofs, c->workdir, temp);
	dput(temp);
	goto out;
}

/* Copyup using O_TMPFILE which does not require cross dir locking */
static int ovl_copy_up_tmpfile(struct ovl_copy_up_ctx *c)
{
	struct ovl_fs *ofs = OVL_FS(c->dentry->d_sb);
	struct inode *udir = d_inode(c->destdir);
	struct dentry *temp, *upper;
	struct file *tmpfile;
	struct ovl_cu_creds cc;
	int err;

	err = ovl_prep_cu_creds(c->dentry, &cc);
	if (err)
		return err;

	ovl_start_write(c->dentry);
	tmpfile = ovl_do_tmpfile(ofs, c->workdir, c->stat.mode);
	ovl_end_write(c->dentry);
	ovl_revert_cu_creds(&cc);
	if (IS_ERR(tmpfile))
		return PTR_ERR(tmpfile);

	temp = tmpfile->f_path.dentry;
	if (!c->metacopy && c->stat.size) {
		err = ovl_copy_up_file(ofs, c->dentry, tmpfile, c->stat.size,
				       !c->metadata_fsync);
		if (err)
			goto out_fput;
	}

	ovl_start_write(c->dentry);

	err = ovl_copy_up_metadata(c, temp);
	if (err)
		goto out;

	inode_lock_nested(udir, I_MUTEX_PARENT);

	upper = ovl_lookup_upper(ofs, c->destname.name, c->destdir,
				 c->destname.len);
	err = PTR_ERR(upper);
	if (!IS_ERR(upper)) {
		err = ovl_do_link(ofs, temp, udir, upper);
		dput(upper);
	}
	inode_unlock(udir);

	if (err)
		goto out;

	if (c->metacopy_digest)
		ovl_set_flag(OVL_HAS_DIGEST, d_inode(c->dentry));
	else
		ovl_clear_flag(OVL_HAS_DIGEST, d_inode(c->dentry));
	ovl_clear_flag(OVL_VERIFIED_DIGEST, d_inode(c->dentry));

	if (!c->metacopy)
		ovl_set_upperdata(d_inode(c->dentry));
	ovl_inode_update(d_inode(c->dentry), dget(temp));

out:
	ovl_end_write(c->dentry);
out_fput:
	fput(tmpfile);
	return err;
}

/*
 * Copy up a single dentry
 *
 * All renames start with copy up of source if necessary.  The actual
 * rename will only proceed once the copy up was successful.  Copy up uses
 * upper parent i_mutex for exclusion.  Since rename can change d_parent it
 * is possible that the copy up will lock the old parent.  At that point
 * the file will have already been copied up anyway.
 */
static int ovl_do_copy_up(struct ovl_copy_up_ctx *c)
{
	int err;
	struct ovl_fs *ofs = OVL_FS(c->dentry->d_sb);
	struct dentry *origin = c->lowerpath.dentry;
	struct ovl_fh *fh = NULL;
	bool to_index = false;

	/*
	 * Indexed non-dir is copied up directly to the index entry and then
	 * hardlinked to upper dir. Indexed dir is copied up to indexdir,
	 * then index entry is created and then copied up dir installed.
	 * Copying dir up to indexdir instead of workdir simplifies locking.
	 */
	if (ovl_need_index(c->dentry)) {
		c->indexed = true;
		if (S_ISDIR(c->stat.mode))
			c->workdir = ovl_indexdir(c->dentry->d_sb);
		else
			to_index = true;
	}

	if (S_ISDIR(c->stat.mode) || c->stat.nlink == 1 || to_index) {
		fh = ovl_get_origin_fh(ofs, origin);
		if (IS_ERR(fh))
			return PTR_ERR(fh);

		/* origin_fh may be NULL */
		c->origin_fh = fh;
		c->origin = true;
	}

	if (to_index) {
		c->destdir = ovl_indexdir(c->dentry->d_sb);
		err = ovl_get_index_name(ofs, origin, &c->destname);
		if (err)
			goto out_free_fh;
	} else if (WARN_ON(!c->parent)) {
		/* Disconnected dentry must be copied up to index dir */
		err = -EIO;
		goto out_free_fh;
	} else {
		/*
		 * c->dentry->d_name is stabilzed by ovl_copy_up_start(),
		 * because if we got here, it means that c->dentry has no upper
		 * alias and changing ->d_name means going through ovl_rename()
		 * that will call ovl_copy_up() on source and target dentry.
		 */
		c->destname = c->dentry->d_name;
		/*
		 * Mark parent "impure" because it may now contain non-pure
		 * upper
		 */
		ovl_start_write(c->dentry);
		err = ovl_set_impure(c->parent, c->destdir);
		ovl_end_write(c->dentry);
		if (err)
			goto out_free_fh;
	}

	/* Should we copyup with O_TMPFILE or with workdir? */
	if (S_ISREG(c->stat.mode) && ofs->tmpfile)
		err = ovl_copy_up_tmpfile(c);
	else
		err = ovl_copy_up_workdir(c);
	if (err)
		goto out;

	if (c->indexed)
		ovl_set_flag(OVL_INDEX, d_inode(c->dentry));

	ovl_start_write(c->dentry);
	if (to_index) {
		/* Initialize nlink for copy up of disconnected dentry */
		err = ovl_set_nlink_upper(c->dentry);
	} else {
		struct inode *udir = d_inode(c->destdir);

		/* Restore timestamps on parent (best effort) */
		inode_lock(udir);
		ovl_set_timestamps(ofs, c->destdir, &c->pstat);
		inode_unlock(udir);

		ovl_dentry_set_upper_alias(c->dentry);
		ovl_dentry_update_reval(c->dentry, ovl_dentry_upper(c->dentry));
	}
	ovl_end_write(c->dentry);

out:
	if (to_index)
		kfree(c->destname.name);
out_free_fh:
	kfree(fh);
	return err;
}

static bool ovl_need_meta_copy_up(struct dentry *dentry, umode_t mode,
				  int flags)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);

	if (!ofs->config.metacopy)
		return false;

	if (!S_ISREG(mode))
		return false;

	if (flags && ((OPEN_FMODE(flags) & FMODE_WRITE) || (flags & O_TRUNC)))
		return false;

	/* Fall back to full copy if no fsverity on source data and we require verity */
	if (ofs->config.verity_mode == OVL_VERITY_REQUIRE) {
		struct path lowerdata;

		ovl_path_lowerdata(dentry, &lowerdata);

		if (WARN_ON_ONCE(lowerdata.dentry == NULL) ||
		    ovl_ensure_verity_loaded(&lowerdata) ||
		    !fsverity_active(d_inode(lowerdata.dentry))) {
			return false;
		}
	}

	return true;
}

static ssize_t ovl_getxattr_value(const struct path *path, char *name, char **value)
{
	ssize_t res;
	char *buf;

	res = ovl_do_getxattr(path, name, NULL, 0);
	if (res == -ENODATA || res == -EOPNOTSUPP)
		res = 0;

	if (res > 0) {
		buf = kzalloc(res, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		res = ovl_do_getxattr(path, name, buf, res);
		if (res < 0)
			kfree(buf);
		else
			*value = buf;
	}
	return res;
}

/* Copy up data of an inode which was copied up metadata only in the past. */
static int ovl_copy_up_meta_inode_data(struct ovl_copy_up_ctx *c)
{
	struct ovl_fs *ofs = OVL_FS(c->dentry->d_sb);
	struct path upperpath;
	int err;
	char *capability = NULL;
	ssize_t cap_size;

	ovl_path_upper(c->dentry, &upperpath);
	if (WARN_ON(upperpath.dentry == NULL))
		return -EIO;

	if (c->stat.size) {
		err = cap_size = ovl_getxattr_value(&upperpath, XATTR_NAME_CAPS,
						    &capability);
		if (cap_size < 0)
			goto out;
	}

	err = ovl_copy_up_data(c, &upperpath);
	if (err)
		goto out_free;

	/*
	 * Writing to upper file will clear security.capability xattr. We
	 * don't want that to happen for normal copy-up operation.
	 */
	ovl_start_write(c->dentry);
	if (capability) {
		err = ovl_do_setxattr(ofs, upperpath.dentry, XATTR_NAME_CAPS,
				      capability, cap_size, 0);
	}
	if (!err) {
		err = ovl_removexattr(ofs, upperpath.dentry,
				      OVL_XATTR_METACOPY);
	}
	ovl_end_write(c->dentry);
	if (err)
		goto out_free;

	ovl_clear_flag(OVL_HAS_DIGEST, d_inode(c->dentry));
	ovl_clear_flag(OVL_VERIFIED_DIGEST, d_inode(c->dentry));
	ovl_set_upperdata(d_inode(c->dentry));
out_free:
	kfree(capability);
out:
	return err;
}

static int ovl_copy_up_one(struct dentry *parent, struct dentry *dentry,
			   int flags)
{
	int err;
	DEFINE_DELAYED_CALL(done);
	struct path parentpath;
	struct ovl_copy_up_ctx ctx = {
		.parent = parent,
		.dentry = dentry,
		.workdir = ovl_workdir(dentry),
	};

	if (WARN_ON(!ctx.workdir))
		return -EROFS;

	ovl_path_lower(dentry, &ctx.lowerpath);
	err = vfs_getattr(&ctx.lowerpath, &ctx.stat,
			  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (err)
		return err;

	if (!kuid_has_mapping(current_user_ns(), ctx.stat.uid) ||
	    !kgid_has_mapping(current_user_ns(), ctx.stat.gid))
		return -EOVERFLOW;

	/*
	 * With metacopy disabled, we fsync after final metadata copyup, for
	 * both regular files and directories to get atomic copyup semantics
	 * on filesystems that do not use strict metadata ordering (e.g. ubifs).
	 *
	 * With metacopy enabled we want to avoid fsync on all meta copyup
	 * that will hurt performance of workloads such as chown -R, so we
	 * only fsync on data copyup as legacy behavior.
	 */
	ctx.metadata_fsync = !OVL_FS(dentry->d_sb)->config.metacopy &&
			     (S_ISREG(ctx.stat.mode) || S_ISDIR(ctx.stat.mode));
	ctx.metacopy = ovl_need_meta_copy_up(dentry, ctx.stat.mode, flags);

	if (parent) {
		ovl_path_upper(parent, &parentpath);
		ctx.destdir = parentpath.dentry;

		err = vfs_getattr(&parentpath, &ctx.pstat,
				  STATX_ATIME | STATX_MTIME,
				  AT_STATX_SYNC_AS_STAT);
		if (err)
			return err;
	}

	/* maybe truncate regular file. this has no effect on dirs */
	if (flags & O_TRUNC)
		ctx.stat.size = 0;

	if (S_ISLNK(ctx.stat.mode)) {
		ctx.link = vfs_get_link(ctx.lowerpath.dentry, &done);
		if (IS_ERR(ctx.link))
			return PTR_ERR(ctx.link);
	}

	err = ovl_copy_up_start(dentry, flags);
	/* err < 0: interrupted, err > 0: raced with another copy-up */
	if (unlikely(err)) {
		if (err > 0)
			err = 0;
	} else {
		if (!ovl_dentry_upper(dentry))
			err = ovl_do_copy_up(&ctx);
		if (!err && parent && !ovl_dentry_has_upper_alias(dentry))
			err = ovl_link_up(&ctx);
		if (!err && ovl_dentry_needs_data_copy_up_locked(dentry, flags))
			err = ovl_copy_up_meta_inode_data(&ctx);
		ovl_copy_up_end(dentry);
	}
	do_delayed_call(&done);

	return err;
}

static int ovl_copy_up_flags(struct dentry *dentry, int flags)
{
	int err = 0;
	const struct cred *old_cred;
	bool disconnected = (dentry->d_flags & DCACHE_DISCONNECTED);

	/*
	 * With NFS export, copy up can get called for a disconnected non-dir.
	 * In this case, we will copy up lower inode to index dir without
	 * linking it to upper dir.
	 */
	if (WARN_ON(disconnected && d_is_dir(dentry)))
		return -EIO;

	/*
	 * We may not need lowerdata if we are only doing metacopy up, but it is
	 * not very important to optimize this case, so do lazy lowerdata lookup
	 * before any copy up, so we can do it before taking ovl_inode_lock().
	 */
	err = ovl_verify_lowerdata(dentry);
	if (err)
		return err;

	old_cred = ovl_override_creds(dentry->d_sb);
	while (!err) {
		struct dentry *next;
		struct dentry *parent = NULL;

		if (ovl_already_copied_up(dentry, flags))
			break;

		next = dget(dentry);
		/* find the topmost dentry not yet copied up */
		for (; !disconnected;) {
			parent = dget_parent(next);

			if (ovl_dentry_upper(parent))
				break;

			dput(next);
			next = parent;
		}

		err = ovl_copy_up_one(parent, next, flags);

		dput(parent);
		dput(next);
	}
	ovl_revert_creds(old_cred);

	return err;
}

static bool ovl_open_need_copy_up(struct dentry *dentry, int flags)
{
	/* Copy up of disconnected dentry does not set upper alias */
	if (ovl_already_copied_up(dentry, flags))
		return false;

	if (special_file(d_inode(dentry)->i_mode))
		return false;

	if (!ovl_open_flags_need_copy_up(flags))
		return false;

	return true;
}

int ovl_maybe_copy_up(struct dentry *dentry, int flags)
{
	if (!ovl_open_need_copy_up(dentry, flags))
		return 0;

	return ovl_copy_up_flags(dentry, flags);
}

int ovl_copy_up_with_data(struct dentry *dentry)
{
	return ovl_copy_up_flags(dentry, O_WRONLY);
}

int ovl_copy_up(struct dentry *dentry)
{
	return ovl_copy_up_flags(dentry, 0);
}
