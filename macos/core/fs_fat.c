/*
 * Rufus for macOS: FAT16/FAT32/exFAT formatting and file writing (via FatFs)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * On Windows, Rufus formats through fmifs/VDS (or its own Large FAT32
 * formatter) and then copies files onto the mounted drive letter. Here the
 * volume is formatted and populated directly through the raw device with
 * FatFs, so it never has to be mounted by macOS during the process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "rufus_core.h"
#include "ff.h"
#include "diskio.h"

/* FatFs #defines FS_FAT12..FS_EXFAT as its own volume types, which would shadow
 * Rufus' fs_type_t enum. Keep Rufus' meaning and name FatFs' exFAT type apart. */
#undef FS_FAT12
#undef FS_FAT16
#undef FS_FAT32
#undef FS_EXFAT
#define FATFS_TYPE_EXFAT 4

#define FAT32_CLUSTER_THRESHOLD 1.011
#define MAX_FAT32_SIZE          (2 * TB)

/* ------------------------------------------------------------------------ */
/* Cluster sizes: same tables as SetFileSystemAndClusterSize() in rufus.c   */
/* ------------------------------------------------------------------------ */
void cluster_sizes(fs_type_t fs, uint64_t disk_size, uint32_t sector_size, uint32_t* allowed, uint32_t* def)
{
	uint64_t i;
	*allowed = 0;
	*def = 0;

	switch (fs) {
	case FS_FAT16:
		if (disk_size >= 4 * GB)
			break;
		*allowed = 0x00001E00;
		for (i = 32; i <= 4096; i <<= 1) {
			if (disk_size < i * MB) {
				*def = 16 * (uint32_t)i;
				break;
			}
			*allowed <<= 1;
		}
		*allowed &= 0x0001FE00;
		break;
	case FS_FAT32:
		if (disk_size < 32 * MB || disk_size >= MAX_FAT32_SIZE)
			break;
		*allowed = 0x000001F8;
		for (i = 32; i <= 32 * 1024; i <<= 1) {
			if ((double)disk_size < (double)(i * MB) * FAT32_CLUSTER_THRESHOLD) {
				*def = 8 * (uint32_t)i;
				break;
			}
			*allowed <<= 1;
		}
		*allowed &= 0x0001FE00;
		if (disk_size >= 256 * MB && disk_size < 32 * GB) {
			for (i = 8; i <= 32; i <<= 1) {
				if ((double)disk_size < (double)(i * GB) * FAT32_CLUSTER_THRESHOLD) {
					*def = (uint32_t)(i / 2) * KB;
					break;
				}
			}
		}
		if (disk_size >= 32 * GB) {
			*allowed &= 0x0001C000;
			*def = 0x00008000;
		}
		break;
	case FS_NTFS:
		if (disk_size >= 256 * TB)
			break;
		*allowed = 0x0001F000;
		for (i = 16; i <= 256; i <<= 1) {
			if (disk_size < i * TB) {
				*def = (uint32_t)(i / 4) * KB;
				break;
			}
		}
		break;
	case FS_EXFAT:
		if (disk_size >= 256 * TB)
			break;
		*allowed = 0x03FFFE00;
		if (disk_size < 256 * MB)
			*def = 4 * KB;
		else if (disk_size < 32 * GB)
			*def = 32 * KB;
		else
			*def = 128 * KB;
		break;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		if (disk_size >= 256 * MB) {
			*allowed = 0x100;	/* SINGLE_CLUSTERSIZE_DEFAULT */
			*def = 0x100;
		}
		return;
	default:
		return;
	}
	/* Remove all cluster sizes that are below the sector size */
	*allowed &= ~(sector_size - 1);
	if ((*def & *allowed) == 0)
		*def = *allowed & (uint32_t)(-(int32_t)*allowed);
}

uint32_t default_cluster_size(fs_type_t fs, uint64_t part_size)
{
	uint32_t allowed, def;
	cluster_sizes(fs, part_size, 512, &allowed, &def);
	return def;
}

