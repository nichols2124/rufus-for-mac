/*
 * Rufus for macOS: partition table creation (MBR and GPT)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * On Windows, Rufus lets the OS build the partition tables through
 * IOCTL_DISK_CREATE_DISK / IOCTL_DISK_SET_DRIVE_LAYOUT_EX. macOS has no such
 * facility for raw devices, so the tables are written out directly. The
 * layout logic itself follows CreatePartition() from src/drive.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "rufus_core.h"

#define MAX_SECTORS_TO_CLEAR    128
#define MBR_UEFI_MARKER         0x49464555  /* 'U', 'E', 'F', 'I' */
#define GPT_MAX_ENTRIES         128
#define GPT_ENTRY_SIZE          128
#define RUFUS_EXTRA_PARTITION_TYPE 0xea
#define GPT_NO_DRIVE_LETTER     0x8000000000000000ULL
#define SECTORS_PER_TRACK       63
#define HEADS                   255

/* GPT partition type GUIDs, in their on-disk (mixed endian) byte order */
static const uint8_t guid_ms_basic_data[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
static const uint8_t guid_esp[16]           = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
static const uint8_t guid_ms_reserved[16]   = { 0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d, 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae };
static const uint8_t guid_linux_data[16]    = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };

#pragma pack(push, 1)
typedef struct {
	uint8_t  status;
	uint8_t  chs_first[3];
	uint8_t  type;
	uint8_t  chs_last[3];
	uint32_t lba_first;
	uint32_t sectors;
} mbr_entry_t;

typedef struct {
	char     signature[8];
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc;
	uint32_t reserved;
	uint64_t current_lba;
	uint64_t backup_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint8_t  disk_guid[16];
	uint64_t entries_lba;
	uint32_t num_entries;
	uint32_t entry_size;
	uint32_t entries_crc;
} gpt_header_t;

typedef struct {
	uint8_t  type_guid[16];
	uint8_t  unique_guid[16];
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t attributes;
	uint16_t name[36];
} gpt_entry_t;
#pragma pack(pop)

_Static_assert(sizeof(mbr_entry_t) == 16, "bad MBR entry size");
_Static_assert(sizeof(gpt_header_t) == 92, "bad GPT header size");
_Static_assert(sizeof(gpt_entry_t) == 128, "bad GPT entry size");

static void random_guid(uint8_t guid[16])
{
	arc4random_buf(guid, 16);
	guid[7] = (guid[7] & 0x0f) | 0x40;	/* version 4 (on-disk byte 7 = high byte of data3) */
	guid[8] = (guid[8] & 0x3f) | 0x80;	/* RFC 4122 variant */
}

static void lba_to_chs(uint64_t lba, uint8_t chs[3])
{
	uint32_t c, h, s;
	if (lba >= 1024ULL * HEADS * SECTORS_PER_TRACK) {
		chs[0] = 0xfe; chs[1] = 0xff; chs[2] = 0xff;
		return;
	}
	c = (uint32_t)(lba / (HEADS * SECTORS_PER_TRACK));
	h = (uint32_t)((lba / SECTORS_PER_TRACK) % HEADS);
	s = (uint32_t)(lba % SECTORS_PER_TRACK) + 1;
	chs[0] = (uint8_t)h;
	chs[1] = (uint8_t)(s | ((c >> 2) & 0xc0));
	chs[2] = (uint8_t)(c & 0xff);
}

bool clear_mbr_gpt(rdev_t* dev)
{
	uint64_t len = (uint64_t)MAX_SECTORS_TO_CLEAR * dev->sector_size;
	if (len > dev->size / 2)
		len = FLOOR_ALIGN(dev->size / 2, dev->sector_size);
	uprintf("Erasing %llu sectors", (unsigned long long)(2 * len / dev->sector_size));
	/* Clear the MBR/primary GPT at the start and the backup GPT at the end */
	return rdev_zero(dev, 0, len) && rdev_zero(dev, dev->size - len, len);
}

