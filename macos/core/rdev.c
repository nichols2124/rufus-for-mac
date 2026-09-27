/*
 * Rufus for macOS: raw device access
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Character devices (/dev/rdiskN) on macOS only accept I/O that is aligned
 * to the sector size, both in offset and length. All writes from the rest of
 * the engine go through here, so unaligned requests are turned into
 * read-modify-write sequences.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disk.h>

#include "rufus_core.h"

#define WRITE_RETRIES 4

bool rdev_attach_fd(rdev_t* dev, int fd, const char* path)
{
	struct stat st;
	uint32_t block_size = 0;
	uint64_t block_count = 0;

	memset(dev, 0, sizeof(*dev));
	dev->fd = fd;
	snprintf(dev->path, sizeof(dev->path), "%s", path);
	if (fstat(fd, &st) != 0) {
		uprintf("Could not stat '%s': %s", path, strerror(errno));
		return false;
	}
	if (S_ISREG(st.st_mode)) {
		dev->is_file = true;
		dev->size = st.st_size;
		dev->sector_size = 512;
	} else {
		if (ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) != 0 ||
			ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count) != 0) {
			uprintf("Could not query geometry of '%s': %s", path, strerror(errno));
			return false;
		}
		dev->sector_size = block_size;
		dev->size = block_count * block_size;
	}
	if (dev->sector_size < 512 || (dev->sector_size & (dev->sector_size - 1)) != 0) {
		uprintf("Unsupported sector size %u for '%s'", dev->sector_size, path);
		return false;
	}
	uprintf("Opened %s '%s': %s (%llu bytes), sector size %u", dev->is_file ? "image file" : "device",
		path, size_to_human(dev->size, false), (unsigned long long)dev->size, dev->sector_size);
	return true;
}

bool rdev_open_file(rdev_t* dev, const char* path, bool create, uint64_t create_size)
{
	int fd = open(path, O_RDWR | (create ? O_CREAT : 0), 0644);
	if (fd < 0) {
		uprintf("Could not open '%s': %s", path, strerror(errno));
		return false;
	}
	if (create && create_size != 0 && ftruncate(fd, (off_t)create_size) != 0) {
		uprintf("Could not size '%s': %s", path, strerror(errno));
		close(fd);
		return false;
	}
	if (!rdev_attach_fd(dev, fd, path)) {
		close(fd);
		return false;
	}
	return true;
}

void rdev_close(rdev_t* dev)
{
	if (dev->fd >= 0) {
		rdev_sync(dev);
		close(dev->fd);
	}
	dev->fd = -1;
}

bool rdev_sync(rdev_t* dev)
{
	if (dev->fd < 0)
		return false;
	if (!dev->is_file)
		(void)ioctl(dev->fd, DKIOCSYNCHRONIZECACHE);
	return fsync(dev->fd) == 0 || errno == ENOTSUP || errno == EINVAL;
}

static bool pwrite_full(int fd, const void* buf, size_t len, uint64_t offset)
{
	const uint8_t* p = buf;
	int retry = 0;
	while (len > 0) {
		ssize_t w = pwrite(fd, p, len, (off_t)offset);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (++retry <= WRITE_RETRIES) {
				uprintf("Write error at offset %llu (%s), retrying...", (unsigned long long)offset, strerror(errno));
				usleep(250000);
				continue;
			}
			uprintf("Write error at offset %llu: %s", (unsigned long long)offset, strerror(errno));
			return false;
		}
		if (w == 0) {
			uprintf("Short write at offset %llu", (unsigned long long)offset);
			return false;
		}
		p += w;
		len -= (size_t)w;
		offset += (uint64_t)w;
	}
	return true;
}

static bool pread_full(int fd, void* buf, size_t len, uint64_t offset)
{
	uint8_t* p = buf;
	while (len > 0) {
		ssize_t r = pread(fd, p, len, (off_t)offset);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			uprintf("Read error at offset %llu: %s", (unsigned long long)offset, strerror(errno));
			return false;
		}
		if (r == 0) {
			/* Reading past the end of an image file: return zeroes */
			memset(p, 0, len);
			return true;
		}
		p += r;
		len -= (size_t)r;
		offset += (uint64_t)r;
	}
	return true;
}

bool rdev_read(rdev_t* dev, uint64_t offset, void* buf, size_t len)
{
	uint32_t ss = dev->sector_size;
	uint64_t start, end;
	uint8_t* tmp;
	bool r;

	if (offset + len > dev->size) {
		uprintf("Read beyond end of device (offset %llu, len %zu)", (unsigned long long)offset, len);
		return false;
	}
	if (dev->is_file || ((offset % ss) == 0 && (len % ss) == 0))
		return pread_full(dev->fd, buf, len, offset);

	start = FLOOR_ALIGN(offset, ss);
	end = CEILING_ALIGN(offset + len, ss);
	tmp = malloc(end - start);
	if (tmp == NULL)
		return false;
	r = pread_full(dev->fd, tmp, end - start, start);
	if (r)
		memcpy(buf, tmp + (offset - start), len);
	free(tmp);
	return r;
}

bool rdev_write(rdev_t* dev, uint64_t offset, const void* buf, size_t len)
{
	uint32_t ss = dev->sector_size;
	uint64_t start, end;
	uint8_t* tmp;
	bool r;

	if (len == 0)
		return true;
	if (offset + len > dev->size) {
		uprintf("Write beyond end of device (offset %llu, len %zu, size %llu)",
			(unsigned long long)offset, len, (unsigned long long)dev->size);
		return false;
	}
	if (dev->is_file || ((offset % ss) == 0 && (len % ss) == 0))
		return pwrite_full(dev->fd, buf, len, offset);

	/* Read-modify-write the partial sectors */
	start = FLOOR_ALIGN(offset, ss);
	end = CEILING_ALIGN(offset + len, ss);
	tmp = malloc(end - start);
	if (tmp == NULL)
		return false;
	r = pread_full(dev->fd, tmp, end - start, start);
	if (r) {
		memcpy(tmp + (offset - start), buf, len);
		r = pwrite_full(dev->fd, tmp, end - start, start);
	}
	free(tmp);
	return r;
}

bool rdev_zero(rdev_t* dev, uint64_t offset, uint64_t len)
{
	const size_t chunk = 1 * MB;
	uint8_t* zero = calloc(1, chunk);
	bool r = true;

	if (zero == NULL)
		return false;
	while (r && len > 0) {
		size_t n = (len > chunk) ? chunk : (size_t)len;
		r = rdev_write(dev, offset, zero, n);
		offset += n;
		len -= n;
	}
	free(zero);
	return r;
}
