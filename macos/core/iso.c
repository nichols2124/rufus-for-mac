/*
 * Rufus for macOS: ISO9660/UDF image analysis and extraction
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Port of the scanning (check_iso_props) and extraction logic of src/iso.c.
 * Instead of writing to a mounted drive letter, files go to a sink_t, which
 * writes directly into the FAT/exFAT/NTFS volume on the raw device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

#include <cdio/cdio.h>
#include <cdio/logging.h>
#include <cdio/iso9660.h>
#include <cdio/udf.h>

#include "rufus_core.h"
#include "ntfs_io.h"

#define ISO_BUFFER_SIZE     (64 * 1024)
#define MAX_CFG_SIZE        (1 * 1024 * 1024)

static const char* bootmgr_name = "bootmgr";
static const char* bootmgr_efi_name = "bootmgr.efi";
static const char* grldr_name = "grldr";
static const char* ldlinux_name = "ldlinux.sys";
static const char* casper_dirname = "/casper";
static const char* proxmox_dirname = "/proxmox";
static const char* efi_bootname[] = { "boot", "grub", "mm" };
static const char* efi_archname[] = { "", "ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64", "ebc" };
static const char* sources_str = "/sources";
static const char* wininst_name[] = { "install.wim", "install.esd", "install.swm" };
static const char* grub_dirname[] = { "/boot/grub/i386-pc", "/boot/grub2/i386-pc" };
static const char* grub_cfg[] = { "grub.cfg", "loopback.cfg" };
static const char* syslinux_cfg[] = { "isolinux.cfg", "syslinux.cfg", "extlinux.conf", "txt.cfg", "live.cfg" };
static const char* isolinux_bin[] = { "isolinux.bin", "boot.bin" };
static const char* pe_dirname[] = { "/i386", "/amd64", "/minint" };
static const char* pe_file[] = { "ntdetect.com", "setupldr.bin", "txtsetup.sif" };
static const char* kolibri_name = "kolibri.img";
static const char* manjaro_marker = ".miso";
static const char* pop_os_name = "pop-os";

#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

static bool scan_only;
static image_report_t* rep;
static char isolinux_path[256];
static uint64_t total_bytes, done_bytes;

/* Extraction context */
static sink_t* out_sink;
static fs_type_t out_fs;
static const image_report_t* xrep;
static char usb_label[64];
static bool add_persistence;

/* libcdio's UDF code references the CD driver layer, which Rufus never uses (see src/iso.c) */
CdIo_t* cdio_open(const char* psz_source, driver_id_t driver_id) { (void)psz_source; (void)driver_id; return NULL; }
void cdio_destroy(CdIo_t* p_cdio) { (void)p_cdio; }

static void cdio_log_handler(cdio_log_level_t level, const char* message)
{
	if (level >= CDIO_LOG_WARN)
		uprintf("libcdio: %s", message);
}

static bool ends_with_ci(const char* s, const char* suffix)
{
	size_t ls = strlen(s), lx = strlen(suffix);
	return (ls >= lx) && strcasecmp(&s[ls - lx], suffix) == 0;
}