bool compute_layout(rdev_t* dev, layout_t* layout, part_style_t style, fs_type_t fs,
	boot_type_t bt, uint32_t cluster_size, uint8_t extra_partitions, bool old_bios_fixes,
	size_t uefi_ntfs_size, bool mbr_uefi_marker, uint64_t persistence_size)
{
	const uint64_t bytes_per_track = (uint64_t)SECTORS_PER_TRACK * dev->sector_size;
	const uint64_t esp_size = 260 * MB;
	uint64_t last_offset, main_size;
	int pi = 0, mi, i;

	memset(layout, 0, sizeof(*layout));
	layout->style = style;
	layout->uefi_ntfs_index = -1;
	layout->esp_index = -1;
	layout->persistence_index = -1;
	layout->disk_signature = mbr_uefi_marker ? MBR_UEFI_MARKER : arc4random();
	if (cluster_size == 0)
		cluster_size = 0x200;

	/* Start of the first partition */
	if (style == PS_GPT || !old_bios_fixes) {
		layout->part[0].offset = 1 * MB;
	} else {
		layout->part[0].offset = CEILING_ALIGN(bytes_per_track, cluster_size) * 2;
	}

	/* ESP up front (Windows To Go) */
	if (extra_partitions & XP_ESP) {
		partition_t* p = &layout->part[pi];
		snprintf(p->name, sizeof(p->name), "EFI System Partition");
		p->size = esp_size;
		p->mbr_type = 0xef;
		p->gpt_type = guid_esp;
		layout->esp_index = pi;
		layout->part[pi + 1].offset = CEILING_ALIGN(p->offset + p->size, bytes_per_track);
		if (cluster_size % dev->sector_size == 0)
			layout->part[pi + 1].offset = FLOOR_ALIGN(layout->part[pi + 1].offset, cluster_size);
		pi++;
		extra_partitions &= ~XP_ESP;
	}

	/* MSR (GPT only, must come before the data partition) */
	if (extra_partitions & XP_MSR) {
		partition_t* p = &layout->part[pi];
		snprintf(p->name, sizeof(p->name), "Microsoft Reserved Partition");
		p->size = 128 * MB;
		p->gpt_type = guid_ms_reserved;
		layout->part[pi + 1].offset = CEILING_ALIGN(p->offset + p->size, bytes_per_track);
		if (cluster_size % dev->sector_size == 0)
			layout->part[pi + 1].offset = FLOOR_ALIGN(layout->part[pi + 1].offset, cluster_size);
		pi++;
		extra_partitions &= ~XP_MSR;
	}

	/* Main partition */
	mi = layout->main_index = pi++;
	snprintf(layout->part[mi].name, sizeof(layout->part[mi].name), "Main Data Partition");
	layout->part[mi].active = (bt != BT_NON_BOOTABLE);
	layout->part[mi].gpt_type = guid_ms_basic_data;
	switch (fs) {
	case FS_FAT16:  layout->part[mi].mbr_type = 0x0e; break;
	case FS_FAT32:  layout->part[mi].mbr_type = 0x0c; break;
	case FS_NTFS:
	case FS_EXFAT:
	case FS_UDF:
	case FS_REFS:   layout->part[mi].mbr_type = 0x07; break;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		layout->part[mi].mbr_type = 0x83;
		layout->part[mi].gpt_type = guid_linux_data;
		break;
	default:
		uprintf("Unsupported file system");
		return false;
	}

	/* Trailing extra partitions */
	if (extra_partitions & XP_PERSISTENCE) {
		partition_t* p = &layout->part[pi];
		snprintf(p->name, sizeof(p->name), "Linux Persistence");
		p->size = CEILING_ALIGN(persistence_size, bytes_per_track);
		p->mbr_type = 0x83;
		p->gpt_type = guid_linux_data;
		layout->persistence_index = pi++;
	}
	if (extra_partitions & XP_UEFI_NTFS) {
		partition_t* p = &layout->part[pi];
		snprintf(p->name, sizeof(p->name), "UEFI:NTFS");
		p->size = CEILING_ALIGN(uefi_ntfs_size, bytes_per_track);
		/* An MBR ESP is fine for UEFI:NTFS, but on GPT it must NOT be an ESP, or
		 * the Windows installer chokes on having two of them (see drive.c) */
		p->mbr_type = 0xef;
		p->gpt_type = guid_ms_basic_data;
		p->gpt_attributes = GPT_NO_DRIVE_LETTER;
		layout->uefi_ntfs_index = pi++;
	}
	if (!(extra_partitions & XP_UEFI_NTFS) && (extra_partitions & XP_COMPAT)) {
		partition_t* p = &layout->part[pi++];
		snprintf(p->name, sizeof(p->name), "BIOS Compatibility");
		p->size = bytes_per_track;
		p->mbr_type = RUFUS_EXTRA_PARTITION_TYPE;
		p->gpt_type = guid_ms_basic_data;
	}

	/* Offsets of the trailing partitions, aligned to a track */
	last_offset = dev->size;
	if (style == PS_GPT)
		last_offset -= 33ULL * dev->sector_size;
	for (i = pi - 1; i > mi; i--) {
		layout->part[i].offset = FLOOR_ALIGN(last_offset - layout->part[i].size, bytes_per_track);
		last_offset = layout->part[i].offset;
	}

	main_size = last_offset - layout->part[mi].offset;
	layout->part[mi].size = FLOOR_ALIGN(main_size, bytes_per_track);
	if (cluster_size % dev->sector_size == 0)
		layout->part[mi].size = FLOOR_ALIGN(layout->part[mi].size, cluster_size);
	if ((int64_t)layout->part[mi].size <= 0 || layout->part[mi].offset >= dev->size) {
		uprintf("Error: The drive is too small for this partition layout");
		return false;
	}
	layout->count = pi;

	for (i = 0; i < pi; i++)
		uprintf("● Creating %s%s (offset: %llu, size: %s)", layout->part[i].name,
			strstr(layout->part[i].name, "Partition") == NULL ? " Partition" : "",
			(unsigned long long)layout->part[i].offset, size_to_human(layout->part[i].size, false));
	return true;
}

