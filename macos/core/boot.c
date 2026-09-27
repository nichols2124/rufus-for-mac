/*
 * Rufus for macOS: boot records, Syslinux and FreeDOS installation
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Port of WriteMBR(), WriteSBR() and WritePBR() from src/format.c,
 * InstallSyslinux() from src/syslinux.c and ExtractFreeDOS() from src/dos.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "rufus_core.h"
#include "file.h"
#include "br.h"
#include "fat16.h"
#include "fat32.h"
#include "ntfs.h"
#include "partition_info.h"
#include "libfat.h"
#include "syslinux.h"
#include "syslxfs.h"
#include "setadv.h"

char resource_dir[1024] = "";

/* Syslinux globals (from src/syslinux.c) */
unsigned char* syslinux_ldlinux[2] = { NULL, NULL };
unsigned long syslinux_ldlinux_len[2];
uint32_t SECTOR_SHIFT = 9;
uint32_t SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_SHIFT = 9;
uint32_t LIBFAT_SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_MASK = 511;

bool fat_set_attributes(const char* path, uint8_t attr);
#define FAT_AM_RDO 0x01
#define FAT_AM_HID 0x02
#define FAT_AM_SYS 0x04

void* load_resource(const char* name, size_t* size)
{
	char path[1200];
	FILE* fp;
	long len;
	void* buf;

	snprintf(path, sizeof(path), "%s/%s", resource_dir, name);
	fp = fopen(path, "rb");
	if (fp == NULL) {
		uprintf("Could not open resource '%s'", path);
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	buf = malloc((size_t)len + 1);
	if (buf != NULL && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		free(buf);
		buf = NULL;
	}
	fclose(fp);
	if (buf != NULL) {
		((uint8_t*)buf)[len] = 0;
		*size = (size_t)len;
	}
	return buf;
}

/* ------------------------------------------------------------------------ */
/* MBR                                                                       */
/* ------------------------------------------------------------------------ */
bool write_mbr(rdev_t* dev, const layout_t* layout, boot_type_t bt, const image_report_t* r,
	fs_type_t fs, target_t tt, bool use_rufus_mbr)
{
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;
	const char* using_msg = "Using %s MBR";
	bool needs_masquerading = (bt == BT_IMAGE) && r != NULL && HAS_WINPE(r);
	uint8_t buf[512];
	bool ok;

	fake_fd._handle = dev;
	set_bytes_per_sector(dev->sector_size);

	if (layout->style == PS_GPT) {
		uprintf(using_msg, "Rufus protective");
		ok = write_rufus_msg_mbr(fp);
		goto sbr;
	}

	if (!rdev_read(dev, 0, buf, sizeof(buf)))
		return false;
	if ((bt != BT_NON_BOOTABLE) && (tt == TT_BIOS)) {
		buf[0x1be] = needs_masquerading ? 0x81 : 0x80;
		uprintf("Set bootable USB partition as 0x%02X", buf[0x1be]);
		if (!rdev_write(dev, 0, buf, sizeof(buf)))
			return false;
	}

	if ((bt == BT_NON_BOOTABLE) || (tt == TT_UEFI)) {
		uprintf(using_msg, "Zeroed");
		ok = write_zero_mbr(fp);
	} else if ((bt == BT_SYSLINUX_V6) || (bt == BT_IMAGE && HAS_SYSLINUX(r))) {
		uprintf(using_msg, "Syslinux");
		ok = write_syslinux_mbr(fp);
	} else if (bt == BT_IMAGE && r->has_grub2) {
		uprintf(using_msg, "Grub 2.0");
		ok = write_grub2_mbr(fp);
	} else if (bt == BT_IMAGE && r->has_grub4dos) {
		uprintf(using_msg, "Grub4DOS");
		ok = write_grub4dos_mbr(fp);
	} else if (bt == BT_IMAGE && r->has_kolibrios && IS_FAT(fs)) {
		uprintf(using_msg, "KolibriOS");
		ok = write_kolibrios_mbr(fp);
	} else if (needs_masquerading || use_rufus_mbr) {
		uprintf(using_msg, "Rufus");
		ok = write_rufus_mbr(fp);
	} else {
		uprintf(using_msg, "Windows 7");
		ok = write_win7_mbr(fp);
	}

sbr:
	if (!ok)
		return false;

	/* Secondary boot record (WriteSBR) */
	{
		uint64_t br_size = 0x200, max_size = layout->part[0].offset;
		void* sbr = NULL;
		size_t size = 0;
		int sub_type = (int)bt;

		if (bt == BT_IMAGE && !HAS_SYSLINUX(r)) {
			if (r->has_grub4dos)
				sub_type = 100;	/* Grub4DOS */
			if (r->has_grub2)
				sub_type = 101;	/* GRUB2 */
		}
		if (bt != BT_NON_BOOTABLE && layout->style == PS_GPT)
			sub_type = 102;	/* protective message */

		switch (sub_type) {
		case 100: {
			uint8_t* grldr = load_resource("grub/grldr.mbr", &size);
			if (grldr == NULL || size <= br_size) {
				free(grldr);
				uprintf("grldr.mbr is either not present or too small");
				return false;
			}
			uprintf("Writing Grub4Dos SBR");
			sbr = malloc(size - br_size);
			memcpy(sbr, grldr + br_size, size - br_size);
			size -= br_size;
			free(grldr);
			break;
		}
		case 101:
			uprintf("Writing Grub 2.0 SBR (from embedded)");
			sbr = load_resource("grub2/core.img", &size);
			if (sbr == NULL)
				return false;
			break;
		case 102:
			uprintf("Writing protective message SBR");
			br_size = 17 * KB;
			sbr = load_resource("mbr/msg.txt", &size);
			if (sbr == NULL)
				return false;
			break;
		default:
			return true;
		}
		if (br_size + size > max_size) {
			uprintf("  SBR size is too large - You may need to uncheck 'Add fixes for old BIOSes'.");
			free(sbr);
			return false;
		}
		ok = rdev_write(dev, br_size, sbr, size);
		free(sbr);
	}
	return ok;
}

/* ------------------------------------------------------------------------ */
/* Partition boot record                                                     */
/* ------------------------------------------------------------------------ */
static const char* bt_to_name(boot_type_t bt, const image_report_t* r)
{
	if (bt == BT_FREEDOS)
		return "FreeDOS";
	return (bt == BT_IMAGE && r != NULL && r->has_kolibrios) ? "KolibriOS" : "Standard";
}

bool write_pbr(part_view_t* pv, fs_type_t fs, boot_type_t bt, const image_report_t* r)
{
	FAKE_FD fake_fd = { 0 };
	FILE* fp = (FILE*)&fake_fd;
	const char* using_msg = "Using %s %s partition boot record";
	int i;

	fake_fd._handle = pv->dev;
	fake_fd._offset = pv->offset;
	set_bytes_per_sector(pv->dev->sector_size);

	switch (fs) {
	case FS_FAT16:
		uprintf(using_msg, bt_to_name(bt, r), "FAT16");
		if (!is_fat_16_fs(fp)) {
			uprintf("New volume does not have a FAT16 boot sector - aborting");
			return false;
		}
		if (bt == BT_FREEDOS) {
			if (!write_fat_16_fd_br(fp, 0)) return false;
		} else if (!write_fat_16_br(fp, 0)) {
			return false;
		}
		return write_partition_physical_disk_drive_id_fat16(fp);
	case FS_FAT32:
		uprintf(using_msg, bt_to_name(bt, r), "FAT32");
		for (i = 0; i < 2; i++) {
			if (!is_fat_32_fs(fp)) {
				uprintf("New volume does not have a %s FAT32 boot sector - aborting", i ? "secondary" : "primary");
				return false;
			}
			uprintf("Confirmed new volume has a %s FAT32 boot sector", i ? "secondary" : "primary");
			if (bt == BT_FREEDOS) {
				if (!write_fat_32_fd_br(fp, 0)) return false;
			} else if (bt == BT_IMAGE && r->has_kolibrios) {
				if (!write_fat_32_kos_br(fp, 0)) return false;
			} else if (bt == BT_IMAGE && HAS_BOOTMGR(r)) {
				if (!write_fat_32_pe_br(fp, 0)) return false;
			} else if (bt == BT_IMAGE && HAS_WINPE(r)) {
				if (!write_fat_32_nt_br(fp, 0)) return false;
			} else {
				if (!write_fat_32_br(fp, 0)) return false;
			}
			if (!write_partition_physical_disk_drive_id_fat32(fp))
				return false;
			fake_fd._offset += 6 * pv->dev->sector_size;
		}
		return true;
	case FS_NTFS:
		uprintf(using_msg, bt_to_name(bt, r), "NTFS");
		if (!is_ntfs_fs(fp)) {
			uprintf("New volume does not have an NTFS boot sector - aborting");
			return false;
		}
		uprintf("Confirmed new volume has an NTFS boot sector");
		if (!write_ntfs_br(fp))
			return false;
		/* Keep the backup boot sector (the last sector of the volume) identical to the
		 * primary one, so that chkdsk/ntfsfix don't flag the volume */
		{
			uint32_t ss = pv->dev->sector_size;
			uint8_t* bs = malloc(ss);
			uint64_t total;
			bool ok = (bs != NULL) && rdev_read(pv->dev, pv->offset, bs, ss);
			if (ok) {
				memcpy(&total, &bs[0x28], sizeof(total));
				ok = (pv->offset + total * ss + ss <= pv->offset + pv->size) &&
					rdev_write(pv->dev, pv->offset + total * ss, bs, ss);
			}
			if (!ok)
				uprintf("WARNING: Could not update the NTFS backup boot sector");
			free(bs);
		}
		return true;
	case FS_EXFAT:
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		return true;
	default:
		uprintf("Unsupported FS for FS BR processing - aborting");
		return false;
	}
}

/* ------------------------------------------------------------------------ */
/* Syslinux (FAT only)                                                       */
/* ------------------------------------------------------------------------ */
static part_view_t* sl_pv;

static int libfat_readfile(intptr_t pp, void* buf, size_t secsize, libfat_sector_t sector)
{
	(void)pp;
	return rdev_read(sl_pv->dev, sl_pv->offset + (uint64_t)sector * secsize, buf, secsize) ? (int)secsize : 0;
}

bool install_syslinux(part_view_t* pv, sink_t* sink, const image_report_t* r)
{
	const bool use_v6 = (SL_MAJOR_OF(r->sl_version) >= 5);
	const char* res[2][2] = { { "syslinux/ldlinux_v4.sys", "syslinux/ldlinux_v4.bss" },
	                          { "syslinux/ldlinux_v6.sys", "syslinux/ldlinux_v6.bss" } };
	uint8_t* sectbuf = NULL;
	struct libfat_filesystem* lf_fs;
	libfat_sector_t s, *sectors = NULL;
	uint32_t ldlinux_cluster;
	int i, nsectors, ldlinux_sectors, fs_stype, w;
	const char* errmsg;
	char cfg_dir[128];
	uint8_t* ldlinux_file = NULL;
	size_t ldlinux_file_len;
	bool ret = false;

	uprintf("Installing Syslinux %s...", use_v6 ? "v6" : "v4");
	update_status("Installing Syslinux...");
	if (use_v6 && r->sl_version != 0x0604 && r->sl_version != 0x0603)
		uprintf("WARNING: The ISO uses Syslinux %s but the embedded version is 6.04. "
			"If BIOS boot fails, try writing in DD Image mode.", r->sl_version_str);
	sl_pv = pv;

	/* 4K sector size support */
	SECTOR_SIZE = pv->dev->sector_size;
	for (SECTOR_SHIFT = 0; (1U << SECTOR_SHIFT) < SECTOR_SIZE; SECTOR_SHIFT++);
	LIBFAT_SECTOR_SHIFT = SECTOR_SHIFT;
	LIBFAT_SECTOR_SIZE = SECTOR_SIZE;
	LIBFAT_SECTOR_MASK = SECTOR_SIZE - 1;

	sectbuf = malloc(SECTOR_SIZE);
	if (sectbuf == NULL || !rdev_read(pv->dev, pv->offset, sectbuf, SECTOR_SIZE))
		goto out;
	if ((errmsg = syslinux_check_bootsect(sectbuf, &fs_stype)) != NULL) {
		uprintf("Error: %s", errmsg);
		goto out;
	}
	syslinux_reset_adv(syslinux_adv);

	for (i = 0; i < 2; i++) {
		size_t len = 0;
		free(syslinux_ldlinux[i]);
		syslinux_ldlinux[i] = load_resource(res[use_v6 ? 1 : 0][i], &len);
		if (syslinux_ldlinux[i] == NULL)
			goto out;
		syslinux_ldlinux_len[i] = (unsigned long)len;
	}

	/* Write ldlinux.sys (image + ADV) */
	ldlinux_file_len = syslinux_ldlinux_len[0] + 2 * ADV_SIZE;
	ldlinux_file = malloc(ldlinux_file_len);
	if (ldlinux_file == NULL)
		goto out;
	memcpy(ldlinux_file, syslinux_ldlinux[0], syslinux_ldlinux_len[0]);
	memcpy(ldlinux_file + syslinux_ldlinux_len[0], syslinux_adv, 2 * ADV_SIZE);
	if (!sink_write_buffer(sink, "/ldlinux.sys", ldlinux_file, ldlinux_file_len)) {
		uprintf("Could not write 'ldlinux.sys'");
		goto out;
	}
	uprintf("Successfully wrote 'ldlinux.sys'");

	/* Map the file's sectors */
	ldlinux_sectors = (int)((ldlinux_file_len + SECTOR_SIZE - 1) >> SECTOR_SHIFT);
	sectors = calloc((size_t)ldlinux_sectors, sizeof(*sectors));
	if (sectors == NULL)
		goto out;
	lf_fs = libfat_open(libfat_readfile, 0);
	if (lf_fs == NULL) {
		uprintf("Syslinux FAT access error");
		goto out;
	}
	ldlinux_cluster = libfat_searchdir(lf_fs, 0, "LDLINUX SYS", NULL);
	nsectors = 0;
	s = libfat_clustertosector(lf_fs, ldlinux_cluster);
	while (s && nsectors < ldlinux_sectors) {
		sectors[nsectors++] = s;
		s = libfat_nextsector(lf_fs, s);
	}
	libfat_close(lf_fs);

	/* Patch ldlinux.sys with the config directory and the sector map */
	snprintf(cfg_dir, sizeof(cfg_dir), "%s", r->cfg_path);
	for (i = (int)strlen(cfg_dir); i > 0 && cfg_dir[i] != '/'; i--);
	cfg_dir[(i > 0) ? i : 0] = 0;
	w = syslinux_patch(sectors, nsectors, 0, 0, (i > 0) ? cfg_dir : NULL, NULL);
	if (w < 0) {
		uprintf("WARNING: Could not patch Syslinux files.");
		goto out;
	}
	memcpy(ldlinux_file, syslinux_ldlinux[0], syslinux_ldlinux_len[0]);
	if (!sink_write_buffer(sink, "/ldlinux.sys", ldlinux_file, ldlinux_file_len)) {
		uprintf("Could not rewrite 'ldlinux.sys'");
		goto out;
	}
	fat_set_attributes("/ldlinux.sys", FAT_AM_RDO | FAT_AM_HID | FAT_AM_SYS);

	/* Make the Syslinux boot sector */
	if (!rdev_read(pv->dev, pv->offset, sectbuf, SECTOR_SIZE))
		goto out;
	syslinux_make_bootsect(sectbuf, VFAT);
	if (!rdev_write(pv->dev, pv->offset, sectbuf, SECTOR_SIZE)) {
		uprintf("Could not write Syslinux boot record");
		goto out;
	}
	uprintf("Successfully wrote Syslinux boot record");
	ret = true;
out:
	free(sectbuf);
	free(sectors);
	free(ldlinux_file);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* FreeDOS                                                                   */
/* ------------------------------------------------------------------------ */
bool install_freedos(sink_t* sink)
{
	static const char* res_name[] = { "COMMAND.COM", "KERNEL.SYS", "DISPLAY.EXE", "KEYB.EXE",
		"MODE.COM", "KEYBOARD.SYS", "KEYBRD2.SYS", "KEYBRD3.SYS", "KEYBRD4.SYS", "EGA.CPX",
		"EGA2.CPX", "EGA3.CPX", "EGA4.CPX", "EGA5.CPX", "EGA6.CPX",
		"EGA7.CPX", "EGA8.CPX", "EGA9.CPX", "EGA10.CPX", "EGA11.CPX",
		"EGA12.CPX", "EGA13.CPX", "EGA14.CPX", "EGA15.CPX", "EGA16.CPX",
		"EGA17.CPX", "EGA18.CPX" };
	static const char autoexec[] = "@echo off\r\nset PATH=.;\\;\\LOCALE\r\n"
		"echo Using US keyboard with US-English codepage [437]\r\n";
	char src[64], dst[64];
	size_t i, size;
	void* data;

	update_status("Copying DOS files...");
	sink->mkdir(sink, "/LOCALE");
	for (i = 0; i < sizeof(res_name) / sizeof(res_name[0]); i++) {
		snprintf(src, sizeof(src), "freedos/%s", res_name[i]);
		snprintf(dst, sizeof(dst), "%s%s", (i < 2) ? "/" : "/LOCALE/", res_name[i]);
		data = load_resource(src, &size);
		if (data == NULL)
			return false;
		if (!sink_write_buffer(sink, dst, data, size)) {
			free(data);
			return false;
		}
		free(data);
		if (i < 2)
			fat_set_attributes(dst, FAT_AM_HID | FAT_AM_SYS);
		uprintf("Successfully wrote '%s' (%zu bytes)", dst, size);
	}
	if (!sink_write_buffer(sink, "/AUTOEXEC.BAT", autoexec, sizeof(autoexec) - 1))
		return false;
	uprintf("Successfully wrote 'AUTOEXEC.BAT'");
	return true;
}