/* ToValidLabel() from format.c */
void to_valid_label(char* label, bool fat)
{
	static const char unauthorized[] = "*?,;:/\\|+=<>[]\"";
	char out[64];
	size_t i, k = 0, underscores = 0;

	for (i = 0; label[i] != 0 && k < sizeof(out) - 1; i++) {
		unsigned char c = (unsigned char)label[i];
		if (fat) {
			if (c >= 0x80) {
				/* Skip UTF-8 continuation bytes so that one character = one '_' */
				if ((c & 0xc0) != 0x80)
					out[k++] = '_';
				continue;
			}
			if (strchr(unauthorized, c) != NULL)
				continue;
		}
		if (c == '\t' || c == '.') {
			out[k++] = '_';
			continue;
		}
		out[k++] = fat ? (char)toupper(c) : (char)c;
	}
	out[k] = 0;
	if (fat) {
		out[11] = 0;
		for (i = 0; out[i] != 0; i++)
			if (out[i] == '_')
				underscores++;
		if (i < 2 * underscores) {
			uprintf("FAT label is mostly underscores. Using 'RUFUS' label instead.");
			snprintf(out, sizeof(out), "RUFUS");
		}
	} else if (strlen(out) > 32) {
		out[32] = 0;
	}
	strcpy(label, out);
}

/* ------------------------------------------------------------------------ */
/* FatFs disk I/O layer: drive 0 is the currently attached partition view   */
/* ------------------------------------------------------------------------ */
static part_view_t* fat_pv = NULL;

DSTATUS disk_initialize(BYTE pdrv) { return (pdrv == 0 && fat_pv != NULL) ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE pdrv) { return (pdrv == 0 && fat_pv != NULL) ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
	uint32_t ss;
	if (pdrv != 0 || fat_pv == NULL)
		return RES_NOTRDY;
	ss = fat_pv->dev->sector_size;
	return rdev_read(fat_pv->dev, fat_pv->offset + (uint64_t)sector * ss, buff, (size_t)count * ss) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
	uint32_t ss;
	if (pdrv != 0 || fat_pv == NULL)
		return RES_NOTRDY;
	if (cancel_requested)
		return RES_ERROR;
	ss = fat_pv->dev->sector_size;
	return rdev_write(fat_pv->dev, fat_pv->offset + (uint64_t)sector * ss, buff, (size_t)count * ss) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
	if (pdrv != 0 || fat_pv == NULL)
		return RES_NOTRDY;
	switch (cmd) {
	case CTRL_SYNC:
		return rdev_sync(fat_pv->dev) ? RES_OK : RES_ERROR;
	case GET_SECTOR_COUNT:
		*(LBA_t*)buff = fat_pv->size / fat_pv->dev->sector_size;
		return RES_OK;
	case GET_SECTOR_SIZE:
		*(WORD*)buff = (WORD)fat_pv->dev->sector_size;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD*)buff = 1;
		return RES_OK;
	default:
		return RES_PARERR;
	}
}

DWORD get_fattime(void)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	return ((DWORD)(tm.tm_year - 80) << 25) | ((DWORD)(tm.tm_mon + 1) << 21) | ((DWORD)tm.tm_mday << 16) |
		((DWORD)tm.tm_hour << 11) | ((DWORD)tm.tm_min << 5) | ((DWORD)tm.tm_sec >> 1);
}

static const char* fresult_str(FRESULT fr)
{
	static const char* str[] = { "OK", "Disk error", "Internal error", "Not ready", "No file", "No path",
		"Invalid name", "Denied", "Exists", "Invalid object", "Write protected", "Invalid drive",
		"Not enabled", "No file system", "mkfs aborted", "Timeout", "Locked", "Not enough core",
		"Too many open files", "Invalid parameter" };
	return (fr < sizeof(str) / sizeof(str[0])) ? str[fr] : "Unknown error";
}

/* ------------------------------------------------------------------------ */
/* Formatting                                                                */
/* ------------------------------------------------------------------------ */

/* FatFs formats "super floppy" volumes that start at sector 0 of the drive it
 * sees. For BIOS booting, the boot record must know the partition's real start
 * ("hidden sectors"), so patch that in afterwards. */