static bool write_mbr_table(rdev_t* dev, const layout_t* layout)
{
	uint8_t mbr[512];
	mbr_entry_t* e = (mbr_entry_t*)&mbr[0x1be];
	uint32_t ss = dev->sector_size;
	int i;

	/* Keep any boot code already present (written later by write_mbr()) */
	if (!rdev_read(dev, 0, mbr, sizeof(mbr)))
		return false;
	memset(&mbr[0x1b8], 0, 512 - 0x1b8);
	memcpy(&mbr[0x1b8], &layout->disk_signature, 4);

	if (layout->count > 4) {
		uprintf("Too many partitions for MBR");
		return false;
	}
	for (i = 0; i < layout->count; i++) {
		const partition_t* p = &layout->part[i];
		uint64_t first = p->offset / ss, count = p->size / ss;
		if (first + count > 0xffffffffULL) {
			uprintf("Partition '%s' exceeds the 2 TB MBR limit - use GPT instead", p->name);
			return false;
		}
		e[i].status = p->active ? 0x80 : 0x00;
		e[i].type = p->mbr_type;
		e[i].lba_first = (uint32_t)first;
		e[i].sectors = (uint32_t)count;
		lba_to_chs(first, e[i].chs_first);
		lba_to_chs(first + count - 1, e[i].chs_last);
	}
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	return rdev_write(dev, 0, mbr, sizeof(mbr));
}

