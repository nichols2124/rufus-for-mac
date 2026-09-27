/*
 * Rufus for macOS: DD image writing and bad blocks check
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
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "rufus_core.h"

/* bled's public interface (bled.h pulls in windows.h, so declare what we use) */
typedef void (*bled_printf_t)(const char* format, ...);
typedef void (*bled_progress_t)(const int64_t read_bytes);
typedef int (*bled_read_t)(int fd, void* buf, unsigned int count);
typedef int (*bled_write_t)(int fd, const void* buf, unsigned int count);
typedef void (*bled_switch_t)(const char* filename, const uint64_t size);
int bled_init(uint32_t buffer_size, bled_printf_t print_function, bled_read_t read_function, bled_write_t write_function,
	bled_progress_t progress_function, bled_switch_t switch_function, unsigned long* cancel_request);
void bled_exit(void);
int64_t bled_uncompress_with_handles(void* hSrc, void* hDst, int type);

#define DD_BUFFER_SIZE  (4 * MB)

/* ------------------------------------------------------------------------ */
/* Compressed images: bled streams arbitrary sized chunks, which we          */
/* coalesce into sector aligned writes at the right offset.                  */
/* ------------------------------------------------------------------------ */
static rdev_t* dd_dev;
static uint8_t* dd_buf;
static size_t dd_buf_pos;
static uint64_t dd_offset;
static uint64_t dd_total;
static unsigned long bled_cancel;

static bool dd_flush(bool final)
{
	size_t len = dd_buf_pos;
	uint32_t ss = dd_dev->sector_size;
	if (len == 0)
		return true;
	if (!final)
		len = FLOOR_ALIGN(len, ss);
	else if (len % ss != 0) {
		/* Pad the last sector with zeroes */
		size_t padded = CEILING_ALIGN(len, ss);
		memset(&dd_buf[len], 0, padded - len);
		len = padded;
	}
	if (dd_offset + len > dd_dev->size) {
		uprintf("The image is larger than the target drive");
		return false;
	}
	if (!rdev_write(dd_dev, dd_offset, dd_buf, len))
		return false;
	dd_offset += len;
	if (len < dd_buf_pos)
		memmove(dd_buf, &dd_buf[len], dd_buf_pos - len);
	dd_buf_pos = (len < dd_buf_pos) ? dd_buf_pos - len : 0;
	return true;
}

static int dd_bled_write(int fd, const void* buf, unsigned int count)
{
	const uint8_t* p = buf;
	unsigned int left = count;
	(void)fd;
	while (left > 0) {
		size_t n = DD_BUFFER_SIZE - dd_buf_pos;
		if (n > left)
			n = left;
		memcpy(&dd_buf[dd_buf_pos], p, n);
		dd_buf_pos += n;
		p += n;
		left -= (unsigned int)n;
		if (dd_buf_pos == DD_BUFFER_SIZE && !dd_flush(false))
			return -1;
	}
	return (int)count;
}

static int dd_bled_read(int fd, void* buf, unsigned int count)
{
	return (int)read(fd, buf, count);
}

static void dd_bled_progress(const int64_t bytes)
{
	if (bytes < 0) {
		dd_total = (uint64_t)(-bytes);
		return;
	}
	if (cancel_requested)
		bled_cancel = 1;
	update_progress_bytes(NULL, (uint64_t)bytes, dd_total);
}

static void dd_bled_printf(const char* format, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	uprintf("%s", buf);
}

bool write_dd_image(rdev_t* dev, const char* image_path, const image_report_t* report)
{
	int src = open(image_path, O_RDONLY);
	bool r = false;

	if (src < 0) {
		uprintf("Could not open image '%s': %s", image_path, strerror(errno));
		return false;
	}
	dd_buf = aligned_alloc(4096, DD_BUFFER_SIZE);
	if (dd_buf == NULL)
		goto out;
	dd_dev = dev;
	dd_buf_pos = 0;
	dd_offset = 0;
	update_status("Writing image...");
	uprintf("Writing image '%s' in DD mode...", image_path);

	if (report->compression_type != IMG_COMPRESSION_NONE) {
		int64_t res;
		bled_cancel = 0;
		dd_total = report->image_size;
		bled_init(256 * KB, dd_bled_printf, dd_bled_read, dd_bled_write, dd_bled_progress, NULL, &bled_cancel);
		/* The destination fd is only handed back to dd_bled_write(), which ignores it */
		res = bled_uncompress_with_handles((void*)(intptr_t)src, (void*)(intptr_t)dev->fd, report->compression_type);
		bled_exit();
		if (res < 0 || cancel_requested) {
			uprintf("Could not decompress image");
			goto out;
		}
		r = dd_flush(true);
	} else {
		uint64_t done = 0;
		if (report->image_size > dev->size) {
			uprintf("The image (%s) is larger than the target drive", size_to_human(report->image_size, false));
			goto out;
		}
		while (done < report->image_size) {
			ssize_t n = read(src, dd_buf, DD_BUFFER_SIZE);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				uprintf("Read error on image: %s", n < 0 ? strerror(errno) : "unexpected end of file");
				goto out;
			}
			if (cancel_requested)
				goto out;
			dd_buf_pos = (size_t)n;
			if (!dd_flush(done + (uint64_t)n >= report->image_size))
				goto out;
			done += (uint64_t)n;
			update_progress_bytes(NULL, done, report->image_size);
		}
		r = true;
	}
	if (r) {
		rdev_sync(dev);
		uprintf("Wrote %s", size_to_human(dd_offset, false));
	}
