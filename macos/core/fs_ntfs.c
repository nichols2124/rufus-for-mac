/*
 * Rufus for macOS: NTFS formatting and file writing (via bundled ntfs-3g)
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Formatting is done by rufus-mkntfs (ntfs-3g's mkntfs, linked against our
 * fd+offset device driver) in a child process that inherits the raw disk fd.
 * Files are then written straight into the volume with libntfs-3g, so neither
 * macFUSE nor the ntfs-3g FUSE driver is needed on the user's machine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>

#include <ntfs-3g/types.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/logging.h>

#include "ntfs_io.h"
/* ntfs-3g has its own BOOL/TRUE/FALSE, which is compatible with ours */
#include "rufus_core.h"

extern char** environ;
char helper_dir[1024] = "";
static volatile pid_t mkntfs_pid = 0;

/* mkntfs runs in a child process that doesn't know about cancel_requested */
void kill_helpers(void)
{
	pid_t pid = mkntfs_pid;
	if (pid > 0) {
		uprintf("Stopping mkntfs (pid %d)", (int)pid);
		kill(pid, SIGKILL);
	}
}

static void ntfs_name(char* name, size_t size, part_view_t* pv)
{
	snprintf(name, size, "rufus:%d:%llu:%llu:%u", pv->dev->fd, (unsigned long long)pv->offset,
		(unsigned long long)pv->size, pv->dev->sector_size);
}

bool format_ntfs(part_view_t* pv, uint32_t cluster_size, const char* label, bool quick)
{
	char dev_name[128], helper[1100], cluster[16], sector[16], start[32], count[32];
	char* argv[24];
	int argc = 0, pipefd[2], status = 0, fl;
	posix_spawn_file_actions_t fa;
	pid_t pid;
	FILE* out;
	char line[1024];

	if (cluster_size == 0)
		cluster_size = default_cluster_size(FS_NTFS, pv->size);
	uprintf("Formatting (NTFS)...");
	update_status("Formatting (NTFS)...");
	update_progress(-1);
	uprintf("Using cluster size: %u bytes", cluster_size);

	snprintf(helper, sizeof(helper), "%s/rufus-mkntfs", helper_dir);
	ntfs_name(dev_name, sizeof(dev_name), pv);
	snprintf(cluster, sizeof(cluster), "%u", cluster_size);
	snprintf(sector, sizeof(sector), "%u", pv->dev->sector_size);
	snprintf(start, sizeof(start), "%llu", (unsigned long long)(pv->offset / pv->dev->sector_size));
	/* mkntfs puts the backup boot sector in the last of these sectors, which is where
	 * Windows (and ntfsfix) expect it: pass the full partition size */
	snprintf(count, sizeof(count), "%llu", (unsigned long long)(pv->size / pv->dev->sector_size));

	argv[argc++] = "rufus-mkntfs";
	argv[argc++] = "-F";                 /* force (we're not a block device node) */
	if (!rflags.enable_file_indexing)
		argv[argc++] = "-I";             /* disable content indexing, as Rufus does (Alt-Q) */
	if (rflags.enable_ntfs_compression)
		argv[argc++] = "-C";             /* Alt-N */
	if (quick)
		argv[argc++] = "-Q";
	argv[argc++] = "-c"; argv[argc++] = cluster;
	argv[argc++] = "-s"; argv[argc++] = sector;
	argv[argc++] = "-p"; argv[argc++] = start;   /* hidden sectors, needed for BIOS boot */
	argv[argc++] = "-H"; argv[argc++] = "255";
	argv[argc++] = "-S"; argv[argc++] = "63";
	if (label != NULL && label[0] != 0) {
		argv[argc++] = "-L";
		argv[argc++] = (char*)label;
	}
	argv[argc++] = dev_name;
	argv[argc++] = count;
	argv[argc] = NULL;

	/* The child must inherit the raw disk descriptor */
	fl = fcntl(pv->dev->fd, F_GETFD);
	if (fl >= 0)
		fcntl(pv->dev->fd, F_SETFD, fl & ~FD_CLOEXEC);
	if (pipe(pipefd) != 0) {
		uprintf("Could not create pipe: %s", strerror(errno));
		return false;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	{
		char cmdline[1024] = "";
		for (int i = 0; argv[i] != NULL; i++) {
			strlcat(cmdline, argv[i], sizeof(cmdline));
			strlcat(cmdline, " ", sizeof(cmdline));
		}
		uprintf("Running: %s", cmdline);
	}
	status = posix_spawn(&pid, helper, &fa, NULL, argv, environ);
	if (status == 0)
		mkntfs_pid = pid;
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);
	if (status != 0) {
		uprintf("Could not launch '%s': %s", helper, strerror(status));
		close(pipefd[0]);
		return false;
	}
	out = fdopen(pipefd[0], "r");
	while (out != NULL && fgets(line, sizeof(line), out) != NULL) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		if (len > 0)
			uprintf("  mkntfs: %s", line);
	}
	if (out != NULL)
		fclose(out);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
	mkntfs_pid = 0;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		uprintf("mkntfs failed (status %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return false;
	}
	update_progress(100.0);
	return true;
}