static bool write_gpt_table(rdev_t* dev, const layout_t* layout)
{
	const uint32_t ss = dev->sector_size;
	const uint64_t last_lba = dev->size / ss - 1;
	const uint32_t entries_bytes = GPT_MAX_ENTRIES * GPT_ENTRY_SIZE;
	const uint64_t entries_sectors = CEILING_ALIGN(entries_bytes, ss) / ss;
	uint8_t* entries = calloc(1, (size_t)(entries_sectors * ss));
	uint8_t* hdr_sector = calloc(1, ss);
	uint8_t mbr[512];
	mbr_entry_t* pmbr = (mbr_entry_t*)&mbr[0x1be];
	gpt_header_t hdr = { 0 };
	bool r = false;
	int i, j;

	if (entries == NULL || hdr_sector == NULL)
		goto out;

	/* Protective MBR (boot code, if any, gets written later) */
	if (!rdev_read(dev, 0, mbr, sizeof(mbr)))
		goto out;
	memset(&mbr[0x1b8], 0, 512 - 0x1b8);
	pmbr->type = 0xee;
	pmbr->lba_first = 1;
	pmbr->sectors = (last_lba > 0xffffffffULL) ? 0xffffffff : (uint32_t)last_lba;
	lba_to_chs(1, pmbr->chs_first);
	lba_to_chs(last_lba, pmbr->chs_last);
	mbr[510] = 0x55;
	mbr[511] = 0xaa;

	for (i = 0; i < layout->count; i++) {
		const partition_t* p = &layout->part[i];
		gpt_entry_t* e = (gpt_entry_t*)&entries[i * GPT_ENTRY_SIZE];
		memcpy(e->type_guid, p->gpt_type, 16);
		random_guid(e->unique_guid);
		e->first_lba = p->offset / ss;
		e->last_lba = (p->offset + p->size) / ss - 1;
		e->attributes = p->gpt_attributes;
		for (j = 0; j < 36 && p->name[j] != 0; j++)
			e->name[j] = (uint16_t)(uint8_t)p->name[j];
	}

	memcpy(hdr.signature, "EFI PART", 8);
	hdr.revision = 0x00010000;
	hdr.header_size = sizeof(gpt_header_t);
	hdr.first_usable_lba = 2 + entries_sectors;
	hdr.last_usable_lba = last_lba - entries_sectors - 1;
	random_guid(hdr.disk_guid);
	hdr.num_entries = GPT_MAX_ENTRIES;
	hdr.entry_size = GPT_ENTRY_SIZE;
	hdr.entries_crc = (uint32_t)crc32(0L, entries, entries_bytes);

	/* Primary header */
	hdr.current_lba = 1;
	hdr.backup_lba = last_lba;
	hdr.entries_lba = 2;
	hdr.header_crc = 0;
	hdr.header_crc = (uint32_t)crc32(0L, (const Bytef*)&hdr, sizeof(hdr));
	memcpy(hdr_sector, &hdr, sizeof(hdr));
	if (!rdev_write(dev, 0, mbr, sizeof(mbr)) ||
		!rdev_write(dev, 1ULL * ss, hdr_sector, ss) ||
		!rdev_write(dev, 2ULL * ss, entries, (size_t)(entries_sectors * ss)))
		goto out;

	/* Backup header & entries */
	hdr.current_lba = last_lba;
	hdr.backup_lba = 1;
	hdr.entries_lba = last_lba - entries_sectors;
	hdr.header_crc = 0;
	hdr.header_crc = (uint32_t)crc32(0L, (const Bytef*)&hdr, sizeof(hdr));
	memset(hdr_sector, 0, ss);
	memcpy(hdr_sector, &hdr, sizeof(hdr));
	if (!rdev_write(dev, hdr.entries_lba * ss, entries, (size_t)(entries_sectors * ss)) ||
		!rdev_write(dev, last_lba * ss, hdr_sector, ss))
		goto out;
	r = true;

out:
	free(entries);
	free(hdr_sector);
	return r;
}

