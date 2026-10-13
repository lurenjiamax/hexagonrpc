/*
 * FastRPC operating system interface implementation
 *
 * Copyright (C) 2023 The Sensor Shell Contributors
 *
 * This file is part of sensh.
 *
 * Sensh is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aee_error.h"
#include "interfaces/apps_std.def"
#include "hexagonfs.h"
#include "iobuffer.h"
#include "listener.h"

struct apps_std_ctx {
	int rootfd;
	int adsp_avs_cfg_dirfd;
	int adsp_library_dirfd;
	int sensor_persist_dirfd;
	struct hexagonfs_fd *fds[HEXAGONFS_MAX_FD];
};

static const int apps_std_whence_table[] = {
	SEEK_SET,
	SEEK_CUR,
	SEEK_END,
};

/*
 * This is a placeholder function used to complete any I/O operations.
 * File descriptors do not have a flush operation because their reads and
 * writes are blocking.
 */
static uint32_t apps_std_fflush(void *data,
				const struct fastrpc_io_buffer *inbufs,
				struct fastrpc_io_buffer *outbufs)
{
#ifdef HEXAGONRPC_VERBOSE
	uint32_t *fd = inbufs[0].p;

	printf("ignore fflush(%u)\n", *fd);
#endif

	return 0;
}

