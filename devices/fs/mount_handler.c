/*
 * (c) Copyright 2016 G. Campana
 * (c) Copyright 2016 Quarkslab
 *
 * This file is part of Cappsule.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE
#include <err.h>
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <linux/limits.h>

#include "aufs.h"
#include "direct.h"
#include "mount.h"
#include "mount_handler.h"
#include "overlay.h"
#include "policy.h"
#include "userland.h"
#include "uuid.h"

#define ARRAY_SIZE(arr)		(sizeof(arr) / sizeof(arr[0]))
#define DIFF_DIR_MOUNT_POINT	"diffdir/"

static struct mount_handler mount_handlers[] = {
	{ FS_MOUNT_TYPE_OVERLAY, "overlay", overlay_mount_fs, true, true },
	{ FS_MOUNT_TYPE_AUFS, "aufs",  aufs_mount_fs, true, false },
	{ FS_MOUNT_TYPE_DIRECT_ACCESS, "direct", direct_mount_fs, false, false },
};


struct mount_handler *mount_handler_by_type(enum fs_mount_type type)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(mount_handlers); i++) {
		if (mount_handlers[i].type == type)
			return &mount_handlers[i];
	}

	return NULL;
}

static int change_privileges(uid_t uid, gid_t gid,
		 	     uid_t *old_uid, gid_t *old_gid)
{
	if (old_uid)
		*old_uid = geteuid();

	if (old_gid)
		*old_gid = getegid();

	if (setegid(gid) != 0) {
		warn("setegid(%u)", gid);
		return -1;
	}

	if (seteuid(uid) != 0) {
		warn("seteuid(%u)", uid);
		return -1;
	}

	return 0;
}

int install_network_config(const char *rootfs)
{
	char resolv_conf[PATH_MAX];
	mode_t mode;
	int fd;

	/* TODO: override this setting with a configuration file. */
	char capsule_nameserver[] = "8.8.8.8";

	snprintf(resolv_conf, sizeof(resolv_conf),
		 "%s/%s", rootfs, "etc/resolv.conf");

	if (unlink(resolv_conf) == -1 && errno != ENOENT) {
		warn("cannot remove %s", resolv_conf);
		return -1;
	}

	mode = umask(S_IWGRP | S_IWOTH);
	fd = open(resolv_conf, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW, 0644);
	if (fd == -1) {
		warn("cannot open %s", resolv_conf);
		return -1;
	}
	umask(mode);

	dprintf(fd,
		"# Generated by Cappsule\n"
		"nameserver %s\n",
		capsule_nameserver);

	close(fd);
	return 0;
}

static int create_run_user_dir(uid_t uid, gid_t gid)
{
	char run_user_dir[PATH_MAX];
	struct stat st;
	err_t error;

	/* /run/user/%d/ may not exist, and can't be created by user */
	fmt_run_user_dir(run_user_dir, sizeof(run_user_dir), uid);
	if (stat(run_user_dir, &st) == -1) {
		error = make_dirs(run_user_dir);
		if (error)
			return -1;

		if (chown(run_user_dir, uid, gid) != 0) {
			warn("chown(%s, 0, 0)", run_user_dir);
			return -1;
		}
	}

	return 0;
}

static int create_directories_as(const char *dirpath, uid_t uid, gid_t gid)
{
	uid_t orig_uid;
	gid_t orig_gid;
	err_t error;

	/* Temporarily drop privileges */
	if (change_privileges(uid, gid, &orig_uid, &orig_gid) != 0)
		return -1;

	error = make_dirs(dirpath);

	/* Restore privileges. Any error is fatal. */
	if (change_privileges(orig_uid, orig_gid, NULL, NULL) != 0)
		errx(1, "cannot restore privileges");

	return error ? -1 : 0;
}

/**
 * @result /home/user/.cappsule/unrestricted/overlay/172a819a-cda4-3d9a-a2f1-541b25ea0a9e/
 */
int build_fs_dir(struct mount_point *mp, char *result, size_t size)
{
	int ret;

	ret = snprintf(result, size, "%s/%s/%s/", mp->base_dir,
		       mp->mhandler->name, mp->uuid);
	if ((size_t)ret >= size) {
		warnx("path too long");
		return -1;
	}

	if (create_directories_as(result, mp->uid, mp->gid) != 0) {
		warnx("cannot create fs directory at %s", result);
		return -1;
	}

	return 0;
}