/* ------------------------------------------------------------------------ */
/* NTFS file sink                                                            */
/* ------------------------------------------------------------------------ */
typedef struct {
	struct ntfs_device* dev;
	ntfs_volume* vol;
} ntfs_sink_priv_t;

typedef struct {
	ntfs_inode* ni;
	ntfs_attr* na;
	s64 pos;
} ntfs_file_t;

/* UTF-8 -> UTF-16LE (with surrogates) */
static int utf8_to_ntfs(const char* s, ntfschar* out, int max)
{
	int n = 0;
	const unsigned char* p = (const unsigned char*)s;
	while (*p != 0 && n < max) {
		uint32_t cp;
		if (*p < 0x80) {
			cp = *p++;
		} else if ((*p & 0xe0) == 0xc0 && p[1] != 0) {
			cp = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
			p += 2;
		} else if ((*p & 0xf0) == 0xe0 && p[1] != 0 && p[2] != 0) {
			cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
			p += 3;
		} else if ((*p & 0xf8) == 0xf0 && p[1] != 0 && p[2] != 0 && p[3] != 0) {
			cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
			p += 4;
		} else {
			cp = '_';
			p++;
		}
		if (cp >= 0x10000) {
			if (n + 2 > max)
				break;
			cp -= 0x10000;
			out[n++] = cpu_to_le16((u16)(0xd800 | (cp >> 10)));
			out[n++] = cpu_to_le16((u16)(0xdc00 | (cp & 0x3ff)));
		} else {
			out[n++] = cpu_to_le16((u16)cp);
		}
	}
	return n;
}

/* Split "/a/b/c" into the parent inode (opened) and the UTF-16 leaf name */
static ntfs_inode* open_parent(ntfs_volume* vol, const char* path, ntfschar* leaf, int* leaf_len)
{
	char parent[1024];
	const char* slash = strrchr(path, '/');
	ntfs_inode* ni;

	if (slash == NULL)
		return NULL;
	if (slash == path)
		snprintf(parent, sizeof(parent), "/");
	else
		snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
	*leaf_len = utf8_to_ntfs(slash + 1, leaf, 255);
	ni = ntfs_pathname_to_inode(vol, NULL, parent);
	if (ni == NULL)
		uprintf("NTFS: could not open directory '%s': %s", parent, strerror(errno));
	return ni;
}

static bool ntfs_sink_mkdir(sink_t* s, const char* path)
{
	ntfs_sink_priv_t* priv = s->priv;
	ntfschar leaf[256];
	int leaf_len;
	ntfs_inode *dir, *ni;
	char tmp[1024];
	char* c;

	/* Create intermediate directories first */
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (c = tmp + 1; *c != 0; c++) {
		if (*c == '/') {
			*c = 0;
			ni = ntfs_pathname_to_inode(priv->vol, NULL, tmp);
			if (ni != NULL)
				ntfs_inode_close(ni);
			else if (!ntfs_sink_mkdir(s, tmp))
				return false;
			*c = '/';
		}
	}
	ni = ntfs_pathname_to_inode(priv->vol, NULL, path);
	if (ni != NULL) {
		ntfs_inode_close(ni);
		return true;
	}
	dir = open_parent(priv->vol, path, leaf, &leaf_len);
	if (dir == NULL)
		return false;
	ni = ntfs_create(dir, const_cpu_to_le32(0), leaf, (u8)leaf_len, S_IFDIR);
	if (ni == NULL) {
		uprintf("NTFS: could not create directory '%s': %s", path, strerror(errno));
		ntfs_inode_close(dir);
		return false;
	}
	ntfs_inode_close_in_dir(ni, dir);
	ntfs_inode_close(dir);
	return true;
}