/* ------------------------------------------------------------------------ */
/* Scanning                                                                  */
/* ------------------------------------------------------------------------ */
static void check_iso_props(const char* dirname, int64_t file_length, const char* basename, const char* fullpath)
{
	size_t i, j, k;
	char bootloader_name[32];

	for (i = 0; i < ARRAYSIZE(syslinux_cfg); i++) {
		if (strcasecmp(basename, syslinux_cfg[i]) == 0) {
			if (i < 3 && rep->cfg_path[0] == 0)
				snprintf(rep->cfg_path, sizeof(rep->cfg_path), "%s", fullpath);
			if (i == 1 && strcasecmp(dirname, "/efi/boot") == 0)
				rep->has_efi_syslinux = true;
		}
	}
	for (i = 0; i < ARRAYSIZE(grub_dirname); i++)
		if (strcasecmp(dirname, grub_dirname[i]) == 0)
			rep->has_grub2 = (uint8_t)i + 1;
	if (strncasecmp(dirname, casper_dirname, strlen(casper_dirname)) == 0) {
		rep->uses_casper = true;
		if (strstr(dirname, pop_os_name) != NULL)
			rep->disable_iso = true;
	}
	if (strcasecmp(dirname, proxmox_dirname) == 0)
		rep->disable_iso = true;

	/* Root directory files */
	if (dirname[0] == 0) {
		if (strcasecmp(basename, bootmgr_name) == 0)
			rep->has_bootmgr = true;
		if (strcasecmp(basename, bootmgr_efi_name) == 0) {
			rep->has_efi |= 1;
			rep->has_bootmgr_efi = true;
		}
		if (strcasecmp(basename, grldr_name) == 0)
			rep->has_grub4dos = true;
		if (strcasecmp(basename, kolibri_name) == 0)
			rep->has_kolibrios = true;
		if (strcasecmp(basename, manjaro_marker) == 0)
			rep->disable_iso = true;
		if (strcasecmp(basename, "autorun.inf") == 0)
			rep->has_autorun = true;
	}

	if (rep->efi_img_path[0] == 0 && strlen(basename) >= 7 && strncasecmp(basename, "efi", 3) == 0 &&
		ends_with_ci(basename, ".img"))
		snprintf(rep->efi_img_path, sizeof(rep->efi_img_path), "%s", fullpath);

	if (strncasecmp(dirname, "/efi/", 5) == 0) {
		for (k = 0; k < ARRAYSIZE(efi_bootname); k++) {
			for (i = 0; i < ARRAYSIZE(efi_archname); i++) {
				snprintf(bootloader_name, sizeof(bootloader_name), "%s%s.efi", efi_bootname[k], efi_archname[i]);
				if (strcasecmp(basename, bootloader_name) == 0 && k == 0)
					rep->has_efi |= (2 << i);
			}
		}
		if (strcasecmp(basename, "bootx64.efi") == 0 && file_length < 256)
			rep->has_efi |= 0x4000;	/* broken symlink (Mint) */
	}

	if (ends_with_ci(dirname, sources_str)) {
		for (i = 0; i < ARRAYSIZE(wininst_name); i++) {
			if (strcasecmp(basename, wininst_name[i]) == 0 && rep->wininst_index < MAX_WININST) {
				snprintf(rep->wininst_path[rep->wininst_index++], sizeof(rep->wininst_path[0]), "%s", fullpath);
				if (file_length >= (int64_t)(4 * GB))
					rep->has_4GB_file |= (uint8_t)(0x10 << i);
			}
		}
	}
	if (file_length >= (int64_t)(4 * GB))
		rep->has_4GB_file |= 0x01;

	if (strcasecmp(dirname, "/sources/$OEM$/$$/Panther") == 0 && strcasecmp(basename, "unattend.xml") == 0)
		rep->has_panther_unattend = true;

	for (i = 0; i < ARRAYSIZE(pe_dirname); i++)
		if (strcasecmp(dirname, pe_dirname[i]) == 0)
			for (j = 0; j < ARRAYSIZE(pe_file); j++)
				if (strcasecmp(basename, pe_file[j]) == 0)
					rep->winpe |= (uint16_t)((1 << j) << (ARRAYSIZE(pe_dirname) * i));

	for (i = 0; i < ARRAYSIZE(isolinux_bin); i++)
		if (strcasecmp(basename, isolinux_bin[i]) == 0 && isolinux_path[0] == 0)
			snprintf(isolinux_path, sizeof(isolinux_path), "%s", fullpath);

	if (strlen(basename) > 64)
		rep->has_long_filename = true;
}

/* ------------------------------------------------------------------------ */
/* Config file patching (fix_config in iso.c)                                */
/* ------------------------------------------------------------------------ */

/* Replace 'from' with 'to' on every line whose first token is 'token' */
static char* replace_in_token_lines(char* data, size_t* len, const char* token, const char* from, const char* to, bool* modified)
{
	size_t flen = strlen(from), tlen = strlen(to), cap = *len * 2 + 4096, olen = 0;
	char* out = malloc(cap);
	const char* p = data;
	const char* end = data + *len;

	if (out == NULL || flen == 0)
		return data;
	while (p < end) {
		const char* eol = memchr(p, '\n', (size_t)(end - p));
		const char* q = p;
		size_t line_len = (eol == NULL) ? (size_t)(end - p) : (size_t)(eol - p + 1);
		bool match;
		while (q < p + line_len && (*q == ' ' || *q == '\t'))
			q++;
		match = (strncasecmp(q, token, strlen(token)) == 0) &&
			(q + strlen(token) < p + line_len) && isspace((unsigned char)q[strlen(token)]);
		if (olen + line_len * 2 + tlen + 16 > cap) {
			char* n;
			cap = (olen + line_len * 2 + tlen + 16) * 2;
			n = realloc(out, cap);
			if (n == NULL) {
				free(out);
				return data;
			}
			out = n;
		}
		if (match) {
			const char* s = p;
			while (s < p + line_len) {
				if ((size_t)(p + line_len - s) >= flen && memcmp(s, from, flen) == 0) {
					if (olen + tlen + (size_t)(p + line_len - s) + 16 > cap) {
						char* n;
						cap = (olen + tlen + line_len + 16) * 2;
						n = realloc(out, cap);
						if (n == NULL) {
							free(out);
							return data;
						}
						out = n;
					}
					memcpy(&out[olen], to, tlen);
					olen += tlen;
					s += flen;
					*modified = true;
				} else {
					out[olen++] = *s++;
				}
			}
		} else {
			memcpy(&out[olen], p, line_len);
			olen += line_len;
		}
		p += line_len;
	}
	free(data);
	*len = olen;
	return out;
}

