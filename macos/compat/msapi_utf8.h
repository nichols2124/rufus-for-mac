/*
 * Rufus for macOS: UTF-8 file API shims
 *
 * On Windows, msapi_utf8.h wraps the wide-char Win32 API so Rufus can use
 * UTF-8 everywhere. macOS is natively UTF-8, so these are thin POSIX calls.
 */
#pragma once

#include "windows.h"
#include <fnmatch.h>
#include <sys/stat.h>

#define _openU(path, flags, mode)   open(path, flags, mode)
#define _mkdirU(path)               mkdir_p(path)
#define fopenU                      fopen
#define _statU                      stat
#define _unlinkU                    unlink

/* mkdir -p */
static inline int mkdir_p(const char* path) {
	char tmp[1024];
	size_t len;
	if (path == NULL || (len = strlen(path)) == 0 || len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);
	for (char* p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

static inline int SHCreateDirectoryExU(HWND hwnd, const char* path, void* sa) {
	(void)hwnd; (void)sa;
	return mkdir_p(path);
}

static inline BOOL PathMatchSpecA(const char* str, const char* pattern) {
	return fnmatch(pattern, str, 0) == 0;
}
