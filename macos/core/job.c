/*
 * Rufus for macOS: the format job (port of FormatThread() from src/format.c)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>

#include "rufus_core.h"

extern char** environ;
static sink_t* copy_sink;
static size_t copy_root_len;

static int copy_one(const char* path, const struct stat* st, int type, struct FTW* ftw)
{
	const char* rel = path + copy_root_len;
	(void)ftw;
	if (rel[0] == 0)
		return 0;
	if (type == FTW_D)
		return copy_sink->mkdir(copy_sink, rel) ? 0 : -1;
	if (type == FTW_F) {
		int fd = open(path, O_RDONLY);
		uint8_t* buf = malloc(1 * MB);
		void* f = (fd >= 0 && buf != NULL) ? copy_sink->create(copy_sink, rel, (uint64_t)st->st_size) : NULL;
		ssize_t n;
		bool ok = (f != NULL);
		while (ok && (n = read(fd, buf, 1 * MB)) > 0)
			ok = copy_sink->write(copy_sink, f, buf, (size_t)n);
		if (f != NULL)
			ok = copy_sink->close(copy_sink, f) && ok;
		if (fd >= 0)
			close(fd);
		free(buf);
		uprintf("  %s", rel);
		return ok ? 0 : -1;
	}
	return 0;
}

bool sink_copy_archive(sink_t* s, const char* archive_path)
{
	char tmpl[] = "/tmp/rufus-archive-XXXXXX";
	char* dir = mkdtemp(tmpl);
	char* argv[] = { "ditto", "-x", "-k", (char*)archive_path, dir, NULL };
	pid_t pid;
	int status = -1;
	bool r;

	if (dir == NULL)
		return false;
	update_status("Extracting archive files...");
	uprintf("Extracting archive '%s'...", archive_path);
	if (posix_spawn(&pid, "/usr/bin/ditto", NULL, NULL, argv, environ) != 0 || waitpid(pid, &status, 0) < 0 || status != 0) {
		uprintf("Could not extract archive");
		return false;
	}
	copy_sink = s;
	copy_root_len = strlen(dir);
	r = (nftw(dir, copy_one, 16, FTW_PHYS) == 0);
	{
		char* rm[] = { "rm", "-rf", dir, NULL };
		if (posix_spawn(&pid, "/bin/rm", NULL, NULL, rm, environ) == 0)
			waitpid(pid, &status, 0);
	}
	return r;
}

static const char* bt_str(boot_type_t bt)
{
	switch (bt) {
	case BT_NON_BOOTABLE: return "Non bootable";
	case BT_IMAGE: return "Disk or ISO image";
	case BT_FREEDOS: return "FreeDOS";
	case BT_UEFI_NTFS: return "UEFI:NTFS";
	case BT_SYSLINUX_V6: return "Syslinux 6";
	default: return "?";
	}
}

static bool set_autorun(sink_t* sink, const char* label)
{
	char buf[512];
	size_t icon_len = 0;
	void* icon = load_resource("rufus.ico", &icon_len);
	bool r;
	int len = snprintf(buf, sizeof(buf), "[autorun]\r\nicon  = autorun.ico\r\nlabel = %s\r\n", label);
	r = sink_write_buffer(sink, "/autorun.inf", buf, (size_t)len);
	if (icon != NULL) {
		r = sink_write_buffer(sink, "/autorun.ico", icon, icon_len) && r;
		free(icon);
	}
	if (r)
		uprintf("Created: autorun.inf and autorun.ico");
	return r;
}

int run_job(rdev_t* dev, const job_options_t* o)
{
	const image_report_t* r = o->report;
	image_report_t empty = { 0 };
	layout_t layout;
	part_view_t main_pv;
	sink_t* sink = NULL;
	uint8_t extra_partitions = 0;
	void* uefi_ntfs = NULL;
	size_t uefi_ntfs_size = 0;
	uint32_t cluster_size;
	char label[64];
	time_t start = time(NULL);
	bool ok = false;

	if (r == NULL)
		r = &empty;
	cancel_requested = false;

	uprintf("Format operation started");
	uprintf("  Target: %s (%s)", dev->path, size_to_human(dev->size, false));
	uprintf("  Boot selection: %s%s%s", bt_str(o->boot_type), o->image_path ? " - " : "", o->image_path ? o->image_path : "");
	if (o->boot_type == BT_IMAGE && o->write_as_image) {
		uprintf("  Mode: DD Image");
	} else {
		uprintf("  Partition scheme: %s, target system: %s", o->part_style == PS_GPT ? "GPT" : "MBR",
			o->target == TT_UEFI ? "UEFI (non CSM)" : "BIOS (or UEFI-CSM)");
		uprintf("  File system: %s, cluster size: %s, label: '%s'", fs_name[o->fs],
			o->cluster_size ? size_to_human(o->cluster_size, false) : "default", o->label);
		uprintf("  Quick format: %s, bad blocks passes: %d", o->quick_format ? "Yes" : "No", o->bad_block_passes);
	}

	/* Bad blocks check (destructive, done before anything else) */
	if (o->bad_block_passes > 0) {
		uint64_t bad = 0;
		if (!check_bad_blocks(dev, o->bad_block_passes, &bad)) {
			uprintf("Bad blocks: Check failed.");
			goto out;
		}
		uprintf("Bad Blocks: Check completed, %llu bad block%s found.", (unsigned long long)bad, bad == 1 ? "" : "s");
		if (bad > 0) {
			uprintf("The device has bad blocks - aborting");
			goto out;
		}
	}

	/* Alt-Z / Ctrl-Alt-Z: zero the drive */
	if (o->zero_drive) {
		ok = zero_drive(dev, o->zero_drive == 2);
		goto out;
	}

	/* DD mode */
	if (o->boot_type == BT_IMAGE && o->write_as_image) {
		ok = write_dd_image(dev, o->image_path, r);
		goto out;
	}

	update_status("Clearing partitions...");
	if (!clear_mbr_gpt(dev)) {
		uprintf("Could not reset partitions");
		goto out;
	}
	CHECK_CANCEL(-1);

	/* Extra partitions (same rules as FormatThread) */
	if (o->boot_type == BT_IMAGE && HAS_PERSISTENCE(r) && o->persistence_size != 0)
		extra_partitions |= XP_PERSISTENCE;
	if ((o->boot_type == BT_IMAGE && IS_EFI_BOOTABLE(r) && (o->fs == FS_NTFS || o->fs == FS_EXFAT)) ||
		o->boot_type == BT_UEFI_NTFS) {
		extra_partitions |= XP_UEFI_NTFS;
		if (o->boot_type == BT_IMAGE && HAS_BOOTMGR_BIOS(r) && o->target == TT_BIOS && !rflags.allow_dual_uefi_bios)
			extra_partitions &= ~XP_UEFI_NTFS;
	}
	if (o->old_bios_fixes)
		extra_partitions |= XP_COMPAT;
	if (extra_partitions & XP_UEFI_NTFS) {
		uefi_ntfs = load_resource("uefi/uefi-ntfs.img", &uefi_ntfs_size);
		if (uefi_ntfs == NULL) {
			uprintf("Could not access embedded 'uefi-ntfs.img'");
			goto out;
		}
	}

	cluster_size = o->cluster_size;
	update_status("Creating partitions...");
	if (!compute_layout(dev, &layout, (o->part_style == PS_SFD) ? PS_MBR : o->part_style, o->fs, o->boot_type, cluster_size,
		(o->part_style == PS_SFD) ? 0 : extra_partitions,
		o->old_bios_fixes, uefi_ntfs_size, o->part_style == PS_MBR && o->target == TT_UEFI, o->persistence_size))
		goto out;
	if (o->part_style == PS_SFD) {
		/* Super Floppy Disk: a single file system spanning the whole drive, no partition table */
		uprintf("Using Super Floppy Disk layout (no partition table)");
		layout.count = 1;
		layout.main_index = 0;
		layout.uefi_ntfs_index = layout.persistence_index = -1;
		layout.part[0].offset = 0;
		layout.part[0].size = FLOOR_ALIGN(dev->size, (cluster_size >= dev->sector_size) ? cluster_size : dev->sector_size);
		if (!rdev_zero(dev, 0, 128 * (uint64_t)dev->sector_size))
			goto out;
	} else if (!write_partition_table(dev, &layout))
		goto out;
	if (layout.uefi_ntfs_index >= 0) {
		uprintf("Writing UEFI:NTFS data...");
		if (!rdev_write(dev, layout.part[layout.uefi_ntfs_index].offset, uefi_ntfs, uefi_ntfs_size))
			goto out;
	}
	CHECK_CANCEL(-1);

	/* Persistence partition (ext3, as Rufus does) */
	if (layout.persistence_index >= 0) {
		part_view_t ppv = { dev, layout.part[layout.persistence_index].offset, layout.part[layout.persistence_index].size };
		uprintf("Using %s-like method to enable persistence", r->uses_casper ? "Ubuntu" : "Debian");
		if (!format_ext_ex(&ppv, FS_EXT3, 0, r->uses_casper ? "casper-rw" : "persistence", o->quick_format, !r->uses_casper))
			goto out;
	}

	/* Format the main partition */
	main_pv.dev = dev;
	main_pv.offset = layout.part[layout.main_index].offset;
	main_pv.size = layout.part[layout.main_index].size;
	snprintf(label, sizeof(label), "%s", o->label);
	if (!IS_EXT(o->fs))
		to_valid_label(label, IS_FAT(o->fs) || o->fs == FS_EXFAT);
	if (cluster_size == 0)
		cluster_size = default_cluster_size(o->fs, main_pv.size);

	switch (o->fs) {
	case FS_FAT16:
	case FS_FAT32:
	case FS_EXFAT:
		if (!format_fat(&main_pv, o->fs, cluster_size, label, o->quick_format))
			goto out;
		break;
	case FS_NTFS:
		if (!format_ntfs(&main_pv, cluster_size, label, o->quick_format))
			goto out;
		break;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		if (!format_ext(&main_pv, o->fs, cluster_size, label))
			goto out;
		break;
	default:
		uprintf("Unsupported file system: %s", fs_name[o->fs]);
		goto out;
	}
	CHECK_CANCEL(-1);

	/* MBR/SBR */
	if (o->part_style != PS_SFD && (layout.style == PS_MBR || (o->boot_type != BT_NON_BOOTABLE && layout.style == PS_GPT))) {
		update_status("Writing master boot record...");
		if (!write_mbr(dev, &layout, o->boot_type, r, o->fs, o->target, o->use_rufus_mbr || rflags.use_rufus_mbr))
			goto out;
	}

	/* PBR (only for BIOS targets, as in FormatThread) */
	if (o->boot_type != BT_NON_BOOTABLE && o->boot_type != BT_UEFI_NTFS && o->target == TT_BIOS &&
		!(o->boot_type == BT_IMAGE && (HAS_SYSLINUX(r) && !HAS_WINDOWS(r)))) {
		update_status("Writing partition boot record...");
		if (!write_pbr(&main_pv, o->fs, o->boot_type, r))
			goto out;
	}

	/* Open the file system and copy the files */
	if (IS_EXT(o->fs)) {
		if (o->boot_type != BT_NON_BOOTABLE) {
			uprintf("Bootable ext file systems are not supported");
			goto out;
		}
		ok = true;
		goto out;
	}
	sink = (o->fs == FS_NTFS) ? ntfs_sink_open(&main_pv) : fat_sink_open(&main_pv);
	if (sink == NULL)
		goto out;

	if (o->boot_type == BT_IMAGE && r->is_iso) {
		if (!extract_iso(o->image_path, r, sink, o->fs, layout.persistence_index >= 0))
			goto out;
		if (o->wue != NULL && HAS_WINDOWS(r) && !apply_windows_customization(sink, o->image_path, r, o->wue))
			uprintf("WARNING: Could not apply the Windows customization");
	} else if (o->boot_type == BT_FREEDOS) {
		if (!install_freedos(sink))
			goto out;
	}
	CHECK_CANCEL(-1);

	/* Syslinux (BIOS boot of Linux ISOs) */
	if (o->boot_type == BT_IMAGE && o->target == TT_BIOS && HAS_SYSLINUX(r) && !HAS_WINDOWS(r)) {
		if (!IS_FAT(o->fs)) {
			uprintf("Syslinux can only be installed on FAT in the macOS version");
			goto out;
		}
		if (!install_syslinux(&main_pv, sink, r))
			goto out;
	}

	/* Like Rufus, never replace an autorun.inf that comes with the image */
	if (o->extended_label && o->fs != FS_EXFAT && !(o->boot_type == BT_IMAGE && r->has_autorun))
		set_autorun(sink, label);

	if (o->archive_path != NULL && !sink_copy_archive(sink, o->archive_path))
		uprintf("WARNING: Could not copy additional files");

	update_status("Finalizing, please wait...");
	update_progress(-1);
	if (!sink->unmount(sink)) {
		sink = NULL;
		goto out;
	}
	sink = NULL;
	ok = true;

out:
	if (sink != NULL)
		sink->unmount(sink);
	free(uefi_ntfs);
	rdev_sync(dev);
	if (ok) {
		long secs = (long)(time(NULL) - start);
		uprintf("Operation completed in %02ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60, secs % 60);
		update_status("READY");
		update_progress(100.0);
	} else {
		uprintf(cancel_requested ? "Operation cancelled" : "Operation failed");
		update_status(cancel_requested ? "Cancelled" : "Error");
	}
	return ok ? 0 : -1;
}