static char* replace_char_str(const char* src, char c, const char* rep_str)
{
	size_t i, n = 0, rl = strlen(rep_str);
	char* out;
	for (i = 0; src[i]; i++)
		if (src[i] == c)
			n++;
	out = malloc(strlen(src) + n * rl + 1);
	if (out == NULL)
		return NULL;
	for (i = 0, n = 0; src[i]; i++) {
		if (src[i] == c) {
			memcpy(&out[n], rep_str, rl);
			n += rl;
		} else {
			out[n++] = src[i];
		}
	}
	out[n] = 0;
	return out;
}

static char* fix_config(char* data, size_t* len, const char* path, const char* basename)
{
	static const char* cfg_token[] = { "options", "append", "linux", "linuxefi", "$linux", "search", "for" };
	bool modified = false, is_grub_cfg = false, is_syslinux_cfg = false;
	bool is_menu_cfg = (strcasecmp(basename, "menu.cfg") == 0);
	char *iso_label, *ulabel;
	size_t i;

	for (i = 0; i < ARRAYSIZE(grub_cfg); i++)
		if (strcasecmp(basename, grub_cfg[i]) == 0)
			is_grub_cfg = true;
	for (i = 0; i < ARRAYSIZE(syslinux_cfg); i++)
		if (strcasecmp(basename, syslinux_cfg[i]) == 0)
			is_syslinux_cfg = true;

	/* Add persistence to the kernel options (same order of attempts as Rufus) */
	if (add_persistence && (is_grub_cfg || is_menu_cfg || is_syslinux_cfg)) {
		const char* tok = is_grub_cfg ? "linux" : "append";
		bool p = false;
		data = replace_in_token_lines(data, len, tok, "file=/cdrom/preseed", "persistent file=/cdrom/preseed", &p);
		if (p) {
			uprintf("  Added 'persistent' kernel option");
			if (is_grub_cfg) {
				bool m = false;
				data = replace_in_token_lines(data, len, "linux", "maybe-ubiquity", "", &m);
				if (m)
					uprintf("  Removed 'maybe-ubiquity' kernel option");
			}
		} else {
			data = replace_in_token_lines(data, len, tok, "boot=casper", "boot=casper persistent", &p);
			if (!p)
				data = replace_in_token_lines(data, len, "linux", "/casper/vmlinuz", "/casper/vmlinuz persistent", &p);
			if (!p)
				data = replace_in_token_lines(data, len, "kernel", "/casper/vmlinuz", "/casper/vmlinuz persistent", &p);
			if (p) {
				uprintf("  Added 'persistent' kernel option");
			} else {
				data = replace_in_token_lines(data, len, tok, "boot=live", "boot=live persistence", &p);
				if (p)
					uprintf("  Added 'persistence' kernel option");
			}
		}
		modified |= p;
	}

	if (xrep->label[0] != 0 && strcmp(xrep->label, usb_label) != 0) {
		iso_label = replace_char_str(xrep->label, ' ', "\\x20");
		ulabel = replace_char_str(usb_label, ' ', "\\x20");
		if (iso_label != NULL && ulabel != NULL) {
			bool patched = false;
			for (i = 0; i < ARRAYSIZE(cfg_token); i++)
				data = replace_in_token_lines(data, len, cfg_token[i], iso_label, ulabel, &patched);
			if (patched)
				uprintf("  Patched %s: '%s' ➔ '%s'", path, iso_label, ulabel);
			modified |= patched;
		}
		free(iso_label);
		free(ulabel);
	}
	if (is_grub_cfg) {
		char from[256], to[256];
		bool patched = false;
		snprintf(from, sizeof(from), "cd9660:/dev/iso9660/%s", xrep->label);
		snprintf(to, sizeof(to), "msdosfs:/dev/msdosfs/%s", usb_label);
		data = replace_in_token_lines(data, len, "set", from, to, &patched);
		if (patched)
			uprintf("  Patched %s: '%s' ➔ '%s'", path, from, to);
	}
	(void)modified;
	return data;
}

