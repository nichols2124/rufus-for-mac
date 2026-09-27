/*
 * Rufus for macOS: libntfs-3g device driver for a partition inside a raw fd
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Device names have the form "rufus:<fd>:<offset>:<size>:<sector_size>",
 * where <fd> is an already opened (and inherited, for rufus-mkntfs) raw disk
 * or image file descriptor. This lets both mkntfs and the NTFS file writer
 * work on a partition without macOS having to create or mount /dev/diskNsM.
 *
 * Raw devices (/dev/rdiskN) are unbuffered on macOS, whereas ntfs-3g expects
 * the buffering that Linux block devices provide: mkntfs writes its metadata
 * in 4 KB chunks, and libntfs-3g re-reads MFT records and directory indexes
 * (1-8 KB each) for every file it creates. Unbuffered, a USB flash drive does
 * a few dozen of these per second, i.e. well under 1 MB/s.
 *
 * So this driver has its own write-back block cache (see below), which gives
 * the drive large, sorted, sequential writes, like the OS page cache would on
 * Windows or Linux. This is safe because Rufus has exclusive access to the
 * drive: macOS is prevented from mounting it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/disk.h>
#include <sys/time.h>

#include <ntfs-3g/types.h>
#include <ntfs-3g/device.h>

#include "ntfs_io.h"

#define BLOCK_SIZE      (256 * 1024)
#define UNIT            512                          /* validity tracking granularity */
#define UNITS           (BLOCK_SIZE / UNIT)
#define CACHE_BLOCKS    1024                         /* 256 MB */
#define HASH_BUCKETS    2048
#define FLUSH_RUN_MAX   (16 * 1024 * 1024)

typedef struct cblk {
	uint64_t idx;
	bool used, dirty, complete;
	uint64_t valid[UNITS / 64];  /* which 512-byte units hold real data */
	uint8_t* data;
	struct cblk *prev, *next;   /* LRU list, most recent first */
	struct cblk* hnext;         /* hash chain */
} cblk_t;

typedef struct {
	int fd;
	uint64_t offset;
	uint64_t size;
	uint32_t sector_size;
	s64 pos;
	cblk_t* blocks;
	cblk_t* bucket[HASH_BUCKETS];
	cblk_t *lru_head, *lru_tail;
	uint8_t* run;               /* scratch buffer for merged flushes */
	uint8_t* fill;              /* scratch buffer to complete partial blocks */
	uint64_t reads_in, reads_out, writes_in, writes_out, bytes_out;
} rufus_ntfs_dev_t;

bool rufus_ntfs_parse_name(const char* name, int* fd, uint64_t* offset, uint64_t* size, uint32_t* ss)
{
	unsigned long long o, s;
	unsigned int sec;
	int f;
	if (name == NULL || sscanf(name, "rufus:%d:%llu:%llu:%u", &f, &o, &s, &sec) != 4)
		return false;
	*fd = f;
	*offset = o;
	*size = s;
	*ss = sec;
	return true;
}

/* ------------------------------------------------------------------------ */
/* Direct device access                                                      */
/* ------------------------------------------------------------------------ */

/* Debug aid: RUFUS_IO_TRACE=<file> records every device access (op, offset, size, microseconds) */
static FILE* trace_fp = NULL;
static bool trace_init = false;

static uint64_t now_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

static s64 raw_xfer_impl(rufus_ntfs_dev_t* p, void* buf, s64 count, s64 pos, bool is_write);

/* Sector-aligned transfer (handles unaligned requests with a bounce buffer) */
void (*ntfs_io_write_hook)(uint64_t bytes) = NULL;

static s64 raw_xfer_traced(rufus_ntfs_dev_t* p, void* buf, s64 count, s64 pos, bool is_write);

static s64 raw_xfer(rufus_ntfs_dev_t* p, void* buf, s64 count, s64 pos, bool is_write)
{
	s64 r = raw_xfer_traced(p, buf, count, pos, is_write);
	if (is_write && r > 0 && ntfs_io_write_hook != NULL)
		ntfs_io_write_hook((uint64_t)r);
	return r;
}

