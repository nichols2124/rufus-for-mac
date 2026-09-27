/*
 * Rufus for macOS: ext2/ext3 formatting (port of src/format_ext.c)
 * Copyright © 2019-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Uses Rufus' copy of e2fsprogs' libext2fs, with an I/O manager that works on
 * a partition inside the raw device fd (instead of Windows' nt_io.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include "config.h"
#include "ext2fs/ext2fs.h"

#include "rufus_core.h"

/* ------------------------------------------------------------------------ */
/* I/O manager for "rufus:<fd>:<offset>:<size>:<sector_size>" devices        */
/* ------------------------------------------------------------------------ */
typedef struct {
	int fd;
	uint64_t offset, size;
	uint32_t sector_size;
} rufus_ext_io_t;

static bool parse_name(const char* name, rufus_ext_io_t* d)
{
	unsigned long long o, s;
	unsigned int ss;
	int fd;
	if (name == NULL || sscanf(name, "rufus:%d:%llu:%llu:%u", &fd, &o, &s, &ss) != 4)
		return false;
	d->fd = fd;
	d->offset = o;
	d->size = s;
	d->sector_size = ss;
	return true;
}

static struct struct_io_manager struct_rufus_manager;

static errcode_t rufus_open(const char* name, int flags, io_channel* channel)
{
	io_channel io;
	rufus_ext_io_t* d;
	(void)flags;
	io = calloc(1, sizeof(struct struct_io_channel));
	d = calloc(1, sizeof(rufus_ext_io_t));
	if (io == NULL || d == NULL || !parse_name(name, d)) {
		free(io);
		free(d);
		return EXT2_ET_BAD_DEVICE_NAME;
	}
	io->magic = EXT2_ET_MAGIC_IO_CHANNEL;
	io->manager = &struct_rufus_manager;
	io->name = strdup(name);
	io->block_size = 1024;
	io->private_data = d;
	io->refcount = 1;
	io->flags = CHANNEL_FLAGS_BLOCK_DEVICE;
	*channel = io;
	return 0;
}

static errcode_t rufus_close(io_channel channel)
{
	if (--channel->refcount > 0)
		return 0;
	free(channel->private_data);
	free(channel->name);
	free(channel);
	return 0;
}

static errcode_t rufus_set_blksize(io_channel channel, int blksize)
{
	channel->block_size = blksize;
	return 0;
}

static errcode_t xfer(io_channel channel, unsigned long long block, int count, void* data, bool is_write)
{
	rufus_ext_io_t* d = channel->private_data;
	/* A negative count is a byte count (libext2fs convention) */
	uint64_t len = (count < 0) ? (uint64_t)(-count) : (uint64_t)count * channel->block_size;
	uint64_t pos = block * (uint64_t)channel->block_size;
	uint64_t start, end;
	uint8_t* tmp;
	ssize_t r;

	if (pos + len > d->size)
		return is_write ? EXT2_ET_SHORT_WRITE : EXT2_ET_SHORT_READ;
	if (cancel_requested)
		return EXT2_ET_OP_NOT_SUPPORTED;
	if ((pos % d->sector_size) == 0 && (len % d->sector_size) == 0) {
		r = is_write ? pwrite(d->fd, data, len, (off_t)(d->offset + pos)) : pread(d->fd, data, len, (off_t)(d->offset + pos));
		return (r == (ssize_t)len) ? 0 : (is_write ? EXT2_ET_SHORT_WRITE : EXT2_ET_SHORT_READ);
	}
	start = (pos / d->sector_size) * d->sector_size;
	end = ((pos + len + d->sector_size - 1) / d->sector_size) * d->sector_size;
	tmp = malloc(end - start);
	if (tmp == NULL)
		return EXT2_ET_NO_MEMORY;
	r = pread(d->fd, tmp, end - start, (off_t)(d->offset + start));
	if (r == (ssize_t)(end - start)) {
		if (is_write) {
			memcpy(tmp + (pos - start), data, len);
			r = pwrite(d->fd, tmp, end - start, (off_t)(d->offset + start));
		} else {
			memcpy(data, tmp + (pos - start), len);
		}
	}
	free(tmp);
	return (r == (ssize_t)(end - start)) ? 0 : (is_write ? EXT2_ET_SHORT_WRITE : EXT2_ET_SHORT_READ);
}

static errcode_t rufus_read_blk64(io_channel channel, unsigned long long block, int count, void* data)
{
	return xfer(channel, block, count, data, false);
}

static errcode_t rufus_write_blk64(io_channel channel, unsigned long long block, int count, const void* data)
{
	return xfer(channel, block, count, (void*)data, true);
}

static errcode_t rufus_read_blk(io_channel channel, unsigned long block, int count, void* data)
{
	return rufus_read_blk64(channel, block, count, data);
}