static bool is_config_file(const char* dirname, const char* basename)
{
	size_t len = strlen(basename);
	if (len >= 4 && strcasecmp(&basename[len - 4], ".cfg") == 0)
		return true;
	if (strcasecmp(basename, "extlinux.conf") == 0)
		return true;
	if (strcasecmp(dirname, "/loader/entries") == 0 && len > 5 && strcasecmp(&basename[len - 5], ".conf") == 0)
		return true;
	return false;
}

/* ------------------------------------------------------------------------ */
/* Writing                                                                   */
/* ------------------------------------------------------------------------ */
typedef int64_t (*block_reader_t)(void* ctx, uint8_t* buf, uint64_t offset, size_t blocks);

/*
 * On NTFS, files go through a large write cache (see ntfs_io.c), which takes in
 * hundreds of MB instantly and then writes them out in one go. Counting progress
 * as files are handed over made the bar jump and then stall, so for NTFS the
 * progress counts the bytes that actually reach the drive instead. That also
 * covers the final flush, so 100% means the data is on the drive.
 */
static bool device_progress;
static uint64_t device_done;

static void device_write_hook(uint64_t n)
{
	device_done += n;
	update_progress_bytes(NULL, (device_done < total_bytes) ? device_done : total_bytes, total_bytes);
}

static void report_progress(uint64_t n)
{
	done_bytes += n;
	if (!device_progress)
		update_progress_bytes(NULL, done_bytes, total_bytes);
}

bool extract_iso_uses_device_progress(void)
{
	return device_progress;
}

void extract_iso_progress_done(void)
{
	ntfs_io_write_hook = NULL;
	device_progress = false;
}

/* Write a file from a reader callback into the sink, with config patching */
static bool write_file(const char* fullpath, const char* dirname, const char* basename, int64_t length,
	int64_t (*read_fn)(void* ctx, uint8_t* buf, size_t max), void* ctx)
{
	uint8_t* buf;
	void* f;
	bool r = false;

	/* Don't let an ISO's own ldlinux.sys overwrite ours */
	if (dirname[0] == 0 && strcasecmp(basename, ldlinux_name) == 0) {
		uprintf("Skipping '%s' file from ISO image", basename);
		return true;
	}
	if ((uint64_t)length > out_sink->max_file_size) {
		uprintf("  '%s' is larger than 4 GB and cannot be copied to %s", fullpath, fs_name[out_fs]);
		return false;
	}
	uprintf("Extracting: %s (%s)", fullpath, size_to_human((uint64_t)length, false));

	if (is_config_file(dirname, basename) && length < MAX_CFG_SIZE) {
		size_t len = 0;
		buf = malloc((size_t)length + ISO_BUFFER_SIZE);
		if (buf == NULL)
			return false;
		while ((int64_t)len < length) {
			int64_t n = read_fn(ctx, buf + len, ISO_BUFFER_SIZE);
			if (n <= 0)
				break;
			len += (size_t)n;
		}
		if ((int64_t)len < length) {
			uprintf("  Error reading '%s'", fullpath);
			free(buf);
			return false;
		}
		len = (size_t)length;
		buf = (uint8_t*)fix_config((char*)buf, &len, fullpath, basename);
		r = sink_write_buffer(out_sink, fullpath, buf, len);
		free(buf);
		report_progress((uint64_t)length);
		return r;
	}

	f = out_sink->create(out_sink, fullpath, (uint64_t)length);
	if (f == NULL)
		return false;
	buf = malloc(ISO_BUFFER_SIZE * 16);
	if (buf == NULL)
		goto out;
	while (length > 0) {
		int64_t n;
		if (cancel_requested)
			goto out;
		n = read_fn(ctx, buf, ISO_BUFFER_SIZE * 16);
		if (n <= 0) {
			uprintf("  Error reading '%s'", fullpath);
			goto out;
		}
		if (n > length)
			n = length;
		if (!out_sink->write(out_sink, f, buf, (size_t)n))
			goto out;
		length -= n;
		report_progress((uint64_t)n);
	}
	r = true;
out:
	free(buf);
	if (!out_sink->close(out_sink, f))
		r = false;
	return r;
}

/* ISO9660 reader */
typedef struct {
	iso9660_t* iso;
	lsn_t lsn;
	uint64_t remaining;
} iso_rd_t;

