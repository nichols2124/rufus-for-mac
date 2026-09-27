/*
 * Rufus for macOS: minimal Win32 compatibility layer
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This file is NOT an emulation of Windows. It only provides the handful of
 * types and calls that Rufus' bundled portable libraries (bled, libcdio,
 * ms-sys, syslinux, ext2fs) reference, mapped onto POSIX. A HANDLE is simply
 * a POSIX file descriptor stored in a pointer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#ifndef __APPLE__
#error This compatibility header is only meant for macOS
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <wchar.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <alloca.h>

/* Tell headers such as bled's libbb.h that POSIX already provides these */
#define _MODE_T_
#define _PID_T_
#define _GID_T_
#define _UID_T_

#define WINAPI
#define CALLBACK
#define __stdcall
#define __cdecl
#define APIENTRY
#define IN
#define OUT
#define OPTIONAL
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif

typedef int                 BOOL;
typedef unsigned char       BOOLEAN;
typedef uint8_t             BYTE, UCHAR, *PBYTE, *LPBYTE;
typedef uint16_t            WORD, USHORT;
typedef uint32_t            DWORD, UINT, ULONG, *PDWORD, *LPDWORD;
typedef int32_t             LONG, INT;
typedef int64_t             LONGLONG, LONG64, INT64;
typedef uint64_t            ULONGLONG, DWORD64, ULONG64, UINT64, QWORD;
typedef intptr_t            INT_PTR, LONG_PTR;
typedef uintptr_t           UINT_PTR, ULONG_PTR, DWORD_PTR, SIZE_T;
typedef char                CHAR, *LPSTR, *PSTR;
typedef const char          *LPCSTR, *PCSTR;
typedef wchar_t             WCHAR, *LPWSTR, *PWSTR;
typedef const wchar_t       *LPCWSTR, *PCWSTR;
typedef void                VOID, *PVOID, *LPVOID;
typedef const void          *LPCVOID;
typedef void                *HANDLE, *HWND, *HMODULE, *HINSTANCE;
typedef DWORD               ERROR_CODE;

typedef union {
	struct { DWORD LowPart; LONG HighPart; };
	struct { DWORD LowPart; LONG HighPart; } u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#define MAX_PATH                260
#define INVALID_HANDLE_VALUE    ((HANDLE)(intptr_t)-1)
#define FILE_BEGIN              SEEK_SET
#define FILE_CURRENT            SEEK_CUR
#define FILE_END                SEEK_END

/* HANDLE <-> fd */
#define fd_to_handle(fd)        ((HANDLE)(intptr_t)(fd))
#define handle_to_fd(h)         ((int)(intptr_t)(h))

#define ERROR_SUCCESS           0
#define ERROR_WRITE_FAULT       29
#define ERROR_READ_FAULT        30
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_PARAMETER 87
#define ERROR_CANCELLED         1223

#define LOWORD(l)   ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l)   ((WORD)((DWORD_PTR)(l) >> 16))
#define LOBYTE(w)   ((BYTE)((DWORD_PTR)(w) & 0xff))
#define HIBYTE(w)   ((BYTE)((DWORD_PTR)(w) >> 8))
#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A)/sizeof((A)[0]))
#endif
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

/* MSVC intrinsics */
#define _byteswap_ushort(x)  __builtin_bswap16(x)
#define _byteswap_ulong(x)   __builtin_bswap32(x)
#define _byteswap_uint64(x)  __builtin_bswap64(x)

/* MSVC CRT names */
#define _stricmp        strcasecmp
#define _strnicmp       strncasecmp
#define stricmp         strcasecmp
#define strnicmp        strncasecmp
#define _strdup         strdup
#define _snprintf       snprintf
#define _vsnprintf      vsnprintf
#define _fseeki64       fseeko
#define _ftelli64       ftello
#define _lseeki64       lseek
#define _open           open
#define _read           read
#define _write          write
#define _close          close
#define _dup2           dup2
#define _unlink         unlink
#define _chmod          chmod
#define _SH_DENYNO      0
#define _S_IFCHR        S_IFCHR
static inline int _sopen_s(int* fd, const char* path, int flags, int share, int mode) {
	(void)share;
	*fd = open(path, flags, mode);
	return (*fd < 0) ? errno : 0;
}
#define _alloca         alloca
#define _TRUNCATE       ((size_t)-1)
#define _snprintf_s(buf, size, count, ...)  snprintf(buf, size, __VA_ARGS__)
#define sprintf_s(buf, size, ...)           snprintf(buf, size, __VA_ARGS__)
#define strcpy_s(dst, size, src)            strlcpy(dst, src, size)
#define strcat_s(dst, size, src)            strlcat(dst, src, size)
#define _O_RDONLY       O_RDONLY
#define _O_WRONLY       O_WRONLY
#define _O_RDWR         O_RDWR
#define _O_CREAT        O_CREAT
#define _O_TRUNC        O_TRUNC
#define _O_EXCL         O_EXCL
#define _O_APPEND       O_APPEND
#define _O_BINARY       0
#define _O_TEXT         0
#define _S_IFMT         S_IFMT
#define _S_IFDIR        S_IFDIR
#define _S_IFREG        S_IFREG
#define _S_IREAD        S_IRUSR
#define _S_IWRITE       S_IWUSR
#define _open_osfhandle(h, flags)   ((int)(intptr_t)(h))
#define _get_osfhandle(fd)          ((intptr_t)(fd))
#define _pipe(fds, size, mode)      pipe(fds)

#include <mm_malloc.h>   /* clang provides _mm_malloc/_mm_free */
#define _aligned_malloc(size, align) _mm_malloc(size, align)
#define _aligned_free free

static inline int localtime_s(struct tm* result, const time_t* timep) {
	return (localtime_r(timep, result) == NULL) ? EINVAL : 0;
}

/* Minimal file API on top of POSIX file descriptors */
static inline BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* newpos, DWORD method) {
	off_t r = lseek(handle_to_fd(h), (off_t)dist.QuadPart, (int)method);
	if (r < 0)
		return FALSE;
	if (newpos != NULL)
		newpos->QuadPart = r;
	return TRUE;
}

static inline BOOL WriteFile(HANDLE h, const void* buf, DWORD size, DWORD* written, void* overlapped) {
	ssize_t r;
	(void)overlapped;
	do { r = write(handle_to_fd(h), buf, size); } while (r < 0 && errno == EINTR);
	if (written != NULL)
		*written = (r < 0) ? 0 : (DWORD)r;
	return (r >= 0);
}

static inline BOOL ReadFile(HANDLE h, void* buf, DWORD size, DWORD* read_size, void* overlapped) {
	ssize_t r;
	(void)overlapped;
	do { r = read(handle_to_fd(h), buf, size); } while (r < 0 && errno == EINTR);
	if (read_size != NULL)
		*read_size = (r < 0) ? 0 : (DWORD)r;
	return (r >= 0);
}

static inline BOOL CloseHandle(HANDLE h) { return close(handle_to_fd(h)) == 0; }
static inline void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }
static inline DWORD GetLastError(void) { return (DWORD)errno; }
static inline void SetLastError(DWORD e) { errno = (int)e; }