static uint32_t apps_std_fclose(void *data,
				const struct fastrpc_io_buffer *inbufs,
				struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const uint32_t *first_in = inbufs[0].p;
	int ret;

	ret = hexagonfs_close(ctx->fds, *first_in);
	if (ret) {
		fprintf(stderr, "Could not close: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("close(%u)\n", *first_in);
#endif

	return 0;
}

static uint32_t apps_std_fread(void *data,
			       const struct fastrpc_io_buffer *inbufs,
			       struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const struct {
		uint32_t fd;
		uint32_t buf_size;
	} *first_in = inbufs[0].p;
	struct {
		uint32_t written;
		uint32_t is_eof;
	} *first_out = outbufs[0].p;
	ssize_t ret;

	ret = hexagonfs_read(ctx->fds, first_in->fd,
			     first_in->buf_size, outbufs[1].p);
	if (ret < 0) {
		fprintf(stderr, "Could not read file: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("read(%u, %u) -> %ld\n", first_in->fd,
					first_in->buf_size,
					ret);
#endif

	first_out->written = ret;
	first_out->is_eof = first_out->written < first_in->buf_size;

	return 0;
}

static uint32_t apps_std_fwrite(void *data,
				const struct fastrpc_io_buffer *inbufs,
				struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const struct {
		uint32_t fd;
		uint32_t buf_size;
	} *first_in = inbufs[0].p;
	struct {
		uint32_t written;
		uint32_t is_eof;
	} *first_out = outbufs[0].p;
	ssize_t ret;

	if (first_in->buf_size > inbufs[1].s)
		return AEE_EBADPARM;

	ret = hexagonfs_write(ctx->fds, first_in->fd,
			      first_in->buf_size, inbufs[1].p);
	if (ret < 0) {
		fprintf(stderr, "Could not write file: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("write(%u, %u) -> %ld\n", first_in->fd,
					 first_in->buf_size,
					 ret);
#endif

	first_out->written = ret;
	first_out->is_eof = 0;

	return 0;
}

static uint32_t apps_std_fsync(void *data,
			       const struct fastrpc_io_buffer *inbufs,
			       struct fastrpc_io_buffer *outbufs)
{
	/*
	 * HexagonFS uses blocking file-descriptor I/O. This mirrors the
	 * existing fflush implementation and is sufficient for the registry
	 * service, which closes the temporary file before renaming it.
	 */
	return 0;
}

static char *apps_std_sensor_path(const char *path)
{
	static const char *const roots[] = {
		"/mnt/vendor/persist/sensors/registry",
		"/persist/sensors/registry",
		"/mnt/vendor/persist/sensors",
		"/persist/sensors",
	};
	const char *suffix = NULL;
	const char *segment;
	char *normalized;
	size_t segment_len;
	size_t path_len;
	size_t root_len;
	size_t out_len = 0;
	size_t depth = 0;
	size_t i;

	if (path == NULL)
		return NULL;

	for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		root_len = strlen(roots[i]);
		if (!strncmp(path, roots[i], root_len)
		 && (path[root_len] == '\0' || path[root_len] == '/')) {
			suffix = &path[root_len];
			break;
		}
	}
	if (suffix == NULL)
		return NULL;

	while (*suffix == '/')
		suffix++;

	path_len = strlen(suffix);
	normalized = malloc(path_len + 1);
	if (normalized == NULL)
		return NULL;

	while (*suffix != '\0') {
		segment = suffix;
		while (*suffix != '\0' && *suffix != '/')
			suffix++;
		segment_len = suffix - segment;
		while (*suffix == '/')
			suffix++;

		if (segment_len == 0
		 || (segment_len == 1 && segment[0] == '.'))
			continue;

		if (segment_len == 2 && segment[0] == '.'
		 && segment[1] == '.') {
			if (depth == 0)
				goto err;
			while (out_len > 0 && normalized[out_len - 1] != '/')
				out_len--;
			if (out_len > 0)
				out_len--;
			depth--;
			continue;
		}

		if (out_len != 0)
			normalized[out_len++] = '/';
		memcpy(&normalized[out_len], segment, segment_len);
		out_len += segment_len;
		depth++;
	}

	normalized[out_len] = '\0';
	return normalized;

err:
	free(normalized);
	return NULL;
}

static uint32_t apps_std_fremove(void *data,
				 const struct fastrpc_io_buffer *inbufs,
				 struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const char *path = inbufs[1].p;
	char *relative;
	int ret;

	if (path == NULL || inbufs[1].s == 0
	 || path[inbufs[1].s - 1] != '\0')
		return AEE_EBADPARM;

	relative = apps_std_sensor_path(path);
	if (relative == NULL || *relative == '\0') {
		free(relative);
		return AEE_EREADONLY;
	}

	ret = unlinkat(ctx->sensor_persist_dirfd, relative, 0);
	if (ret && errno != ENOENT) {
		fprintf(stderr, "Could not remove %s: %s\n",
				path, strerror(errno));
		free(relative);
		return AEE_EFAILED;
	}

	free(relative);
	return 0;
}

static uint32_t apps_std_ftrunc(void *data,
				const struct fastrpc_io_buffer *inbufs,
				struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const struct {
		uint32_t fd;
		uint32_t length;
	} *first_in = inbufs[0].p;
	int ret;

	ret = hexagonfs_ftruncate(ctx->fds, first_in->fd, first_in->length);
	if (ret) {
		fprintf(stderr, "Could not truncate file: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

	return 0;
}

static uint32_t apps_std_frename(void *data,
				 const struct fastrpc_io_buffer *inbufs,
				 struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const char *old_path = inbufs[1].p;
	const char *new_path = inbufs[2].p;
	char *old_relative;
	char *new_relative;
	int ret;

	if (old_path == NULL || new_path == NULL
	 || inbufs[1].s == 0 || inbufs[2].s == 0
	 || old_path[inbufs[1].s - 1] != '\0'
	 || new_path[inbufs[2].s - 1] != '\0')
		return AEE_EBADPARM;

	old_relative = apps_std_sensor_path(old_path);
	new_relative = apps_std_sensor_path(new_path);
	if (old_relative == NULL || new_relative == NULL
	 || *old_relative == '\0' || *new_relative == '\0') {
		free(old_relative);
		free(new_relative);
		return AEE_EREADONLY;
	}

	ret = renameat(ctx->sensor_persist_dirfd, old_relative,
		       ctx->sensor_persist_dirfd, new_relative);
	if (ret) {
		fprintf(stderr, "Could not rename %s to %s: %s\n",
				old_path, new_path, strerror(errno));
		free(old_relative);
		free(new_relative);
		return AEE_EFAILED;
	}

	free(old_relative);
	free(new_relative);
	return 0;
}

static uint32_t apps_std_fseek(void *data,
			       const struct fastrpc_io_buffer *inbufs,
			       struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const struct {
		uint32_t fd;
		uint32_t pos;
		uint32_t whence;
	} *first_in = inbufs[0].p;
	int ret;
	int whence;

	whence = apps_std_whence_table[first_in->whence];

	ret = hexagonfs_lseek(ctx->fds, first_in->fd, first_in->pos, whence);
	if (ret) {
		fprintf(stderr, "Could not seek stream: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("lseek(%u, %d, %d)\n", first_in->fd,
				      first_in->pos,
				      first_in->whence);
#endif

	return 0;
}

static uint32_t apps_std_fopen_with_env(void *data,
					const struct fastrpc_io_buffer *inbufs,
					struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	uint32_t *out = outbufs[0].p;
	char *sensor_path = NULL;
	char rw_mode;
	int flags;
	int dirfd, fd;

	// The name and environment variable must also be NULL-terminated
	if (inbufs[1].s == 0 || inbufs[3].s == 0 || inbufs[4].s == 0
	 || ((const char *) inbufs[1].p)[inbufs[1].s - 1] != 0
	 || ((const char *) inbufs[3].p)[inbufs[3].s - 1] != 0
	 || ((const char *) inbufs[4].p)[inbufs[4].s - 1] != 0)
		return AEE_EBADPARM;

	rw_mode = ((const char *) inbufs[4].p)[0];
	switch (rw_mode) {
	case 'r':
		flags = O_RDONLY;
		break;
	case 'w':
		flags = O_WRONLY | O_CREAT | O_TRUNC;
		break;
	case 'a':
		flags = O_WRONLY | O_CREAT | O_APPEND;
		break;
	default:
		return AEE_EBADPARM;
	}
	if (inbufs[4].s > 1 && ((const char *) inbufs[4].p)[1] == '+')
		flags = (flags & ~O_ACCMODE) | O_RDWR;

	/*
	 * The DSP library search path is also used as the base for absolute
	 * registry paths. Permit write-mode opens only below the persistent
	 * sensor tree; all firmware and configuration mappings stay read-only.
	 */
	if ((flags & O_ACCMODE) != O_RDONLY) {
		sensor_path = apps_std_sensor_path(inbufs[3].p);
		if (sensor_path == NULL || *sensor_path == '\0') {
			free(sensor_path);
			return AEE_EREADONLY;
		}
	}

	if (!strcmp(inbufs[1].p, "ADSP_LIBRARY_PATH")) {
		dirfd = ctx->adsp_library_dirfd;
	} else if (!strcmp(inbufs[1].p, "ADSP_AVS_CFG_PATH")) {
		dirfd = ctx->adsp_avs_cfg_dirfd;
	} else {
		fprintf(stderr, "Unknown search directory %s\n",
				(const char *) inbufs[1].p);
		free(sensor_path);
		return AEE_EBADPARM;
	}

	if (dirfd < 0) {
		fprintf(stderr, "Could not open virtual %s: %s\n",
				(const char *) inbufs[1].p, strerror(-dirfd));
		free(sensor_path);
		return AEE_EFAILED;
	}

	fd = hexagonfs_openat_flags(ctx->fds, ctx->rootfd, dirfd,
				    inbufs[3].p, flags, 0644);
	free(sensor_path);
	if (fd < 0) {
		fprintf(stderr, "openat($%s, %s, %c) failed: %s\n",
				(const char *) inbufs[1].p,
				(const char *) inbufs[3].p,
				rw_mode,
				strerror(-fd));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("openat($%s, %s, %c) -> %d\n",
	       (const char *) inbufs[1].p,
	       (const char *) inbufs[3].p,
	       rw_mode, fd);
#endif

	*out = fd;

	return 0;
}

static uint32_t apps_std_opendir(void *data,
				 const struct fastrpc_io_buffer *inbufs,
				 struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	uint64_t *dir_out = outbufs[0].p;
	int ret;

	// The name must be NULL-terminated
	if (((const char *) inbufs[1].p)[inbufs[1].s - 1] != 0)
		return AEE_EBADPARM;

	ret = hexagonfs_openat(ctx->fds, ctx->rootfd, ctx->rootfd, inbufs[1].p);
	if (ret < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
				(const char *) inbufs[1].p,
				strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("opendir(%s) -> %d\n", (const char *) inbufs[1].p, ret);
#endif

	*dir_out = ret;

	return 0;
}

static uint32_t apps_std_closedir(void *data,
				  const struct fastrpc_io_buffer *inbufs,
				  struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const uint64_t *dir = inbufs[0].p;
	int ret;

	ret = hexagonfs_close(ctx->fds, *dir);
	if (ret)
		return AEE_EFAILED;

#ifdef HEXAGONRPC_VERBOSE
	printf("closedir(%ld)\n", *dir);
#endif

	return 0;
}

static uint32_t apps_std_readdir(void *data,
				 const struct fastrpc_io_buffer *inbufs,
				 struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const uint64_t *dir = inbufs[0].p;
	struct {
		uint32_t inode;
		char name[255];
		uint32_t is_eof;
	} *first_out = outbufs[0].p;
	int ret;

	ret = hexagonfs_readdir(ctx->fds, *dir, 255, first_out->name);
	if (ret < 0) {
		fprintf(stderr, "Could not read from directory: %s\n",
				strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("readdir(%ld) -> %s\n", *dir, first_out->name);
#endif

	first_out->inode = 0;
	first_out->is_eof = (*first_out->name == '\0');

	return 0;
}

static uint32_t apps_std_stat(void *data,
			      const struct fastrpc_io_buffer *inbufs,
			      struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	struct {
		uint64_t tsz; // Unknown purpose
		uint64_t dev;
		uint64_t ino;
		uint32_t mode;
		uint32_t nlink;
		uint64_t rdev;
		uint64_t size;
		int64_t atime;
		int64_t atimensec;
		int64_t mtime;
		int64_t mtimensec;
		int64_t ctime;
		int64_t ctimensec;
	} *first_out = outbufs[0].p;
	const char *pathname = inbufs[1].p;
	struct stat stats;
	int fd, ret;

	if (((const char *) inbufs[1].p)[inbufs[1].s - 1] != 0)
		return AEE_EBADPARM;

	fd = hexagonfs_openat(ctx->fds, ctx->rootfd, ctx->adsp_library_dirfd, pathname);
	if (fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
				pathname, strerror(-fd));
		return AEE_EFAILED;
	}

	ret = hexagonfs_fstat(ctx->fds, fd, &stats);
	if (ret) {
		fprintf(stderr, "Could not stat %s: %s\n",
				pathname, strerror(-fd));
		return AEE_EFAILED;
	}

	hexagonfs_close(ctx->fds, fd);

#ifdef HEXAGONRPC_VERBOSE
	printf("stat(%s)\n", pathname);
#endif

	first_out->tsz = 0;

	first_out->dev = stats.st_dev;
	first_out->ino = stats.st_ino;
	first_out->mode = stats.st_mode;
	first_out->nlink = stats.st_nlink;
	first_out->rdev = stats.st_rdev;
	first_out->size = stats.st_size;
	first_out->atime = (int64_t) stats.st_atim.tv_sec;
	first_out->atimensec = (int64_t) stats.st_atim.tv_nsec;
	first_out->mtime = (int64_t) stats.st_mtim.tv_sec;
	first_out->mtimensec = (int64_t) stats.st_mtim.tv_nsec;
	first_out->ctime = (int64_t) stats.st_ctim.tv_nsec;
	first_out->ctimensec = (int64_t) stats.st_ctim.tv_nsec;

	return 0;
}

struct fastrpc_interface *fastrpc_apps_std_init(struct hexagonfs_dirent *root,
						const char *prefix)
{
	struct fastrpc_interface *iface;
	struct apps_std_ctx *ctx;
	char *sensor_persist;
	size_t prefix_len;

	iface = malloc(sizeof(struct fastrpc_interface));
	if (iface == NULL)
		return NULL;

	ctx = calloc(1, sizeof(struct apps_std_ctx));
	if (ctx == NULL)
		goto err_free_iface;

	memcpy(iface, &apps_std_interface, sizeof(struct fastrpc_interface));

	ctx->rootfd = hexagonfs_open_root(ctx->fds, root);
	if (ctx->rootfd < 0)
		goto err_free_ctx;

	ctx->adsp_avs_cfg_dirfd = hexagonfs_openat(ctx->fds,
						   ctx->rootfd,
						   ctx->rootfd,
						   "/vendor/etc/acdbdata/");
	ctx->adsp_library_dirfd = hexagonfs_openat(ctx->fds,
						   ctx->rootfd,
						   ctx->rootfd,
						   "/usr/lib/qcom/adsp/");

	prefix_len = strlen(prefix);
	sensor_persist = malloc(prefix_len + strlen("/sensors") + 1);
	if (sensor_persist == NULL)
		goto err_close_fds;
	strcpy(sensor_persist, prefix);
	strcat(sensor_persist, "/sensors");
	ctx->sensor_persist_dirfd = open(sensor_persist,
					 O_RDONLY | O_DIRECTORY);
	free(sensor_persist);
	if (ctx->sensor_persist_dirfd < 0)
		goto err_close_fds;

	iface->data = ctx;

	return iface;

err_close_fds:
	for (int i = 0; i < HEXAGONFS_MAX_FD; i++) {
		if (ctx->fds[i] != NULL)
			hexagonfs_close(ctx->fds, i);
	}
err_free_ctx:
	free(ctx);
err_free_iface:
	free(iface);

	return NULL;
}

void fastrpc_apps_std_deinit(struct fastrpc_interface *iface)
{
	struct apps_std_ctx *ctx = iface->data;
	int i;

	for (i = 0; i < HEXAGONFS_MAX_FD; i++) {
		if (ctx->fds[i] != NULL)
			hexagonfs_close(ctx->fds, i);
	}

	close(ctx->sensor_persist_dirfd);
	free(iface->data);
	free(iface);
}

static const struct fastrpc_function_impl apps_std_procs[34] = {
	[2] = {
		.def = &apps_std_fflush_def,
		.impl = apps_std_fflush,
	},
	[3] = {
		.def = &apps_std_fclose_def,
		.impl = apps_std_fclose,
	},
	[4] = {
		.def = &apps_std_fread_def,
		.impl = apps_std_fread,
	},
	[5] = {
		.def = &apps_std_fwrite_def,
		.impl = apps_std_fwrite,
	},
	[9] = {
		.def = &apps_std_fseek_def,
		.impl = apps_std_fseek,
	},
	[19] = {
		.def = &apps_std_fopen_with_env_def,
		.impl = apps_std_fopen_with_env,
	},
	[23] = {
		.def = &apps_std_fsync_def,
		.impl = apps_std_fsync,
	},
	[24] = {
		.def = &apps_std_fremove_def,
		.impl = apps_std_fremove,
	},
	[26] = {
		.def = &apps_std_opendir_def,
		.impl = apps_std_opendir,
	},
	[27] = {
		.def = &apps_std_closedir_def,
		.impl = apps_std_closedir,
	},
	[28] = {
		.def = &apps_std_readdir_def,
		.impl = apps_std_readdir,
	},
	[31] = {
		.def = &apps_std_stat_def,
		.impl = apps_std_stat,
	},
	[32] = {
		.def = &apps_std_ftrunc_def,
		.impl = apps_std_ftrunc,
	},
	[33] = {
		.def = &apps_std_frename_def,
		.impl = apps_std_frename,
	},
};

const struct fastrpc_interface apps_std_interface = {
	.name = "apps_std",
	.n_procs = 34,
	.procs = apps_std_procs,
};