out:
	free(dd_buf);
	dd_buf = NULL;
	close(src);
	return r;
}

/* ------------------------------------------------------------------------ */
/* Bad blocks (destructive read/write test, like badblocks -w)               */
/* ------------------------------------------------------------------------ */
bool check_bad_blocks(rdev_t* dev, int passes, uint64_t* bad_count)
{
	static const uint8_t patterns[] = { 0xaa, 0x55, 0xff, 0x00 };
	const size_t block = 1 * MB;
	uint8_t *wbuf = aligned_alloc(4096, block), *rbuf = aligned_alloc(4096, block);
	uint64_t off, bad = 0;
	int p;
	bool r = false;

	*bad_count = 0;
	if (wbuf == NULL || rbuf == NULL)
		goto out;
	if (passes < 1)
		passes = 1;
	if (passes > 4)
		passes = 4;
	for (p = 0; p < passes; p++) {
		memset(wbuf, patterns[p], block);
		update_status("Bad Blocks: Pass %d/%d - Writing 0x%02X", p + 1, passes, patterns[p]);
		uprintf("Bad blocks: pass %d/%d, writing pattern 0x%02X", p + 1, passes, patterns[p]);
		for (off = 0; off < dev->size; off += block) {
			size_t n = (dev->size - off < block) ? (size_t)(dev->size - off) : block;
			if (cancel_requested)
				goto out;
			if (!rdev_write(dev, off, wbuf, n))
				bad += n / dev->sector_size;
			update_progress_bytes(NULL, off + n, dev->size * 2);
		}
		rdev_sync(dev);
		update_status("Bad Blocks: Pass %d/%d - Reading 0x%02X", p + 1, passes, patterns[p]);
		for (off = 0; off < dev->size; off += block) {
			size_t n = (dev->size - off < block) ? (size_t)(dev->size - off) : block, i;
			if (cancel_requested)
				goto out;
			if (!rdev_read(dev, off, rbuf, n)) {
				bad += n / dev->sector_size;
			} else if (memcmp(wbuf, rbuf, n) != 0) {
				for (i = 0; i < n; i += dev->sector_size)
					if (memcmp(&wbuf[i], &rbuf[i], dev->sector_size) != 0)
						bad++;
			}
			update_progress_bytes(NULL, dev->size + off + n, dev->size * 2);
		}
	}
	*bad_count = bad;
	r = true;
out:
	free(wbuf);
	free(rbuf);
	return r;
}

/* ------------------------------------------------------------------------ */
/* Zero the drive (Alt-Z), optionally skipping blocks that are already empty */
/* (Ctrl-Alt-Z), which is much faster on flash media that reads as zero.     */
/* ------------------------------------------------------------------------ */
bool zero_drive(rdev_t* dev, bool skip_empty)
{
	const size_t block = 4 * MB;
	uint8_t* zero = calloc(1, block), *buf = skip_empty ? malloc(block) : NULL;
	uint64_t off, skipped = 0;
	bool r = false;

	if (zero == NULL || (skip_empty && buf == NULL))
		goto out;
	update_status(skip_empty ? "Zeroing drive (fast)..." : "Zeroing drive...");
	uprintf("Zeroing drive%s...", skip_empty ? " (skipping empty blocks)" : "");
	for (off = 0; off < dev->size; off += block) {
		size_t n = (dev->size - off < block) ? (size_t)(dev->size - off) : block;
		if (cancel_requested)
			goto out;
		if (skip_empty && rdev_read(dev, off, buf, n) && memcmp(buf, zero, n) == 0) {
			skipped += n;
		} else if (!rdev_write(dev, off, zero, n)) {
			goto out;
		}
		update_progress_bytes(NULL, off + n, dev->size);
	}
	rdev_sync(dev);
	if (skip_empty)
		uprintf("Skipped %s of already empty blocks", size_to_human(skipped, false));
	r = true;
out:
	free(zero);
	free(buf);
	return r;
}
