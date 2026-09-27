/*
 * Rufus for macOS: libntfs-3g device driver for a partition inside a raw fd
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FLOOR(x, a)  (((x) / (a)) * (a))
#define CEIL(x, a)   ((((x) + (a) - 1) / (a)) * (a))

struct ntfs_device_operations;
extern struct ntfs_device_operations rufus_ntfs_io_ops;

bool rufus_ntfs_parse_name(const char* name, int* fd, uint64_t* offset, uint64_t* size, uint32_t* ss);