static errcode_t rufus_write_blk(io_channel channel, unsigned long block, int count, const void* data)
{
	return rufus_write_blk64(channel, block, count, data);
}

static errcode_t rufus_flush(io_channel channel)
{
	rufus_ext_io_t* d = channel->private_data;
	fsync(d->fd);
	return 0;
}

static struct struct_io_manager struct_rufus_manager = {
	.magic = EXT2_ET_MAGIC_IO_MANAGER,
	.name = "Rufus macOS I/O Manager",
	.open = rufus_open,
	.close = rufus_close,
	.set_blksize = rufus_set_blksize,
	.read_blk = rufus_read_blk,
	.read_blk64 = rufus_read_blk64,
	.write_blk = rufus_write_blk,
	.write_blk64 = rufus_write_blk64,
	.flush = rufus_flush,
};

/* Replaces the nt_io.c version */
errcode_t ext2fs_get_device_size2(const char* file, int blocksize, blk64_t* retblocks)
{
	rufus_ext_io_t d;
	if (!parse_name(file, &d))
		return EXT2_ET_BAD_DEVICE_NAME;
	*retblocks = d.size / (uint64_t)blocksize;
	return 0;
}

errcode_t ext2fs_get_device_size(const char* file, int blocksize, blk_t* retblocks)
{
	blk64_t b;
	errcode_t r = ext2fs_get_device_size2(file, blocksize, &b);
	if (r == 0)
		*retblocks = (b > 0xffffffffULL) ? 0xffffffff : (blk_t)b;
	return r;
}

/* We never let macOS mount the target while we work on it */
errcode_t ext2fs_check_mount_point(const char* file, int* mount_flags, char* mtpt, int mtlen)
{
	(void)file; (void)mtpt; (void)mtlen;
	*mount_flags = 0;
	return 0;
}

/* Referenced by libext2fs (defined in format_ext.c on Windows) */
const char* error_message(errcode_t error_code)
{
	static char msg[64];
	switch (error_code) {
	case EXT2_ET_MAGIC_EXT2FS_FILSYS: return "Wrong magic number for ext2_filsys structure";
	case EXT2_ET_SHORT_READ: return "Attempt to read block from filesystem resulted in short read";
	case EXT2_ET_SHORT_WRITE: return "Attempt to write block to filesystem resulted in short write";
	case EXT2_ET_DIR_NO_SPACE: return "No free space in the directory";
	case EXT2_ET_TOOSMALL: return "Not enough space to build proposed filesystem";
	case EXT2_ET_NO_MEMORY: return "Memory allocation failed";
	case EXT2_ET_BAD_DEVICE_NAME: return "Illegal or malformed device name";
	default:
		snprintf(msg, sizeof(msg), "Unknown ext2fs error %ld (0x%lx)", (long)error_code, (long)error_code);
		return msg;
	}
}

/* Progress callback expected by libext2fs (see format_ext.c) */
static float ext2_percent_start = 0.0f, ext2_percent_share = 0.5f;
errcode_t ext2fs_print_progress(int64_t cur_value, int64_t max_value)
{
	if (max_value > 0)
		update_progress(100.0 * (ext2_percent_start + ext2_percent_share * (double)cur_value / (double)max_value));
	return cancel_requested ? EXT2_ET_CANCEL_REQUESTED : 0;
}

/* ------------------------------------------------------------------------ */
/* FormatExtFs                                                               */
/* ------------------------------------------------------------------------ */
typedef struct {
	uint64_t max_size;
	uint32_t block_size;
	uint32_t inode_size;
	uint32_t inode_ratio;
} ext2fs_default_t;