/**
 * Check that the diff directory is owned by the user, and create the mount
 * point directory inside it.
 *
 * @fs_dir   /home/user/.cappsule/unrestricted/overlay/172a819a-cda4-3d9a-a2f1-541b25ea0a9e/
 * @diff_dir /home/user/.cappsule/unrestricted/overlay/172a819a-cda4-3d9a-a2f1-541b25ea0a9e/diffdir/
 */
static int build_diff_dir(struct mount_point *mp, const char *fs_dir,
			  char *diff_dir, size_t size)
{
	struct stat st;
	int dirfd, ret;
	mode_t mode;

	dirfd = open(fs_dir, O_DIRECTORY);
	if (dirfd == -1) {
		warn("open(\"%s\")", diff_dir);
		return -1;
	}

	/* A malicious user may give an invalid base_dir, or modify some of its
	 * components after the directory creation. Ensure that the owner of
	 * the directory is the user. */
	if (fstat(dirfd, &st) != 0) {
		warn("failed to stat \"%s\"", diff_dir);
		close(dirfd);
		return -1;
	}

	if (st.st_uid != mp->uid || st.st_gid != mp->gid) {
		warnx("bad owner for \"%s\"", diff_dir);
		close(dirfd);
		return -1;
	}

	/* append DIFF_DIR_MOUNT_POINT to diff_dir path */
	ret = snprintf(diff_dir, size, "%s/" DIFF_DIR_MOUNT_POINT, fs_dir);
	if ((size_t)ret >= size) {
		warnx("path too long");
		close(dirfd);
		return -1;
	}

	/* The capsule's root directory must be owned by root with 0755
	 * permissions. This is why a new directory owned by root is created.
	 *
	 * Instead of creating a new directory, one could also check if the
	 * parent directory is owned by the user, but ~/x/ wouldn't be
	 * accepted. */
	ret = fstatat(dirfd, DIFF_DIR_MOUNT_POINT, &st, 0);
	if (ret != 0) {
		if (errno != ENOENT) {
			warnx("failed to stat \"%s\"", diff_dir);
			close(dirfd);
			return -1;
		}

		mode = umask(0);
		ret = mkdirat(dirfd, DIFF_DIR_MOUNT_POINT, 0755);
		umask(mode);

		if (ret != 0) {
			warnx("failed to mkdir \"%s\"", diff_dir);
			close(dirfd);
			return -1;
		}
	}

	close(dirfd);

	return 0;
}

static int create_diff_dir(struct mount_point *mp, char *diff_dir, size_t size)
{
	char fs_dir[PATH_MAX];

	if (!mp->mhandler->diff_dir_required) {
		if (size == 0)
			return -1;

		diff_dir[0] = '\x00';
		return 0;
	}

	if (build_fs_dir(mp, fs_dir, sizeof(fs_dir)) != 0)
		return -1;

	if (build_diff_dir(mp, fs_dir, diff_dir, size) != 0)
		return -1;

	return 0;
}

static int create_work_dir(struct mount_point *mp, char *diff_dir)
{
	char workdir[PATH_MAX];

	if (!mp->mhandler->work_dir_required)
		return 0;

	if (get_workdir(diff_dir, workdir, sizeof(workdir)) != 0)
		return -1;

	if (create_directories_as(workdir, mp->uid, mp->gid) != 0) {
		warnx("cannot create workdir at %s", workdir);
		return -1;
	}

	return 0;
}

/**
 * @target mount @mp->fs->path to the directory specified by @target
 */
int mount_capsule_fs(struct mount_point *mp, char *target)
{
	char diff_dir[PATH_MAX];

	if (create_directories_as(target, mp->uid, mp->gid) != 0) {
		warnx("cannot create fs directory at %s", target);
		return -1;
	}

	if (create_diff_dir(mp, diff_dir, sizeof(diff_dir)) != 0)
		return -1;

	if (create_work_dir(mp, diff_dir) != 0)
		return -1;

	if (mp->mhandler->mount(mp->fs->path, target, diff_dir) != 0) {
		warnx("failed to mount capsule fs");
		return -1;
	}

	return 0;
}

int init_capsule_filesystems(struct mount_point *mp)
{
	if (create_run_user_dir(mp->uid, mp->gid) != 0) {
		warnx("cannot create user directory in /run");
		return -1;
	}

	return 0;
}
