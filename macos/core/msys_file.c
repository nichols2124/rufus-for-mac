/*
 * Rufus for macOS: ms-sys data access, replacing src/ms-sys/file.c
 * Copyright © 2009 Henrik Carlqvist
 * Copyright © 2011-2019 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ms-sys hands us a FILE* that is really a FAKE_FD. On Windows, _handle is a
 * disk HANDLE; in the macOS port it is an rdev_t* and _offset is the start of
 * the partition (or 0 for the whole disk).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "rufus_core.h"
#include "file.h"


int64_t write_sectors(void* hDrive, uint64_t SectorSize, uint64_t StartSector, uint64_t nSectors, const void* pBuf)
{
	rdev_t* dev = (rdev_t*)hDrive;
	return rdev_write(dev, StartSector * SectorSize, pBuf, (size_t)(nSectors * SectorSize)) ?
		(int64_t)(nSectors * SectorSize) : -1;
}

int64_t read_sectors(void* hDrive, uint64_t SectorSize, uint64_t StartSector, uint64_t nSectors, void* pBuf)
{
	rdev_t* dev = (rdev_t*)hDrive;
	return rdev_read(dev, StartSector * SectorSize, pBuf, (size_t)(nSectors * SectorSize)) ?
		(int64_t)(nSectors * SectorSize) : -1;
}

int read_data(FILE* fp, uint64_t Position, void* pData, uint64_t Len)
{
	FAKE_FD* fd = (FAKE_FD*)fp;
	if (Len > MAX_DATA_LEN)
		return 0;
	/* rdev_read() already handles sector alignment */
	return rdev_read((rdev_t*)fd->_handle, fd->_offset + Position, pData, (size_t)Len) ? 1 : 0;
}

int write_data(FILE* fp, uint64_t Position, const void* pData, uint64_t Len)
{
	FAKE_FD* fd = (FAKE_FD*)fp;
	if (Len > MAX_DATA_LEN)
		return 0;
	return rdev_write((rdev_t*)fd->_handle, fd->_offset + Position, pData, (size_t)Len) ? 1 : 0;
}

int contains_data(FILE* fp, uint64_t Position, const void* pData, uint64_t Len)
{
	uint8_t buf[MAX_DATA_LEN];
	if (Len > sizeof(buf) || !read_data(fp, Position, buf, Len))
		return 0;
	return memcmp(pData, buf, (size_t)Len) == 0;
}