static int64_t iso_read_fn(void* ctx, uint8_t* buf, size_t max)
{
	iso_rd_t* rd = ctx;
	size_t nb = max / ISO_BLOCKSIZE;
	uint64_t need = (rd->remaining + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;
	if (need < nb)
		nb = (size_t)need;
	if (nb == 0)
		return 0;
	if (iso9660_iso_seek_read(rd->iso, buf, rd->lsn, (long)nb) != (long)(nb * ISO_BLOCKSIZE))
		return -1;
	rd->lsn += (lsn_t)nb;
	if ((uint64_t)nb * ISO_BLOCKSIZE >= rd->remaining) {
		int64_t r = (int64_t)rd->remaining;
		rd->remaining = 0;
		return r;
	}
	rd->remaining -= (uint64_t)nb * ISO_BLOCKSIZE;
	return (int64_t)nb * ISO_BLOCKSIZE;
}

static bool iso_walk(iso9660_t* iso, const char* path, bool use_rr)
{
	CdioISO9660FileList_t* list;
	CdioListNode_t* node;
	char fullpath[1024], basename[512];
	int joliet = iso9660_ifs_get_joliet_level(iso);
	bool r = true;

	list = iso9660_ifs_readdir(iso, path[0] ? path : "/");
	if (list == NULL) {
		uprintf("Could not read ISO directory '%s'", path);
		return false;
	}
	_CDIO_LIST_FOREACH(node, list) {
		iso9660_stat_t* st = (iso9660_stat_t*)_cdio_list_node_data(node);
		if (cancel_requested) {
			r = false;
			break;
		}
		if (strcmp(st->filename, ".") == 0 || strcmp(st->filename, "..") == 0)
			continue;
		if (use_rr && st->rr.b3_rock == yep)
			snprintf(basename, sizeof(basename), "%s", st->filename);
		else
			iso9660_name_translate_ext(st->filename, basename, joliet);
		snprintf(fullpath, sizeof(fullpath), "%s/%s", path, basename);

		if (st->type == _STAT_DIR) {
			if (!scan_only && !out_sink->mkdir(out_sink, fullpath)) {
				r = false;
				break;
			}
			if (!iso_walk(iso, fullpath, use_rr)) {
				r = false;
				break;
			}
		} else {
			int64_t length = (int64_t)st->total_size;
			bool is_symlink = use_rr && st->rr.b3_rock == yep && st->rr.psz_symlink != NULL;
			if (scan_only) {
				if (is_symlink)
					rep->has_symlinks = true;
				check_iso_props(path, length, basename, fullpath);
				rep->projected_size += (uint64_t)length;
				continue;
			}
			if (is_symlink) {
				/* FAT/NTFS can't hold POSIX symlinks. Like Rufus, follow the link if it
				 * points to a file, otherwise skip it. */
				char target[1024];
				iso9660_stat_t* st2;
				snprintf(target, sizeof(target), "%s/%s", path, st->rr.psz_symlink);
				st2 = iso9660_ifs_stat_translate(iso, target);
				if (st2 == NULL || st2->type == _STAT_DIR) {
					uprintf("  Ignoring Rock Ridge symbolic link '%s' ➔ '%s'", fullpath, st->rr.psz_symlink);
					if (st2 != NULL)
						iso9660_stat_free(st2);
					continue;
				}
				{
					iso_rd_t rd = { iso, st2->lsn, st2->total_size };
					r = write_file(fullpath, path, basename, (int64_t)st2->total_size, iso_read_fn, &rd);
				}
				iso9660_stat_free(st2);
			} else {
				iso_rd_t rd = { iso, st->lsn, (uint64_t)length };
				r = write_file(fullpath, path, basename, length, iso_read_fn, &rd);
			}
			if (!r)
				break;
		}
	}
	iso9660_filelist_free(list);
	return r;
}

/* UDF reader */
typedef struct {
	udf_dirent_t* d;
	uint64_t remaining;
} udf_rd_t;

static int64_t udf_read_fn(void* ctx, uint8_t* buf, size_t max)
{
	udf_rd_t* rd = ctx;
	size_t nb = max / UDF_BLOCKSIZE;
	uint64_t need = (rd->remaining + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE;
	ssize_t r;
	/* Never ask libcdio for more blocks than the file has left */
	if (need < nb)
		nb = (size_t)need;
	if (nb == 0)
		return 0;
	r = udf_read_block(rd->d, buf, nb);
	if (r > 0)
		rd->remaining = ((uint64_t)r >= rd->remaining) ? 0 : rd->remaining - (uint64_t)r;
	return (int64_t)r;
}

static bool udf_walk(udf_t* udf, udf_dirent_t* dir, const char* path)
{
	char fullpath[1024];
	bool r = true;

	while ((dir = udf_readdir(dir)) != NULL) {
		const char* basename = udf_get_filename(dir);
		if (cancel_requested) {
			udf_dirent_free(dir);
			return false;
		}
		if (basename == NULL || basename[0] == 0)
			continue;
		snprintf(fullpath, sizeof(fullpath), "%s/%s", path, basename);
		if (udf_is_dir(dir)) {
			udf_dirent_t* sub;
			if (!scan_only && !out_sink->mkdir(out_sink, fullpath)) {
				udf_dirent_free(dir);
				return false;
			}
			sub = udf_opendir(dir);
			if (sub != NULL && !udf_walk(udf, sub, fullpath)) {
				udf_dirent_free(dir);
				return false;
			}
		} else {
			int64_t length = udf_get_file_length(dir);
			if (scan_only) {
				check_iso_props(path, length, basename, fullpath);
				rep->projected_size += (uint64_t)length;
				continue;
			}
			udf_rd_t rd = { dir, (uint64_t)length };
			if (!write_file(fullpath, path, basename, length, udf_read_fn, &rd)) {
				udf_dirent_free(dir);
				return false;
			}
		}
	}
	return r;
}

/* ------------------------------------------------------------------------ */
/* Syslinux version (GetSyslinuxVersion in stdfn.c)                          */
/* ------------------------------------------------------------------------ */
static uint16_t get_syslinux_version(const uint8_t* buf, size_t size, char* str, size_t str_size)
{
	static const char* token[] = { "ISOLINUX ", "SYSLINUX ", "EXTLINUX " };
	size_t i, t;
	for (i = 0; i + 16 < size; i++) {
		for (t = 0; t < ARRAYSIZE(token); t++) {
			if (memcmp(&buf[i], token[t], 9) == 0 && isdigit(buf[i + 9]) && buf[i + 10] == '.' &&
				isdigit(buf[i + 11]) && isdigit(buf[i + 12])) {
				unsigned major = buf[i + 9] - '0', minor = (buf[i + 11] - '0') * 10 + (buf[i + 12] - '0');
				snprintf(str, str_size, "%u.%02u", major, minor);
				return (uint16_t)((major << 8) | minor);
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Raw (non-ISO) image detection                                             */
/* ------------------------------------------------------------------------ */
static uint8_t compression_from_ext(const char* path)
{
	static const struct { const char* ext; uint8_t type; } map[] = {
		{ ".zip", IMG_COMPRESSION_ZIP }, { ".Z", IMG_COMPRESSION_LZW }, { ".gz", IMG_COMPRESSION_GZIP },
		{ ".lzma", IMG_COMPRESSION_LZMA }, { ".bz2", IMG_COMPRESSION_BZIP2 }, { ".xz", IMG_COMPRESSION_XZ },
		{ ".vtsi", IMG_COMPRESSION_VTSI }, { ".zst", IMG_COMPRESSION_ZSTD },
	};
	size_t i;
	for (i = 0; i < ARRAYSIZE(map); i++)
		if (ends_with_ci(path, map[i].ext))
			return map[i].type;
	return IMG_COMPRESSION_NONE;
}

static int8_t is_bootable_image(const char* path, uint8_t compression)
{
	uint8_t buf[512 * 2];
	FILE* fp;
	size_t n;

	if (compression != IMG_COMPRESSION_NONE)
		return 1;	/* Compressed disk images are assumed to be DD images */
	fp = fopen(path, "rb");
	if (fp == NULL)
		return 0;
	n = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);
	if (n < sizeof(buf))
		return 0;
	if (memcmp(&buf[512], "EFI PART", 8) == 0)
		return 2;
	if (buf[510] == 0x55 && buf[511] == 0xaa) {
		/* Has an MBR: check that at least one partition entry looks valid */
		int i;
		for (i = 0; i < 4; i++) {
			const uint8_t* e = &buf[0x1be + 16 * i];
			if ((e[0] == 0x00 || e[0] == 0x80) && e[4] != 0)
				return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */
bool image_scan(const char* path, image_report_t* report)
{
	struct stat st;
	iso9660_t* iso = NULL;
	udf_t* udf = NULL;
	char* vol_id = NULL;
	bool r = false;

	memset(report, 0, sizeof(*report));
	rep = report;
	scan_only = true;
	isolinux_path[0] = 0;
	cdio_log_set_handler(cdio_log_handler);

	if (stat(path, &st) != 0) {
		uprintf("Could not access '%s'", path);
		return false;
	}
	report->image_size = (uint64_t)st.st_size;
	report->compression_type = compression_from_ext(path);
	report->is_bootable_img = is_bootable_image(path, report->compression_type);
	uprintf("Scanning image...");

	if (report->compression_type == IMG_COMPRESSION_NONE && rflags.enable_iso) {
		udf = udf_open(path);
		if (udf != NULL) {
			udf_dirent_t* root = udf_get_root(udf, true, 0);
			uprintf("  Image is a UDF image");
			report->is_iso = true;
			report->is_udf = true;
			if (udf_get_logical_volume_id(udf, report->label, sizeof(report->label)) <= 0)
				report->label[0] = 0;
			if (root != NULL)
				r = udf_walk(udf, root, "");
			udf_close(udf);
		}
		if (!report->is_iso) {
			/* Rock Ridge (no Joliet) first, to catch long names & symlinks, as Rufus does */
			iso = iso9660_open_ext(path, ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
			if (iso != NULL) {
				uprintf("  Image is an ISO9660 image");
				report->is_iso = true;
				if (iso9660_ifs_get_volume_id(iso, &vol_id)) {
					snprintf(report->label, sizeof(report->label), "%s", vol_id);
					free(vol_id);
				}
				r = iso_walk(iso, "", true);
				iso9660_close(iso);
			}
		}
	}

	if (report->is_iso && isolinux_path[0] != 0) {
		void* buf = NULL;
		size_t size = 0;
		if (extract_iso_file(path, isolinux_path, &buf, &size)) {
			report->sl_version = get_syslinux_version(buf, size, report->sl_version_str, sizeof(report->sl_version_str));
			free(buf);
		}
	}
	/* Windows install images need NTFS on FAT32-hostile sizes */
	if (report->has_4GB_file)
		report->needs_ntfs = true;
	if (!report->is_iso && report->is_bootable_img <= 0) {
		uprintf("  Image is not an ISO or bootable disk image");
		return false;
	}
	image_report_log(report, path);
	return report->is_iso ? r : true;
}

void image_report_log(const image_report_t* r, const char* path)
{
	int i;
	uprintf("Image: %s", path);
	uprintf("  Size: %s", size_to_human(r->image_size, false));
	if (r->label[0])
		uprintf("  Label: '%s'", r->label);
	if (r->is_iso) {
		uprintf("  Projected size: %s", size_to_human(r->projected_size, false));
		uprintf("  Has a >4GB file: %s", r->has_4GB_file ? "Yes" : "No");
		uprintf("  Uses EFI: %s%s", r->has_efi ? "Yes" : "No", (r->has_efi & 1) ? " (bootmgr.efi)" : "");
		uprintf("  Uses Bootmgr: %s", r->has_bootmgr ? "Yes" : "No");
		uprintf("  Uses WinPE: %s", r->winpe ? "Yes" : "No");
		for (i = 0; i < r->wininst_index; i++)
			uprintf("  Windows install image: %s", r->wininst_path[i]);
		if (HAS_SYSLINUX(r))
			uprintf("  Uses: Syslinux/Isolinux v%s (%s)", r->sl_version_str, r->cfg_path);
		if (r->has_grub2)
			uprintf("  Uses: GRUB2");
		if (r->has_grub4dos)
			uprintf("  Uses: Grub4DOS");
		if (r->has_kolibrios)
			uprintf("  Uses: KolibriOS");
		if (r->has_symlinks)
			uprintf("  Note: This ISO uses symbolic links, which will not be preserved");
		if (r->disable_iso)
			uprintf("  Note: This ISO must be written in DD Image mode");
	}
	if (r->is_bootable_img > 0)
		uprintf("  Can be written in DD mode: Yes (%s)", r->is_bootable_img == 2 ? "GPT" : "MBR");
	if (r->compression_type != IMG_COMPRESSION_NONE)
		uprintf("  Compressed image");
}

bool extract_iso(const char* iso_path, const image_report_t* report, sink_t* sink, fs_type_t fs, bool persistence)
{
	iso9660_t* iso = NULL;
	udf_t* udf = NULL;
	bool r = false;
	image_report_t dummy;

	scan_only = false;
	out_sink = sink;
	out_fs = fs;
	xrep = report;
	add_persistence = persistence;
	rep = &dummy;
	total_bytes = report->projected_size;
	done_bytes = 0;
	device_progress = (fs == FS_NTFS);
	device_done = 0;
	ntfs_io_write_hook = device_progress ? device_write_hook : NULL;
	snprintf(usb_label, sizeof(usb_label), "%s", report->label);
	to_valid_label(usb_label, IS_FAT(fs) || fs == FS_EXFAT);
	cdio_log_set_handler(cdio_log_handler);
	uprintf("Extracting files...");
	update_status("Copying ISO files...");

	if (report->is_udf) {
		udf = udf_open(iso_path);
		if (udf != NULL) {
			udf_dirent_t* root = udf_get_root(udf, true, 0);
			if (root != NULL)
				r = udf_walk(udf, root, "");
			udf_close(udf);
			goto out;
		}
	}
	/* Rufus uses Joliet for extraction, unless Rock Ridge has long names or symlinks */
	{
		iso_extension_mask_t mask = ISO_EXTENSION_ALL;
		if (!rflags.enable_joliet || (rflags.enable_rockridge && (report->has_long_filename || report->has_symlinks)))
			mask &= ~ISO_EXTENSION_JOLIET;
		if (!rflags.enable_rockridge)
			mask &= ~ISO_EXTENSION_ROCK_RIDGE;
		iso = iso9660_open_ext(iso_path, mask);
		if (iso == NULL) {
			uprintf("'%s' doesn't look like an ISO image", iso_path);
			goto out;
		}
		uprintf((mask & (ISO_EXTENSION_JOLIET | ISO_EXTENSION_ROCK_RIDGE)) ?
			"This image will be extracted using %s extensions (if present)" : "This image will not be extracted using any ISO extensions%s",
			(mask & ISO_EXTENSION_JOLIET) ? "Joliet" : (mask & ISO_EXTENSION_ROCK_RIDGE) ? "Rock Ridge" : "");
		r = iso_walk(iso, "", rflags.enable_rockridge);
	}
	iso9660_close(iso);
out:
	if (r)
		uprintf("Extraction complete (%s)", size_to_human(done_bytes, false));
	return r;
}

bool extract_iso_file(const char* iso_path, const char* src, void** buf, size_t* size)
{
	iso9660_t* iso;
	iso9660_stat_t* st;
	udf_t* udf;
	uint8_t* b;

	*buf = NULL;
	*size = 0;
	udf = udf_open(iso_path);
	if (udf != NULL) {
		udf_dirent_t* root = udf_get_root(udf, true, 0);
		udf_dirent_t* d = (root != NULL) ? udf_fopen(root, src) : NULL;
		bool r = false;
		if (d != NULL) {
			int64_t len = udf_get_file_length(d);
			if (len > 0 && len < 256 * (int64_t)MB && (b = malloc((size_t)len + UDF_BLOCKSIZE)) != NULL) {
				int64_t got = 0;
				while (got < len) {
					ssize_t n = udf_read_block(d, b + got, ((size_t)(len - got) + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE);
					if (n <= 0)
						break;
					got += n;
				}
				if (got >= len) {
					*buf = b;
					*size = (size_t)len;
					r = true;
				} else {
					free(b);
				}
			}
			udf_dirent_free(d);
		}
		if (root != NULL)
			udf_dirent_free(root);
		udf_close(udf);
		if (r)
			return true;
	}
	/* Paths recorded during the scan may be Joliet/Rock Ridge or plain ISO9660 ones */
	iso = iso9660_open_ext(iso_path, ISO_EXTENSION_ALL);
	if (iso == NULL)
		return false;
	st = iso9660_ifs_stat_translate(iso, src);
	if (st == NULL) {
		iso9660_close(iso);
		iso = iso9660_open_ext(iso_path, ISO_EXTENSION_NONE);
		if (iso == NULL)
			return false;
		st = iso9660_ifs_stat_translate(iso, src);
	}
	if (st == NULL || st->total_size == 0 || st->total_size > 256 * MB) {
		if (st != NULL)
			iso9660_stat_free(st);
		iso9660_close(iso);
		return false;
	}
	{
		long blocks = (long)((st->total_size + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
		b = malloc((size_t)blocks * ISO_BLOCKSIZE);
		if (b != NULL && iso9660_iso_seek_read(iso, b, st->lsn, blocks) == blocks * ISO_BLOCKSIZE) {
			*buf = b;
			*size = (size_t)st->total_size;
		} else {
			free(b);
		}
	}
	iso9660_stat_free(st);
	iso9660_close(iso);
	return *buf != NULL;
}
