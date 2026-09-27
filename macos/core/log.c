/*
 * Rufus for macOS: logging, status and progress reporting
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "rufus_core.h"

const char* fs_name[FS_MAX] = { "FAT", "FAT32", "NTFS", "UDF", "exFAT", "ReFS", "ext2", "ext3", "ext4" };

volatile bool cancel_requested = false;

rufus_flags_t rflags = {
	.enable_iso = true,
	.enable_joliet = true,
	.enable_rockridge = true,
	.size_check = true,
	.lock_drive = true,
	.detect_fakes = true,
};

static log_handler_t log_handler = NULL;
static progress_handler_t progress_handler = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char current_status[256] = "";

void set_log_handler(log_handler_t handler) { log_handler = handler; }
void set_progress_handler(progress_handler_t handler) { progress_handler = handler; }

void uprintf(const char* format, ...)
{
	char buf[4096];
	va_list args;
	size_t len;

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	/* Some of the libraries we borrow from Windows Rufus add their own newlines */
	len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = 0;

	pthread_mutex_lock(&log_mutex);
	if (log_handler != NULL)
		log_handler(buf);
	else
		fprintf(stderr, "%s\n", buf);
	pthread_mutex_unlock(&log_mutex);
}

void update_status(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(current_status, sizeof(current_status), format, args);
	va_end(args);
	if (progress_handler != NULL)
		progress_handler(current_status, -1.0);
}

void update_progress(double percent)
{
	static double last = -2.0;
	if (percent > 100.0)
		percent = 100.0;
	/* Don't flood the UI: only report changes of 0.1% or more */
	if (percent >= 0.0 && last >= 0.0 && percent > last && percent - last < 0.1 && percent < 100.0)
		return;
	last = percent;
	if (progress_handler != NULL)
		progress_handler(current_status, percent);
}

void update_progress_bytes(const char* label, uint64_t done, uint64_t total)
{
	(void)label;
	if (total == 0)
		return;
	update_progress(100.0 * (double)done / (double)total);
}

const char* size_to_human(uint64_t size, bool round)
{
	static __thread char str[32];
	static const char* suffix[] = { "bytes", "KB", "MB", "GB", "TB", "PB" };
	double hr = (double)size;
	int s = 0;

	while (s < 5 && hr >= 1024.0) {
		hr /= 1024.0;
		s++;
	}
	if (s == 0)
		snprintf(str, sizeof(str), "%llu %s", (unsigned long long)size, suffix[0]);
	else if (round)
		snprintf(str, sizeof(str), "%.0f %s", hr, suffix[s]);
	else
		snprintf(str, sizeof(str), (hr - (int)hr < 0.05) ? "%.0f %s" : "%.1f %s", hr, suffix[s]);
	return str;
}

const char* strerror_last(void)
{
	return strerror(errno);
}
