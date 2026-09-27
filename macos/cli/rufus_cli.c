/*
 * Rufus for macOS: command line front-end (used for testing the engine)
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Examples:
 *   rufus-cli --create 8G --image Win11.iso --gpt --uefi --fs ntfs test.img
 *   rufus-cli --scan Win11.iso
 *   sudo rufus-cli --image ubuntu.iso /dev/rdisk4       (real device)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <getopt.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <limits.h>

#include "rufus_core.h"

static void progress(const char* status, double pct)
{
	static char last_status[256] = "";
	if (strcmp(status, last_status) != 0) {
		snprintf(last_status, sizeof(last_status), "%s", status);
		fprintf(stderr, "[status] %s\n", status);
	}
	if (pct >= 0)
		fprintf(stderr, "\r[%5.1f%%]", pct);
	if (pct >= 100.0)
		fprintf(stderr, "\n");
}

static uint64_t parse_size(const char* s)
{
	char* end;
	double v = strtod(s, &end);
	switch (*end) {
	case 'k': case 'K': return (uint64_t)(v * KB);
	case 'm': case 'M': return (uint64_t)(v * MB);
	case 'g': case 'G': return (uint64_t)(v * GB);
	case 't': case 'T': return (uint64_t)(v * TB);
	default: return (uint64_t)v;
	}
}

static fs_type_t parse_fs(const char* s)
{
	int i;
	for (i = 0; i < FS_MAX; i++)
		if (strcasecmp(s, fs_name[i]) == 0)
			return (fs_type_t)i;
	if (strcasecmp(s, "fat16") == 0)
		return FS_FAT16;
	fprintf(stderr, "Unknown file system '%s'\n", s);
	exit(1);
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: rufus-cli [options] <target>\n"
		"  --scan <image>        analyse an image and exit\n"
		"  --image <path>        ISO or disk image (default: non bootable)\n"
		"  --freedos             make a FreeDOS bootable drive\n"
		"  --dd                  write the image in DD mode\n"
		"  --mbr | --gpt         partition scheme (default: MBR)\n"
		"  --bios | --uefi       target system (default: BIOS)\n"
		"  --fs <fs>             FAT32, NTFS, exFAT, FAT (default: FAT32)\n"
		"  --cluster <size>      cluster size\n"
		"  --label <label>       volume label\n"
		"  --full                full (non quick) format\n"
		"  --badblocks <passes>  check for bad blocks first\n"
		"  --create <size>       create <target> as an image file of this size\n"
		"  --res <dir>           resource directory\n"
		"  --persistence <size>  add a Linux persistence partition\n"
		"  --wue <flags>         Windows customization flags (see UNATTEND_*)\n"
		"  --user <name>         local account name for --wue\n"
		"  --zero                zero the whole drive\n");
	exit(1);
}

int main(int argc, char** argv)
{
	static struct option long_opts[] = {
		{ "scan", required_argument, 0, 's' }, { "image", required_argument, 0, 'i' },
		{ "freedos", no_argument, 0, 'D' }, { "dd", no_argument, 0, 'd' },
		{ "mbr", no_argument, 0, 'm' }, { "gpt", no_argument, 0, 'g' },
		{ "bios", no_argument, 0, 'b' }, { "uefi", no_argument, 0, 'u' },
		{ "fs", required_argument, 0, 'f' }, { "cluster", required_argument, 0, 'c' },
		{ "label", required_argument, 0, 'l' }, { "full", no_argument, 0, 'F' },
		{ "badblocks", required_argument, 0, 'B' }, { "create", required_argument, 0, 'C' },
		{ "res", required_argument, 0, 'r' }, { "persistence", required_argument, 0, 'P' },
		{ "wue", required_argument, 0, 'W' }, { "user", required_argument, 0, 'U' },
		{ "zero", no_argument, 0, 'Z' }, { 0, 0, 0, 0 } };
	static wue_options_t wue = { 0 };
	job_options_t o = { 0 };
	image_report_t report;
	uint64_t create_size = 0;
	char exe[PATH_MAX], resolved[PATH_MAX];
	uint32_t exe_len = sizeof(exe);
	const char* scan = NULL;
	rdev_t dev;
	int c, rc;

	/* Helpers sit next to us; resources default to the source tree's res/ */
	if (_NSGetExecutablePath(exe, &exe_len) == 0 && realpath(exe, resolved) != NULL) {
		snprintf(helper_dir, sizeof(helper_dir), "%s", dirname(resolved));
		snprintf(resource_dir, sizeof(resource_dir), "%s/../../res", helper_dir);
	}
	o.boot_type = BT_NON_BOOTABLE;
	o.fs = FS_FAT32;
	o.quick_format = true;

	while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
		switch (c) {
		case 's': scan = optarg; break;
		case 'i': o.image_path = optarg; o.boot_type = BT_IMAGE; break;
		case 'D': o.boot_type = BT_FREEDOS; break;
		case 'd': o.write_as_image = true; break;
		case 'm': o.part_style = PS_MBR; break;
		case 'g': o.part_style = PS_GPT; break;
		case 'b': o.target = TT_BIOS; break;
		case 'u': o.target = TT_UEFI; break;
		case 'f': o.fs = parse_fs(optarg); break;
		case 'c': o.cluster_size = (uint32_t)parse_size(optarg); break;
		case 'l': snprintf(o.label, sizeof(o.label), "%s", optarg); break;
		case 'F': o.quick_format = false; break;
		case 'B': o.bad_block_passes = atoi(optarg); break;
		case 'C': create_size = parse_size(optarg); break;
		case 'r': snprintf(resource_dir, sizeof(resource_dir), "%s", optarg); break;
		case 'P': o.persistence_size = parse_size(optarg); break;
		case 'W': wue.flags = (int)strtol(optarg, NULL, 0); snprintf(wue.locale, sizeof(wue.locale), "en-US"); o.wue = &wue; break;
		case 'U': snprintf(wue.username, sizeof(wue.username), "%s", optarg); break;
		case 'Z': o.zero_drive = 1; break;
		default: usage();
		}
	}
	set_progress_handler(progress);

	if (scan != NULL)
		return image_scan(scan, &report) ? 0 : 1;
	if (optind != argc - 1)
		usage();

	if (o.boot_type == BT_IMAGE) {
		if (!image_scan(o.image_path, &report))
			return 1;
		o.report = &report;
		if (o.label[0] == 0)
			snprintf(o.label, sizeof(o.label), "%s", report.label);
	}
	if (!rdev_open_file(&dev, argv[optind], create_size != 0, create_size))
		return 1;
	rc = run_job(&dev, &o);
	rdev_close(&dev);
	return rc == 0 ? 0 : 1;
}