static void* ntfs_sink_create(sink_t* s, const char* path, uint64_t size)
{
	ntfs_sink_priv_t* priv = s->priv;
	ntfschar leaf[256];
	int leaf_len;
	ntfs_inode* dir;
	ntfs_file_t* f = calloc(1, sizeof(ntfs_file_t));
	(void)size;

	if (f == NULL)
		return NULL;
	dir = open_parent(priv->vol, path, leaf, &leaf_len);
	if (dir == NULL) {
		/* Parent missing: create it */
		char parent[1024];
		const char* slash = strrchr(path, '/');
		snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
		if (slash == NULL || slash == path || !ntfs_sink_mkdir(s, parent) ||
			(dir = open_parent(priv->vol, path, leaf, &leaf_len)) == NULL) {
			free(f);
			return NULL;
		}
	}
	f->ni = ntfs_create(dir, const_cpu_to_le32(0), leaf, (u8)leaf_len, S_IFREG);
	if (f->ni == NULL) {
		uprintf("NTFS: could not create '%s': %s", path, strerror(errno));
		ntfs_inode_close(dir);
		free(f);
		return NULL;
	}
	/* Keep the directory index entry in sync */
	ntfs_inode_update_times(f->ni, NTFS_UPDATE_AMCTIME);
	ntfs_inode_close(dir);
	f->na = ntfs_attr_open(f->ni, AT_DATA, AT_UNNAMED, 0);
	if (f->na == NULL) {
		uprintf("NTFS: could not open data of '%s': %s", path, strerror(errno));
		ntfs_inode_close(f->ni);
		free(f);
		return NULL;
	}
	return f;
}

static bool ntfs_sink_write(sink_t* s, void* file, const void* buf, size_t len)
{
	ntfs_file_t* f = file;
	s64 done = 0;
	(void)s;
	while (done < (s64)len) {
		s64 w = ntfs_attr_pwrite(f->na, f->pos, (s64)len - done, (const u8*)buf + done);
		if (w <= 0) {
			uprintf("NTFS: write error: %s", strerror(errno));
			return false;
		}
		done += w;
		f->pos += w;
	}
	return true;
}

static bool ntfs_sink_close(sink_t* s, void* file)
{
	ntfs_file_t* f = file;
	bool r = true;
	(void)s;
	ntfs_attr_close(f->na);
	if (ntfs_inode_close(f->ni) != 0) {
		uprintf("NTFS: could not close inode: %s", strerror(errno));
		r = false;
	}
	free(f);
	return r;
}

static bool ntfs_sink_set_label(sink_t* s, const char* label)
{
	ntfs_sink_priv_t* priv = s->priv;
	ntfschar ulabel[128];
	int len = utf8_to_ntfs(label, ulabel, 32);
	return ntfs_volume_rename(priv->vol, ulabel, len) == 0;
}

static bool ntfs_sink_unmount(sink_t* s)
{
	ntfs_sink_priv_t* priv = s->priv;
	bool r = (ntfs_umount(priv->vol, FALSE) == 0);
	if (!r)
		uprintf("NTFS: could not unmount volume: %s", strerror(errno));
	free(priv);
	free(s);
	return r;
}

static int ntfs_log_to_uprintf(const char* function, const char* file, int line, u32 level,
	void* data, const char* format, va_list args)
{
	char buf[1024];
	(void)function; (void)file; (void)line; (void)data;
	if (level & (NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_PERROR | NTFS_LOG_LEVEL_WARNING)) {
		vsnprintf(buf, sizeof(buf), format, args);
		uprintf("  ntfs-3g: %s", buf);
	}
	return 0;
}

sink_t* ntfs_sink_open(part_view_t* pv)
{
	char name[128];
	sink_t* s = calloc(1, sizeof(sink_t));
	ntfs_sink_priv_t* priv = calloc(1, sizeof(ntfs_sink_priv_t));

	if (s == NULL || priv == NULL)
		goto fail;
	ntfs_log_set_handler(ntfs_log_to_uprintf);
	ntfs_name(name, sizeof(name), pv);
	priv->dev = ntfs_device_alloc(name, 0, &rufus_ntfs_io_ops, NULL);
	if (priv->dev == NULL) {
		uprintf("NTFS: could not allocate device");
		goto fail;
	}
	priv->vol = ntfs_device_mount(priv->dev, NTFS_MNT_EXCLUSIVE);
	if (priv->vol == NULL) {
		uprintf("NTFS: could not open the volume: %s", strerror(errno));
		ntfs_device_free(priv->dev);
		goto fail;
	}
	s->priv = priv;
	s->mkdir = ntfs_sink_mkdir;
	s->create = ntfs_sink_create;
	s->write = ntfs_sink_write;
	s->close = ntfs_sink_close;
	s->set_label = ntfs_sink_set_label;
	s->unmount = ntfs_sink_unmount;
	s->max_file_size = UINT64_MAX;
	return s;
fail:
	free(priv);
	free(s);
	return NULL;
}