bool write_partition_table(rdev_t* dev, const layout_t* layout)
{
	int i;
	uprintf("Partitioning (%s)...", layout->style == PS_GPT ? "GPT" : "MBR");

	/* Zero the start of each partition, to avoid stale file system detection */
	for (i = 0; i < layout->count; i++) {
		uint64_t len = (uint64_t)MAX_SECTORS_TO_CLEAR * dev->sector_size;
		if (len > layout->part[i].size)
			len = layout->part[i].size;
		if (!rdev_zero(dev, layout->part[i].offset, len))
			uprintf("Could not zero %s", layout->part[i].name);
	}
	return (layout->style == PS_GPT) ? write_gpt_table(dev, layout) : write_mbr_table(dev, layout);
}

/* ------------------------------------------------------------------------ */
/* Alt-P: toggle the first ESP to Basic Data (and back), for both GPT copies */
/* ------------------------------------------------------------------------ */
static bool gpt_rewrite(rdev_t* dev, uint64_t hdr_lba, int* toggled_index, bool* to_esp)
{
	const uint32_t ss = dev->sector_size;
	uint8_t* sector = malloc(ss);
	uint8_t* entries = NULL;
	gpt_header_t hdr;
	uint32_t crc, entries_bytes;
	bool r = false;
	int i;

	if (sector == NULL || !rdev_read(dev, hdr_lba * ss, sector, ss))
		goto out;
	memcpy(&hdr, sector, sizeof(hdr));
	if (memcmp(hdr.signature, "EFI PART", 8) != 0) {
		uprintf("No GPT found at LBA %llu", (unsigned long long)hdr_lba);
		goto out;
	}
	entries_bytes = hdr.num_entries * hdr.entry_size;
	if (hdr.entry_size != GPT_ENTRY_SIZE || entries_bytes > 64 * KB)
		goto out;
	entries = malloc(CEILING_ALIGN(entries_bytes, ss));
	if (entries == NULL || !rdev_read(dev, hdr.entries_lba * ss, entries, CEILING_ALIGN(entries_bytes, ss)))
		goto out;
	if (*toggled_index < 0) {
		/* First pass: find an ESP, or failing that, a Basic Data partition that was an ESP */
		for (i = 0; i < (int)hdr.num_entries && *toggled_index < 0; i++)
			if (memcmp(&entries[i * GPT_ENTRY_SIZE], guid_esp, 16) == 0) {
				*toggled_index = i;
				*to_esp = false;
			}
		for (i = 0; i < (int)hdr.num_entries && *toggled_index < 0; i++)
			if (memcmp(&entries[i * GPT_ENTRY_SIZE], guid_ms_basic_data, 16) == 0) {
				*toggled_index = i;
				*to_esp = true;
			}
		if (*toggled_index < 0) {
			uprintf("No ESP or Basic Data partition to toggle");
			goto out;
		}
	}
	memcpy(&entries[*toggled_index * GPT_ENTRY_SIZE], *to_esp ? guid_esp : guid_ms_basic_data, 16);
	hdr.entries_crc = (uint32_t)crc32(0L, entries, entries_bytes);
	hdr.header_crc = 0;
	crc = (uint32_t)crc32(0L, (const Bytef*)&hdr, sizeof(hdr));
	hdr.header_crc = crc;
	memcpy(sector, &hdr, sizeof(hdr));
	r = rdev_write(dev, hdr.entries_lba * ss, entries, CEILING_ALIGN(entries_bytes, ss)) &&
		rdev_write(dev, hdr_lba * ss, sector, ss);
out:
	free(sector);
	free(entries);
	return r;
}

bool gpt_toggle_esp(rdev_t* dev)
{
	int index = -1;
	bool to_esp = false;
	if (!gpt_rewrite(dev, 1, &index, &to_esp))
		return false;
	if (!gpt_rewrite(dev, dev->size / dev->sector_size - 1, &index, &to_esp))
		uprintf("WARNING: Could not update the backup GPT");
	uprintf("Partition %d changed to %s", index + 1, to_esp ? "EFI System Partition" : "Basic Data");
	return true;
}