static bool fix_hidden_sectors(part_view_t* pv, fs_type_t fs)
{
	uint32_t ss = pv->dev->sector_size;
	uint64_t hidden = pv->offset / ss;
	uint8_t* buf = malloc(12 * ss);
	bool r = false;
	int copy, i;

	if (buf == NULL)
		return false;
	if (fs == FS_EXFAT) {
		/* exFAT: PartitionOffset at 0x40, and the boot region checksum must be recomputed */
		for (copy = 0; copy < 2; copy++) {
			uint64_t base = pv->offset + (uint64_t)copy * 12 * ss;
			uint32_t sum = 0;
			if (!rdev_read(pv->dev, base, buf, 12 * ss))
				goto out;
			memcpy(&buf[0x40], &hidden, 8);
			for (i = 0; i < 11 * (int)ss; i++) {
				if (i == 106 || i == 107 || i == 112)
					continue;
				sum = ((sum & 1) ? 0x80000000 : 0) + (sum >> 1) + buf[i];
			}
			for (i = 0; i < (int)ss / 4; i++)
				memcpy(&buf[11 * ss + i * 4], &sum, 4);
			if (!rdev_write(pv->dev, base, buf, 12 * ss))
				goto out;
		}
	} else {
		uint32_t h32 = (uint32_t)hidden;
		if (hidden > 0xffffffffULL)
			h32 = 0xffffffff;
		if (!rdev_read(pv->dev, pv->offset, buf, ss))
			goto out;
		memcpy(&buf[0x1c], &h32, 4);
		/* Heads & sectors per track that BIOSes expect with LBA */
		buf[0x18] = 63; buf[0x19] = 0;
		buf[0x1a] = 255; buf[0x1b] = 0;
		if (!rdev_write(pv->dev, pv->offset, buf, ss))
			goto out;
		if (fs == FS_FAT32) {
			/* Backup boot sector */
			uint16_t bk = buf[0x32] | (buf[0x33] << 8);
			if (bk != 0 && !rdev_write(pv->dev, pv->offset + (uint64_t)bk * ss, buf, ss))
				goto out;
		}
	}
	r = true;
out:
	free(buf);
	return r;
}

bool format_fat(part_view_t* pv, fs_type_t fs, uint32_t cluster_size, const char* label, bool quick)
{
	MKFS_PARM opt = { 0 };
	FATFS* fatfs = NULL;
	const size_t work_len = 4 * MB;
	void* work = NULL;
	char vlabel[64];
	FRESULT fr;
	bool r = false;

	if (fs != FS_FAT16 && fs != FS_FAT32 && fs != FS_EXFAT) {
		uprintf("format_fat: unsupported file system");
		return false;
	}
	if (cluster_size == 0)
		cluster_size = default_cluster_size(fs, pv->size);
	uprintf("Formatting (%s)...", fs_name[fs]);
	update_status("Formatting (%s)...", fs == FS_FAT32 && pv->size > 32 * GB ? "Large FAT32" : fs_name[fs]);
	uprintf("Using cluster size: %u bytes", cluster_size);

	if (!quick) {
		/* A full format, like on Windows, writes the whole partition */
		const size_t chunk = 4 * MB;
		uint64_t done = 0;
		uint8_t* zero = calloc(1, chunk);
		if (zero == NULL)
			return false;
		update_status("Zeroing partition...");
		while (done < pv->size) {
			size_t n = (pv->size - done > chunk) ? chunk : (size_t)(pv->size - done);
			if (cancel_requested || !rdev_write(pv->dev, pv->offset + done, zero, n)) {
				free(zero);
				return false;
			}
			done += n;
			update_progress_bytes(NULL, done, pv->size);
		}
		free(zero);
		update_status("Formatting (%s)...", fs_name[fs]);
	}

	fat_pv = pv;
	opt.fmt = FM_SFD | (fs == FS_FAT16 ? FM_FAT : fs == FS_FAT32 ? FM_FAT32 : FM_EXFAT);
	opt.n_fat = (fs == FS_EXFAT) ? 1 : 2;
	opt.align = 0;
	opt.n_root = 512;
	opt.au_size = cluster_size;
	work = malloc(work_len);
	fatfs = calloc(1, sizeof(FATFS));
	if (work == NULL || fatfs == NULL)
		goto out;
	update_progress(-1);
	fr = f_mkfs("0:", &opt, work, (UINT)work_len);
	if (fr != FR_OK) {
		uprintf("Could not format volume: %s", fresult_str(fr));
		goto out;
	}
	if (!fix_hidden_sectors(pv, fs)) {
		uprintf("Could not set the boot record's hidden sectors");
		goto out;
	}
	/* Set the label */
	snprintf(vlabel, sizeof(vlabel), "%s", (label != NULL) ? label : "");
	if (vlabel[0] != 0) {
		fr = f_mount(fatfs, "0:", 1);
		if (fr == FR_OK)
			fr = f_setlabel(vlabel);
		f_unmount("0:");
		if (fr != FR_OK)
			uprintf("Could not set label '%s': %s", vlabel, fresult_str(fr));
		else
			uprintf("Volume label set to '%s'", vlabel);
	}
	update_progress(100.0);
	r = true;
out:
	free(work);
	free(fatfs);
	fat_pv = NULL;
	return r;
}

/* ------------------------------------------------------------------------ */
/* File sink                                                                 */
/* ------------------------------------------------------------------------ */
typedef struct {
	FATFS fs;
	part_view_t* pv;
} fat_sink_priv_t;