static s64 raw_xfer_traced(rufus_ntfs_dev_t* p, void* buf, s64 count, s64 pos, bool is_write)
{
	uint64_t t0;
	s64 r;
	if (!trace_init) {
		const char* path = getenv("RUFUS_IO_TRACE");
		trace_init = true;
		if (path != NULL)
			trace_fp = fopen(path, "a");
		if (trace_fp != NULL)
			setvbuf(trace_fp, NULL, _IOLBF, 0);   /* survive Ctrl-C */
	}
	if (trace_fp == NULL)
		return raw_xfer_impl(p, buf, count, pos, is_write);
	t0 = now_us();
	r = raw_xfer_impl(p, buf, count, pos, is_write);
	fprintf(trace_fp, "%c %lld %lld %llu\n", is_write ? 'W' : 'R', (long long)pos, (long long)count,
		(unsigned long long)(now_us() - t0));
	return r;
}

static s64 raw_xfer_impl(rufus_ntfs_dev_t* p, void* buf, s64 count, s64 pos, bool is_write)
{
	uint64_t ss = p->sector_size, start, end, len;
	uint8_t* tmp;
	ssize_t r;

	if (pos < 0 || count < 0) {
		errno = EINVAL;
		return -1;
	}
	if ((uint64_t)pos >= p->size)
		return 0;
	if ((uint64_t)(pos + count) > p->size)
		count = (s64)(p->size - (uint64_t)pos);
	if (count == 0)
		return 0;
	if (is_write)
		p->writes_out++;
	else
		p->reads_out++;

	if (((uint64_t)pos % ss) == 0 && ((uint64_t)count % ss) == 0) {
		s64 done = 0;
		while (done < count) {
			r = is_write ? pwrite(p->fd, (uint8_t*)buf + done, (size_t)(count - done), (off_t)(p->offset + pos + done))
			             : pread(p->fd, (uint8_t*)buf + done, (size_t)(count - done), (off_t)(p->offset + pos + done));
			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0)
				return (done > 0) ? done : -1;
			done += r;
		}
		return done;
	}

	start = FLOOR(pos, ss);
	end = CEIL(pos + count, ss);
	len = end - start;
	tmp = malloc(len);
	if (tmp == NULL) {
		errno = ENOMEM;
		return -1;
	}
	r = pread(p->fd, tmp, len, (off_t)(p->offset + start));
	if (r < 0) {
		free(tmp);
		return -1;
	}
	if (is_write) {
		memcpy(tmp + (pos - start), buf, (size_t)count);
		r = pwrite(p->fd, tmp, len, (off_t)(p->offset + start));
	} else {
		memcpy(buf, tmp + (pos - start), (size_t)count);
	}
	free(tmp);
	return (r < 0) ? -1 : count;
}

/* ------------------------------------------------------------------------ */
/* Block cache                                                               */
/*                                                                           */
/* Behaves like an OS write-back cache: everything, file data included, is   */
/* written to memory first. When space runs out, *all* dirty blocks are      */
/* written, sorted and merged into large sequential runs, which is what      */
/* flash drives are fast at. Blocks track which 512 byte units hold data, so */
/* a block that is being filled sequentially is never read from the drive.   */
/* ------------------------------------------------------------------------ */
static uint64_t block_len(rufus_ntfs_dev_t* p, uint64_t idx)
{
	uint64_t start = idx * BLOCK_SIZE;
	return (p->size - start < BLOCK_SIZE) ? p->size - start : BLOCK_SIZE;
}

static void lru_unlink(rufus_ntfs_dev_t* p, cblk_t* b)
{
	if (b->prev) b->prev->next = b->next; else p->lru_head = b->next;
	if (b->next) b->next->prev = b->prev; else p->lru_tail = b->prev;
	b->prev = b->next = NULL;
}

static void lru_push_front(rufus_ntfs_dev_t* p, cblk_t* b)
{
	b->prev = NULL;
	b->next = p->lru_head;
	if (p->lru_head) p->lru_head->prev = b;
	p->lru_head = b;
	if (p->lru_tail == NULL) p->lru_tail = b;
}

static void hash_remove(rufus_ntfs_dev_t* p, cblk_t* b)
{
	cblk_t** pp = &p->bucket[b->idx % HASH_BUCKETS];
	while (*pp != NULL && *pp != b)
		pp = &(*pp)->hnext;
	if (*pp == b)
		*pp = b->hnext;
	b->hnext = NULL;
}