bool format_ext_ex(part_view_t* pv, fs_type_t fs, uint32_t block_size, const char* label, bool quick, bool persistence_conf)
{
	const float reserve_ratio = 0.05f;
	const ext2fs_default_t ext2fs_default[5] = {
		{ 3 * MB, 1024, 128, 3 },       /* "floppy" */
		{ 512 * MB, 1024, 128, 2 },     /* "small" */
		{ 4 * GB, 4096, 256, 2 },       /* "default" */
		{ 16 * GB, 4096, 256, 3 },      /* "big" */
		{ 1024 * TB, 4096, 256, 4 }     /* "huge" */
	};
	const char* fs_str;
	char volume_name[128];
	struct ext2_super_block features = { 0 };
	ext2_filsys ext2fs = NULL;
	blk_t journal_size;
	blk64_t size, cur;
	uint8_t* buf;
	errcode_t r;
	int i, count;
	bool ret = false;

	if (fs == FS_EXT4) {
		uprintf("ext4 file system is not supported, defaulting to ext3");
		fs = FS_EXT3;
	}
	fs_str = fs_name[fs];
	snprintf(volume_name, sizeof(volume_name), "rufus:%d:%llu:%llu:%u", pv->dev->fd,
		(unsigned long long)pv->offset, (unsigned long long)pv->size, pv->dev->sector_size);
	uprintf("Formatting (%s)...", fs_str);
	update_status("Formatting (%s)...", fs_str);

	size = pv->size;
	for (i = 0; i < 5; i++)
		if (size < ext2fs_default[i].max_size)
			break;
	if (block_size == 0 || block_size < EXT2_MIN_BLOCK_SIZE)
		block_size = ext2fs_default[i].block_size;
	for (features.s_log_block_size = 0; EXT2_BLOCK_SIZE_BITS(&features) <= EXT2_MAX_BLOCK_LOG_SIZE; features.s_log_block_size++)
		if (EXT2_BLOCK_SIZE(&features) == (int)block_size)
			break;
	features.s_log_cluster_size = features.s_log_block_size;
	size /= block_size;
	if (size >= 0x100000000ULL) {
		uprintf("Volume size is too large for ext2 or ext3");
		goto out;
	}

	ext2fs_blocks_count_set(&features, size);
	ext2fs_r_blocks_count_set(&features, (blk64_t)(reserve_ratio * size));
	features.s_rev_level = 1;
	features.s_inode_size = ext2fs_default[i].inode_size;
	features.s_inodes_count = ((ext2fs_blocks_count(&features) >> ext2fs_default[i].inode_ratio) > UINT32_MAX) ?
		UINT32_MAX : (uint32_t)(ext2fs_blocks_count(&features) >> ext2fs_default[i].inode_ratio);
	uprintf("%u possible inodes out of %llu blocks (block size = %d)", features.s_inodes_count,
		(unsigned long long)size, EXT2_BLOCK_SIZE(&features));
	uprintf("%llu blocks (%0.1f%%) reserved for the super user", (unsigned long long)ext2fs_r_blocks_count(&features),
		reserve_ratio * 100.0f);

	ext2fs_set_feature_dir_index(&features);
	ext2fs_set_feature_filetype(&features);
	ext2fs_set_feature_large_file(&features);
	ext2fs_set_feature_sparse_super(&features);
	ext2fs_set_feature_xattr(&features);
	if (fs != FS_EXT2)
		ext2fs_set_feature_journal(&features);
	features.s_default_mount_opts = EXT2_DEFM_XATTR_USER | EXT2_DEFM_ACL;

	r = ext2fs_initialize(volume_name, EXT2_FLAG_EXCLUSIVE | EXT2_FLAG_64BITS, &features, &struct_rufus_manager, &ext2fs);
	if (r != 0) {
		uprintf("Could not initialize %s features: %s", fs_str, error_message(r));
		goto out;
	}
	buf = calloc(16, ext2fs->io->block_size);
	r = io_channel_write_blk64(ext2fs->io, 0, 16, buf);
	free(buf);
	if (r != 0) {
		uprintf("Could not zero %s superblock area: %s", fs_str, error_message(r));
		goto out;
	}

	arc4random_buf(ext2fs->super->s_uuid, sizeof(ext2fs->super->s_uuid));
	ext2fs_init_csum_seed(ext2fs);
	ext2fs->super->s_def_hash_version = EXT2_HASH_HALF_MD4;
	arc4random_buf(ext2fs->super->s_hash_seed, sizeof(ext2fs->super->s_hash_seed));
	ext2fs->super->s_max_mnt_count = -1;
	ext2fs->super->s_creator_os = EXT2_OS_LINUX;
	ext2fs->super->s_errors = EXT2_ERRORS_CONTINUE;
	if (label != NULL)
		snprintf((char*)ext2fs->super->s_volume_name, sizeof(ext2fs->super->s_volume_name), "%s", label);

	r = ext2fs_allocate_tables(ext2fs);
	if (r != 0) {
		uprintf("Could not allocate %s tables: %s", fs_str, error_message(r));
		goto out;
	}
	r = ext2fs_convert_subcluster_bitmap(ext2fs, &ext2fs->block_map);
	if (r != 0) {
		uprintf("Could not set %s cluster bitmap: %s", fs_str, error_message(r));
		goto out;
	}

	ext2_percent_start = 0.0f;
	ext2_percent_share = (fs == FS_EXT2) ? 1.0f : 0.5f;
	uprintf("Creating %d inode sets", ext2fs->group_desc_count);
	for (i = 0; i < (int)ext2fs->group_desc_count; i++) {
		if (ext2fs_print_progress((int64_t)i, (int64_t)ext2fs->group_desc_count))
			goto out;
		cur = ext2fs_inode_table_loc(ext2fs, i);
		count = ext2fs_div_ceil((ext2fs->super->s_inodes_per_group - ext2fs_bg_itable_unused(ext2fs, i))
			* EXT2_INODE_SIZE(ext2fs->super), EXT2_BLOCK_SIZE(ext2fs->super));
		r = ext2fs_zero_blocks2(ext2fs, cur, count, &cur, &count);
		if (r != 0) {
			uprintf("Could not zero inode set at position %llu (%d blocks): %s", (unsigned long long)cur, count, error_message(r));
			goto out;
		}
	}

	r = ext2fs_mkdir(ext2fs, EXT2_ROOT_INO, EXT2_ROOT_INO, 0);
	if (r != 0) {
		uprintf("Failed to create %s root dir: %s", fs_str, error_message(r));
		goto out;
	}
	ext2fs->umask = 077;
	r = ext2fs_mkdir(ext2fs, EXT2_ROOT_INO, 0, "lost+found");
	if (r != 0) {
		uprintf("Failed to create %s 'lost+found' dir: %s", fs_str, error_message(r));
		goto out;
	}
	for (i = EXT2_ROOT_INO + 1; i < (int)EXT2_FIRST_INODE(ext2fs->super); i++)
		ext2fs_inode_alloc_stats(ext2fs, i, 1);
	ext2fs_mark_ib_dirty(ext2fs);
	r = ext2fs_mark_inode_bitmap2(ext2fs->inode_map, EXT2_BAD_INO);
	if (r != 0) {
		uprintf("Could not set inode bitmaps: %s", error_message(r));
		goto out;
	}
	ext2fs_inode_alloc_stats(ext2fs, EXT2_BAD_INO, 1);
	r = ext2fs_update_bb_inode(ext2fs, NULL);
	if (r != 0) {
		uprintf("Could not set inode stats: %s", error_message(r));
		goto out;
	}

	if (fs != FS_EXT2) {
		ext2_percent_start = 0.5f;
		journal_size = ext2fs_default_journal_size(ext2fs_blocks_count(ext2fs->super));
		journal_size /= 2;
		uprintf("Creating %u journal blocks", journal_size);
		r = ext2fs_add_journal_inode(ext2fs, journal_size, EXT2_MKJOURNAL_NO_MNT_CHECK | (quick ? EXT2_MKJOURNAL_LAZYINIT : 0));
		if (r != 0) {
			uprintf("Could not create %s journal: %s", fs_str, error_message(r));
			goto out;
		}
	}

	if (persistence_conf) {
		/* Debian Live wants a 'persistence.conf' with "/ union" (and the LF!) */
		const char* name = "persistence.conf", data[] = "/ union\n";
		unsigned int written = 0, fsize = sizeof(data) - 1;
		ext2_file_t ext2fd;
		ext2_ino_t inode_id;
		uint32_t ctime = (uint32_t)time(NULL);
		struct ext2_inode inode = { 0 };
		inode.i_mode = 0100644;
		inode.i_links_count = 1;
		inode.i_atime = ctime;
		inode.i_ctime = ctime;
		inode.i_mtime = ctime;
		inode.i_size = fsize;
		ext2fs_namei(ext2fs, EXT2_ROOT_INO, EXT2_ROOT_INO, name, &inode_id);
		ext2fs_new_inode(ext2fs, EXT2_ROOT_INO, 010755, 0, &inode_id);
		ext2fs_link(ext2fs, EXT2_ROOT_INO, name, inode_id, EXT2_FT_REG_FILE);
		ext2fs_inode_alloc_stats(ext2fs, inode_id, 1);
		ext2fs_write_new_inode(ext2fs, inode_id, &inode);
		ext2fs_file_open(ext2fs, inode_id, EXT2_FILE_WRITE, &ext2fd);
		if (ext2fs_file_write(ext2fd, data, fsize, &written) != 0 || written != fsize)
			uprintf("Error: Could not create '%s' file", name);
		else
			uprintf("Created '%s' file", name);
		ext2fs_file_close(ext2fd);
	}

	r = ext2fs_close(ext2fs);
	if (r != 0) {
		uprintf("Could not create %s volume: %s", fs_str, error_message(r));
		goto out;
	}
	ext2fs = NULL;
	update_progress(100.0);
	ret = true;
out:
	if (ext2fs != NULL)
		ext2fs_free(ext2fs);
	return ret;
}

bool format_ext(part_view_t* pv, fs_type_t fs, uint32_t cluster_size, const char* label)
{
	/* ext cluster sizes in the UI are placeholders ("default") */
	return format_ext_ex(pv, fs, (cluster_size >= 1024 && cluster_size <= 65536) ? cluster_size : 0, label, true, false);
}
