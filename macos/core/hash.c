/*
 * Rufus for macOS: image checksums
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Windows Rufus has its own hash implementations (src/hash.c); on macOS,
 * CommonCrypto provides fast versions of all four.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <CommonCrypto/CommonDigest.h>

#include "rufus_core.h"

static void to_hex(const unsigned char* d, size_t n, char* out)
{
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = hex[d[i] >> 4];
		out[2 * i + 1] = hex[d[i] & 15];
	}
	out[2 * n] = 0;
}

bool compute_hashes(const char* path, hashes_t* h, bool sha512)
{
	CC_MD5_CTX md5;
	CC_SHA1_CTX sha1;
	CC_SHA256_CTX sha256;
	CC_SHA512_CTX sha512c;
	unsigned char d[CC_SHA512_DIGEST_LENGTH];
	const size_t bufsize = 4 * MB;
	uint8_t* buf = malloc(bufsize);
	struct stat st;
	uint64_t done = 0;
	int fd = open(path, O_RDONLY);
	bool r = false;

	memset(h, 0, sizeof(*h));
	if (fd < 0 || buf == NULL || fstat(fd, &st) != 0)
		goto out;
	update_status("Computing image checksums...");
	CC_MD5_Init(&md5);
	CC_SHA1_Init(&sha1);
	CC_SHA256_Init(&sha256);
	CC_SHA512_Init(&sha512c);
	for (;;) {
		ssize_t n = read(fd, buf, bufsize);
		if (n < 0)
			goto out;
		if (n == 0)
			break;
		if (cancel_requested)
			goto out;
		CC_MD5_Update(&md5, buf, (CC_LONG)n);
		CC_SHA1_Update(&sha1, buf, (CC_LONG)n);
		CC_SHA256_Update(&sha256, buf, (CC_LONG)n);
		if (sha512)
			CC_SHA512_Update(&sha512c, buf, (CC_LONG)n);
		done += (uint64_t)n;
		update_progress_bytes(NULL, done, (uint64_t)st.st_size);
	}
	CC_MD5_Final(d, &md5);       to_hex(d, CC_MD5_DIGEST_LENGTH, h->md5);
	CC_SHA1_Final(d, &sha1);     to_hex(d, CC_SHA1_DIGEST_LENGTH, h->sha1);
	CC_SHA256_Final(d, &sha256); to_hex(d, CC_SHA256_DIGEST_LENGTH, h->sha256);
	if (sha512) {
		CC_SHA512_Final(d, &sha512c);
		to_hex(d, CC_SHA512_DIGEST_LENGTH, h->sha512);
	}
	uprintf("MD5:    %s", h->md5);
	uprintf("SHA1:   %s", h->sha1);
	uprintf("SHA256: %s", h->sha256);
	if (sha512)
		uprintf("SHA512: %s", h->sha512);
	r = true;
out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return r;
}