static cblk_t* lookup(rufus_ntfs_dev_t* p, uint64_t idx)
{
	cblk_t* b = p->bucket[idx % HASH_BUCKETS];
	while (b != NULL && b->idx != idx)
		b = b->hnext;
	return b;
}

static bool units_valid(const cblk_t* b, uint64_t first, uint64_t last)
{
	uint64_t u;
	if (b->complete)
		return true;
	for (u = first; u <= last; u++)
		if (!(b->valid[u / 64] & (1ULL << (u % 64))))
			return false;
	return true;
}

static void set_units_valid(cblk_t* b, uint64_t first, uint64_t last, uint64_t nunits)
{
	uint64_t u;
	for (u = first; u <= last; u++)
		b->valid[u / 64] |= (1ULL << (u % 64));
	b->complete = units_valid(b, 0, nunits - 1);
}

/* Read the parts of a block that we don't have yet from the drive */
static bool complete_block(rufus_ntfs_dev_t* p, cblk_t* b)
{
	uint64_t len = block_len(p, b->idx), nunits = (len + UNIT - 1) / UNIT, u;
	if (b->complete)
		return true;
	if (raw_xfer(p, p->fill, (s64)len, (s64)(b->idx * BLOCK_SIZE), false) != (s64)len) {
		errno = EIO;
		return false;
	}
	for (u = 0; u < nunits; u++) {
		if (!(b->valid[u / 64] & (1ULL << (u % 64)))) {
			uint64_t n = (u == nunits - 1) ? len - u * UNIT : UNIT;
			memcpy(b->data + u * UNIT, p->fill + u * UNIT, (size_t)n);
		}
	}
	memset(b->valid, 0xff, sizeof(b->valid));
	b->complete = true;
	return true;
}

static int compare_blocks(const void* a, const void* b)
{
	uint64_t x = (*(cblk_t* const*)a)->idx, y = (*(cblk_t* const*)b)->idx;
	return (x > y) - (x < y);
}

/* Write all dirty blocks, sorted and merged into large sequential writes */
static int cache_flush(rufus_ntfs_dev_t* p)
{
	static cblk_t* dirty[CACHE_BLOCKS];
	int i, n = 0, r = 0;

	if (p == NULL || p->blocks == NULL)
		return 0;
	for (i = 0; i < CACHE_BLOCKS; i++)
		if (p->blocks[i].used && p->blocks[i].dirty)
			dirty[n++] = &p->blocks[i];
	qsort(dirty, (size_t)n, sizeof(dirty[0]), compare_blocks);
	for (i = 0; i < n; ) {
		uint64_t first = dirty[i]->idx, len = 0;
		int j = i;
		while (j < n && dirty[j]->idx == first + (uint64_t)(j - i) && len + BLOCK_SIZE <= FLUSH_RUN_MAX) {
			uint64_t bl = block_len(p, dirty[j]->idx);
			if (!complete_block(p, dirty[j]))
				return -1;
			memcpy(p->run + len, dirty[j]->data, (size_t)bl);
			len += bl;
			j++;
		}
		if (raw_xfer(p, p->run, (s64)len, (s64)(first * BLOCK_SIZE), true) != (s64)len) {
			errno = EIO;
			r = -1;
		} else {
			p->bytes_out += len;
			for (; i < j; i++)
				dirty[i]->dirty = false;
		}
		i = j;
	}
	return r;
}

/* Get block 'idx' in the cache (its content may be incomplete, see valid[]) */
static cblk_t* get_block(rufus_ntfs_dev_t* p, uint64_t idx)
{
	cblk_t* b = lookup(p, idx);

	if (b != NULL) {
		lru_unlink(p, b);
		lru_push_front(p, b);
		return b;
	}
	/* Recycle the least recently used block. If it's dirty, the cache is full
	 * of pending writes: write them all out in one sorted pass. */
	b = p->lru_tail;
	if (b->used && b->dirty && cache_flush(p) != 0)
		return NULL;
	if (b->used)
		hash_remove(p, b);
	lru_unlink(p, b);
	b->idx = idx;
	b->used = true;
	b->dirty = false;
	b->complete = false;
	memset(b->valid, 0, sizeof(b->valid));
	b->hnext = p->bucket[idx % HASH_BUCKETS];
	p->bucket[idx % HASH_BUCKETS] = b;
	lru_push_front(p, b);
	return b;
}