static bool fat_mkdir(sink_t* s, const char* path)
{
	char p[1024];
	FRESULT fr;
	char* c;
	(void)s;
	snprintf(p, sizeof(p), "%s", path);
	/* Create intermediate directories too */
	for (c = p + 1; *c != 0; c++) {
		if (*c == '/') {
			*c = 0;
			fr = f_mkdir(p);
			*c = '/';
			if (fr != FR_OK && fr != FR_EXIST) {
				uprintf("Could not create directory '%s': %s", p, fresult_str(fr));
				return false;
			}
		}
	}
	fr = f_mkdir(p);
	if (fr != FR_OK && fr != FR_EXIST) {
		uprintf("Could not create directory '%s': %s", p, fresult_str(fr));
		return false;
	}
	return true;
}

static void* fat_create(sink_t* s, const char* path, uint64_t size)
{
	FIL* fp = calloc(1, sizeof(FIL));
	FRESULT fr;
	(void)s;
	if (fp == NULL)
		return NULL;
	fr = f_open(fp, path, FA_CREATE_ALWAYS | FA_WRITE);
	if (fr == FR_NO_PATH) {
		/* Parent directory missing: create it and retry */
		char dir[1024];
		char* slash;
		snprintf(dir, sizeof(dir), "%s", path);
		slash = strrchr(dir, '/');
		if (slash != NULL && slash != dir) {
			*slash = 0;
			if (fat_mkdir(s, dir))
				fr = f_open(fp, path, FA_CREATE_ALWAYS | FA_WRITE);
		}
	}
	if (fr != FR_OK) {
		uprintf("Could not create '%s': %s", path, fresult_str(fr));
		free(fp);
		return NULL;
	}
	/* Pre-allocate contiguous clusters when possible (faster, and avoids fragmentation) */
	if (size > 0 && f_expand(fp, (FSIZE_t)size, 1) != FR_OK)
		(void)0;	/* Not fatal: the file will just be allocated as we go */
	return fp;
}

static bool fat_write(sink_t* s, void* file, const void* buf, size_t len)
{
	UINT wb = 0;
	FRESULT fr = f_write((FIL*)file, buf, (UINT)len, &wb);
	(void)s;
	if (fr != FR_OK || wb != len) {
		uprintf("Write error: %s", fr != FR_OK ? fresult_str(fr) : "disk full");
		return false;
	}
	return true;
}

static bool fat_close(sink_t* s, void* file)
{
	FRESULT fr = f_close((FIL*)file);
	(void)s;
	free(file);
	return fr == FR_OK;
}

static bool fat_set_label(sink_t* s, const char* label)
{
	(void)s;
	return f_setlabel(label) == FR_OK;
}

static bool fat_unmount(sink_t* s)
{
	fat_sink_priv_t* priv = s->priv;
	f_unmount("0:");
	rdev_sync(priv->pv->dev);
	fat_pv = NULL;
	free(priv);
	free(s);
	return true;
}

sink_t* fat_sink_open(part_view_t* pv)
{
	sink_t* s = calloc(1, sizeof(sink_t));
	fat_sink_priv_t* priv = calloc(1, sizeof(fat_sink_priv_t));
	FRESULT fr;

	if (s == NULL || priv == NULL)
		goto fail;
	fat_pv = pv;
	priv->pv = pv;
	fr = f_mount(&priv->fs, "0:", 1);
	if (fr != FR_OK) {
		uprintf("Could not access FAT volume: %s", fresult_str(fr));
		goto fail;
	}
	s->priv = priv;
	s->mkdir = fat_mkdir;
	s->create = fat_create;
	s->write = fat_write;
	s->close = fat_close;
	s->set_label = fat_set_label;
	s->unmount = fat_unmount;
	s->max_file_size = (priv->fs.fs_type == FATFS_TYPE_EXFAT) ? UINT64_MAX : 0xffffffffULL;
	return s;
fail:
	fat_pv = NULL;
	free(priv);
	free(s);
	return NULL;
}

/* Used by the syslinux installer, which needs FAT attribute changes */
bool fat_set_attributes(const char* path, uint8_t attr)
{
	return f_chmod(path, attr, AM_RDO | AM_HID | AM_SYS | AM_ARC) == FR_OK;
}

bool sink_write_buffer(sink_t* s, const char* path, const void* buf, size_t len)
{
	void* f = s->create(s, path, len);
	bool r;
	if (f == NULL)
		return false;
	r = (len == 0) || s->write(s, f, buf, len);
	return s->close(s, f) && r;
}