static bool cache_init(rufus_ntfs_dev_t* p)
{
	int i;
	p->blocks = calloc(CACHE_BLOCKS, sizeof(cblk_t));
	p->run = malloc(FLUSH_RUN_MAX);
	p->fill = malloc(BLOCK_SIZE);
	if (p->blocks == NULL || p->run == NULL || p->fill == NULL)
		return false;
	for (i = 0; i < CACHE_BLOCKS; i++) {
		p->blocks[i].data = malloc(BLOCK_SIZE);
		if (p->blocks[i].data == NULL)
			return false;
		lru_push_front(p, &p->blocks[i]);
	}
	return true;
}

static void cache_free(rufus_ntfs_dev_t* p)
{
	int i;
	if (p->blocks != NULL)
		for (i = 0; i < CACHE_BLOCKS; i++)
			free(p->blocks[i].data);
	free(p->blocks);
	free(p->run);
	free(p->fill);
}

/* Cached transfer of an arbitrary byte range */
static s64 xfer(rufus_ntfs_dev_t* p, void* buf, s64 count, s64 pos, bool is_write)
{
	uint64_t upos = (uint64_t)pos, end, done = 0;

	if (pos < 0 || count < 0) {
		errno = EINVAL;
		return -1;
	}
	if (upos >= p->size || count == 0)
		return 0;
	if (upos + (uint64_t)count > p->size)
		count = (s64)(p->size - upos);
	end = upos + (uint64_t)count;
	if (is_write)
		p->writes_in++;
	else
		p->reads_in++;

	while (upos + done < end) {
		uint64_t cur = upos + done, idx = cur / BLOCK_SIZE, boff = cur % BLOCK_SIZE;
		uint64_t blen = block_len(p, idx), n = blen - boff, nunits = (blen + UNIT - 1) / UNIT;
		uint64_t ufirst, ulast;
		cblk_t* b;
		if (n > end - cur)
			n = end - cur;
		ufirst = boff / UNIT;
		ulast = (boff + n - 1) / UNIT;
		b = get_block(p, idx);
		if (b == NULL)
			return (done > 0) ? (s64)done : -1;
		if (is_write) {
			/* Partially written units must hold real data around the new bytes */
			bool head_partial = (boff % UNIT) != 0;
			bool tail_partial = ((boff + n) % UNIT) != 0 && (boff + n) != blen;
			if (((head_partial && !units_valid(b, ufirst, ufirst)) || (tail_partial && !units_valid(b, ulast, ulast))) &&
				!complete_block(p, b))
				return (done > 0) ? (s64)done : -1;
			memcpy(b->data + boff, (uint8_t*)buf + done, (size_t)n);
			b->dirty = true;
			set_units_valid(b, ufirst, ulast, nunits);
		} else {
			if (!units_valid(b, ufirst, ulast) && !complete_block(p, b))
				return (done > 0) ? (s64)done : -1;
			memcpy((uint8_t*)buf + done, b->data + boff, (size_t)n);
		}
		done += n;
	}
	return count;
}

/* ------------------------------------------------------------------------ */
/* ntfs-3g device operations                                                 */
/* ------------------------------------------------------------------------ */
static int rdev_open(struct ntfs_device* dev, int flags)
{
	rufus_ntfs_dev_t* p;
	if (NDevOpen(dev)) {
		errno = EBUSY;
		return -1;
	}
	p = calloc(1, sizeof(*p));
	if (p == NULL) {
		errno = ENOMEM;
		return -1;
	}
	if (!rufus_ntfs_parse_name(dev->d_name, &p->fd, &p->offset, &p->size, &p->sector_size) ||
		p->sector_size < 512 || (BLOCK_SIZE % p->sector_size) != 0) {
		free(p);
		errno = EINVAL;
		return -1;
	}
	if (!cache_init(p)) {
		cache_free(p);
		free(p);
		errno = ENOMEM;
		return -1;
	}
	dev->d_private = p;
	NDevSetOpen(dev);
	NDevSetBlock(dev);
	if ((flags & O_RDWR) != O_RDWR)
		NDevSetReadOnly(dev);
	return 0;
}

static int rdev_close(struct ntfs_device* dev)
{
	rufus_ntfs_dev_t* p = dev->d_private;
	int r = cache_flush(p);
	if (r == 0)
		fsync(p->fd);
	if (trace_fp != NULL)
		fflush(trace_fp);
	if (getenv("RUFUS_IO_STATS") != NULL)
		fprintf(stderr, "ntfs_io: %llu reads -> %llu device reads, %llu writes -> %llu device writes (avg %llu KB)\n",
			(unsigned long long)p->reads_in, (unsigned long long)p->reads_out,
			(unsigned long long)p->writes_in, (unsigned long long)p->writes_out,
			(unsigned long long)(p->writes_out ? p->bytes_out / p->writes_out / 1024 : 0));
	/* The fd belongs to the caller: don't close it */
	cache_free(p);
	free(p);
	dev->d_private = NULL;
	NDevClearOpen(dev);
	return r;
}

static s64 rdev_seek(struct ntfs_device* dev, s64 offset, int whence)
{
	rufus_ntfs_dev_t* p = dev->d_private;
	switch (whence) {
	case SEEK_SET: p->pos = offset; break;
	case SEEK_CUR: p->pos += offset; break;
	case SEEK_END: p->pos = (s64)p->size + offset; break;
	default: errno = EINVAL; return -1;
	}
	return p->pos;
}

static s64 rdev_pread(struct ntfs_device* dev, void* buf, s64 count, s64 offset)
{
	return xfer(dev->d_private, buf, count, offset, false);
}

static s64 rdev_pwrite(struct ntfs_device* dev, const void* buf, s64 count, s64 offset)
{
	if (NDevReadOnly(dev)) {
		errno = EROFS;
		return -1;
	}
	NDevSetDirty(dev);
	return xfer(dev->d_private, (void*)buf, count, offset, true);
}

static s64 rdev_read(struct ntfs_device* dev, void* buf, s64 count)
{
	rufus_ntfs_dev_t* p = dev->d_private;
	s64 r = rdev_pread(dev, buf, count, p->pos);
	if (r > 0)
		p->pos += r;
	return r;
}

static s64 rdev_write(struct ntfs_device* dev, const void* buf, s64 count)
{
	rufus_ntfs_dev_t* p = dev->d_private;
	s64 r = rdev_pwrite(dev, buf, count, p->pos);
	if (r > 0)
		p->pos += r;
	return r;
}

static int rdev_sync(struct ntfs_device* dev)
{
	rufus_ntfs_dev_t* p = dev->d_private;
	if (cache_flush(p) != 0)
		return -1;
	NDevClearDirty(dev);
	return (fsync(p->fd) == 0 || errno == ENOTSUP || errno == EINVAL) ? 0 : -1;
}

static int rdev_stat(struct ntfs_device* dev, struct stat* buf)
{
	rufus_ntfs_dev_t* p = dev->d_private;
	memset(buf, 0, sizeof(*buf));
	/* Present ourselves as a block device, so that mkntfs doesn't complain */
	buf->st_mode = S_IFBLK | 0600;
	buf->st_size = (off_t)p->size;
	buf->st_blksize = p->sector_size;
	return 0;
}

static int rdev_ioctl(struct ntfs_device* dev, unsigned long request, void* argp)
{
	rufus_ntfs_dev_t* p = dev->d_private;
	switch (request) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t*)argp = p->sector_size;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t*)argp = p->size / p->sector_size;
		return 0;
	default:
		errno = ENOTTY;
		return -1;
	}
}

struct ntfs_device_operations rufus_ntfs_io_ops = {
	.open = rdev_open,
	.close = rdev_close,
	.seek = rdev_seek,
	.read = rdev_read,
	.write = rdev_write,
	.pread = rdev_pread,
	.pwrite = rdev_pwrite,
	.sync = rdev_sync,
	.stat = rdev_stat,
	.ioctl = rdev_ioctl,
};
