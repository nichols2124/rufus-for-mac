/*
 * Rufus for macOS: core engine (platform independent part of the port)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The engine does all of its work on a single file descriptor: either a raw
 * disk (/dev/rdiskN, opened through authopen) or a plain image file. Every
 * partition is addressed by offset, so nothing ever needs to be mounted while
 * the drive is being written, and the whole pipeline can be tested on files.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KB  1024ULL
#define MB  (1024ULL * KB)
#define GB  (1024ULL * MB)
#define TB  (1024ULL * GB)

#define CEILING_ALIGN(x, a)  ((((x) + (a) - 1) / (a)) * (a))
#define FLOOR_ALIGN(x, a)    (((x) / (a)) * (a))

#define RUFUS_MAC_VERSION    "4.15"

/* Same enumerations and ordering as the Windows version */
typedef enum { FS_FAT16 = 0, FS_FAT32, FS_NTFS, FS_UDF, FS_EXFAT, FS_REFS, FS_EXT2, FS_EXT3, FS_EXT4, FS_MAX } fs_type_t;
typedef enum { PS_MBR = 0, PS_GPT, PS_SFD } part_style_t;
typedef enum { TT_BIOS = 0, TT_UEFI } target_t;
typedef enum { BT_NON_BOOTABLE = 0, BT_IMAGE, BT_FREEDOS, BT_UEFI_NTFS, BT_SYSLINUX_V6, BT_MAX } boot_type_t;
typedef enum { IMOP_STANDARD = 0, IMOP_WINTOGO } image_option_t;

#define IS_FAT(fs)  ((fs) == FS_FAT16 || (fs) == FS_FAT32)
#define SL_MAJOR_OF(x) ((uint8_t)((x) >> 8))
#define IS_EXT(fs)  ((fs) >= FS_EXT2 && (fs) <= FS_EXT4)

extern const char* fs_name[FS_MAX];

/* ------------------------------------------------------------------------ */
/* Logging, progress & cancellation                                          */
/* ------------------------------------------------------------------------ */
typedef void (*log_handler_t)(const char* msg);
typedef void (*progress_handler_t)(const char* status, double percent);  /* percent < 0 = indeterminate */

void uprintf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void set_log_handler(log_handler_t handler);
void set_progress_handler(progress_handler_t handler);
void update_status(const char* format, ...) __attribute__((format(printf, 1, 2)));
void update_progress(double percent);
void update_progress_bytes(const char* label, uint64_t done, uint64_t total);
const char* size_to_human(uint64_t size, bool round);
const char* strerror_last(void);

extern volatile bool cancel_requested;
#define CHECK_CANCEL(ret) do { if (cancel_requested) { uprintf("Operation cancelled by the user"); return ret; } } while (0)

/* ------------------------------------------------------------------------ */
/* Runtime toggles, mostly set through Rufus' "cheat mode" keyboard shortcuts */
/* ------------------------------------------------------------------------ */
typedef struct {
	bool enable_iso;               /* Alt-I */
	bool enable_joliet;            /* Alt-J */
	bool enable_rockridge;         /* Alt-K */
	bool preserve_timestamps;      /* Alt-T */
	bool use_rufus_mbr;            /* Alt-A */
	bool allow_dual_uefi_bios;     /* Alt-E */
	bool enable_ntfs_compression;  /* Alt-N */
	bool enable_file_indexing;     /* Alt-Q */
	bool detect_fakes;             /* Alt-B */
	bool ignore_boot_marker;       /* Alt-M */
	bool size_check;               /* Alt-S (true = enforce image size limits) */
	bool use_proper_size_units;    /* Alt-U */
	bool esp_as_basic_data;        /* Alt-P */
	bool usb_debug;                /* Alt-. */
	bool lock_drive;               /* Alt-, (true = exclusive access) */
	bool enable_sha512;            /* Alt-H */
	bool force_large_fat32;        /* Alt-L */
	bool list_usb_hdd;             /* Alt-F / "List USB Hard Drives" */
	bool list_virtual_disks;       /* Alt-G */
	bool list_vmware_disks;        /* Alt-W */
	bool list_non_usb_removable;   /* Ctrl-Alt-F */
	bool expert_mode;              /* Ctrl-Alt-E */
	int  priority_boost;           /* Alt-+ / Alt-- */
} rufus_flags_t;
extern rufus_flags_t rflags;

/* ------------------------------------------------------------------------ */
/* Raw target device (whole disk or image file)                              */
/* ------------------------------------------------------------------------ */
typedef struct {
	int fd;
	uint64_t size;           /* size in bytes */
	uint32_t sector_size;    /* logical sector size */
	bool is_file;            /* regular file rather than a device */
	char path[256];
} rdev_t;

bool rdev_open_file(rdev_t* dev, const char* path, bool create, uint64_t create_size);
bool rdev_attach_fd(rdev_t* dev, int fd, const char* path);
void rdev_close(rdev_t* dev);
bool rdev_write(rdev_t* dev, uint64_t offset, const void* buf, size_t len);
bool rdev_read(rdev_t* dev, uint64_t offset, void* buf, size_t len);
bool rdev_zero(rdev_t* dev, uint64_t offset, uint64_t len);
bool rdev_sync(rdev_t* dev);

/* ------------------------------------------------------------------------ */
/* Image analysis (mirrors RUFUS_IMG_REPORT)                                 */
/* ------------------------------------------------------------------------ */
#define MAX_WININST 4
enum { IMG_COMPRESSION_NONE = 0, IMG_COMPRESSION_ZIP, IMG_COMPRESSION_LZW, IMG_COMPRESSION_GZIP,
       IMG_COMPRESSION_LZMA, IMG_COMPRESSION_BZIP2, IMG_COMPRESSION_XZ, IMG_COMPRESSION_7ZIP,
       IMG_COMPRESSION_VTSI, IMG_COMPRESSION_ZSTD };

typedef struct {
	char label[192];
	char cfg_path[128];              /* isolinux/syslinux config */
	char wininst_path[MAX_WININST][128];
	char efi_img_path[128];
	uint64_t image_size;             /* size of the image file */
	uint64_t projected_size;         /* size of extracted content */
	bool is_iso;
	bool is_udf;
	int8_t is_bootable_img;          /* >0: has an MBR/GPT and can be DD-written */
	bool disable_iso;                /* ISO mode known to be broken (e.g. Pop!_OS) */
	uint16_t has_efi;                /* bit0: bootmgr.efi, bit n+1: boot<arch>.efi */
	uint8_t has_4GB_file;            /* 0x10 << i: wininst_name[i] is >= 4 GB; 0x01: other */
	bool has_bootmgr;
	bool has_bootmgr_efi;
	bool has_grub4dos;
	uint8_t has_grub2;
	bool has_kolibrios;
	bool has_efi_syslinux;
	bool has_long_filename;
	bool has_symlinks;
	bool has_panther_unattend;
	bool has_autorun;                /* the image has its own /autorun.inf */
	bool uses_casper;
	bool needs_ntfs;
	uint16_t winpe;
	uint8_t wininst_index;
	uint16_t sl_version;             /* syslinux version (major << 8 | minor) */
	char sl_version_str[12];
	char grub2_version[64];
	uint8_t compression_type;
	uint32_t win_build;              /* Windows build number, when detectable */
} image_report_t;

#define HAS_SYSLINUX(r)     ((r)->sl_version != 0)
#define HAS_BOOTMGR_BIOS(r) ((r)->has_bootmgr)
#define HAS_BOOTMGR_EFI(r)  ((r)->has_bootmgr_efi)
#define HAS_BOOTMGR(r)      (HAS_BOOTMGR_BIOS(r) || HAS_BOOTMGR_EFI(r))
#define HAS_WININST(r)      ((r)->wininst_index != 0)
#define HAS_WINPE(r)        ((r)->winpe != 0)
#define HAS_WINDOWS(r)      (HAS_BOOTMGR(r) || HAS_WINPE(r))
#define HAS_GRUB(r)         ((r)->has_grub2 || (r)->has_grub4dos)
#define IS_EFI_BOOTABLE(r)  ((r)->has_efi != 0)
#define IS_BIOS_BOOTABLE(r) (HAS_BOOTMGR(r) || HAS_SYSLINUX(r) || HAS_WINPE(r) || HAS_GRUB(r) || (r)->has_kolibrios)
#define IS_DD_BOOTABLE(r)   ((r)->is_bootable_img > 0)
#define IS_DD_ONLY(r)       (((r)->is_bootable_img > 0) && (!(r)->is_iso || (r)->disable_iso))
#define IS_FAT32_COMPAT(r)  (((r)->has_4GB_file == 0) && !(r)->needs_ntfs)
#define HAS_WINTOGO(r)      (HAS_BOOTMGR(r) && IS_EFI_BOOTABLE(r) && HAS_WININST(r))
#define HAS_PERSISTENCE(r)  ((HAS_SYSLINUX(r) || HAS_GRUB(r)) && !(HAS_WINDOWS(r) || (r)->has_kolibrios))

bool image_scan(const char* path, image_report_t* report);
void image_report_log(const image_report_t* report, const char* path);

/* ------------------------------------------------------------------------ */
/* Partitioning                                                              */
/* ------------------------------------------------------------------------ */
#define MAX_PARTITIONS 8
#define XP_MSR          0x01
#define XP_ESP          0x02
#define XP_UEFI_NTFS    0x04
#define XP_COMPAT       0x08
#define XP_PERSISTENCE  0x10

typedef struct {
	char name[40];
	uint64_t offset;
	uint64_t size;
	uint8_t mbr_type;
	const uint8_t* gpt_type;          /* 16-byte on-disk GUID */
	uint64_t gpt_attributes;
	bool active;
} partition_t;

typedef struct {
	part_style_t style;
	int count;
	int main_index;
	int uefi_ntfs_index;
	int esp_index;
	int persistence_index;
	uint32_t disk_signature;
	partition_t part[MAX_PARTITIONS];
} layout_t;

bool compute_layout(rdev_t* dev, layout_t* layout, part_style_t style, fs_type_t fs,
	boot_type_t bt, uint32_t cluster_size, uint8_t extra_partitions, bool old_bios_fixes,
	size_t uefi_ntfs_size, bool mbr_uefi_marker, uint64_t persistence_size);
bool clear_mbr_gpt(rdev_t* dev);
bool write_partition_table(rdev_t* dev, const layout_t* layout);
bool gpt_toggle_esp(rdev_t* dev);   /* Alt-P */

/* ------------------------------------------------------------------------ */
/* File systems                                                              */
/* ------------------------------------------------------------------------ */
/* A partition view onto the raw device */
typedef struct {
	rdev_t* dev;
	uint64_t offset;
	uint64_t size;
} part_view_t;

uint32_t default_cluster_size(fs_type_t fs, uint64_t part_size);
bool format_fat(part_view_t* pv, fs_type_t fs, uint32_t cluster_size, const char* label, bool quick);
bool format_ntfs(part_view_t* pv, uint32_t cluster_size, const char* label, bool quick);
void kill_helpers(void);   /* stop helper processes (mkntfs) when cancelling */
bool format_ext(part_view_t* pv, fs_type_t fs, uint32_t cluster_size, const char* label);
bool format_ext_ex(part_view_t* pv, fs_type_t fs, uint32_t block_size, const char* label, bool quick, bool persistence_conf);
void to_valid_label(char* label, bool fat);

/* Where the bundled helper executables live (set by the app / CLI) */
extern char helper_dir[1024];

/* Generic "file sink" used by the ISO extractor, implemented for FAT & NTFS */
typedef struct sink sink_t;
struct sink {
	void* priv;
	bool (*mkdir)(sink_t* s, const char* path);
	void* (*create)(sink_t* s, const char* path, uint64_t size);
	bool (*write)(sink_t* s, void* file, const void* buf, size_t len);
	bool (*close)(sink_t* s, void* file);
	bool (*set_label)(sink_t* s, const char* label);
	bool (*unmount)(sink_t* s);
	uint64_t max_file_size;
};

sink_t* fat_sink_open(part_view_t* pv);
sink_t* ntfs_sink_open(part_view_t* pv);
sink_t* ext_sink_open(part_view_t* pv);
bool sink_write_buffer(sink_t* s, const char* path, const void* buf, size_t len);
bool sink_copy_archive(sink_t* s, const char* archive_path);

/* ------------------------------------------------------------------------ */
/* ISO extraction & bootloaders                                              */
/* ------------------------------------------------------------------------ */
bool extract_iso(const char* iso_path, const image_report_t* report, sink_t* sink, fs_type_t fs, bool persistence);
bool extract_iso_file(const char* iso_path, const char* src, void** buf, size_t* size);
bool extract_iso_uses_device_progress(void);
void extract_iso_progress_done(void);
bool write_mbr(rdev_t* dev, const layout_t* layout, boot_type_t bt, const image_report_t* report,
	fs_type_t fs, target_t tt, bool use_rufus_mbr);
bool write_pbr(part_view_t* pv, fs_type_t fs, boot_type_t bt, const image_report_t* report);
bool install_syslinux(part_view_t* pv, sink_t* sink, const image_report_t* report);
bool install_freedos(sink_t* sink);
bool write_dd_image(rdev_t* dev, const char* image_path, const image_report_t* report);

/* Resources (embedded boot files, loaded from the app bundle) */
extern char resource_dir[1024];
void* load_resource(const char* name, size_t* size);

/* ------------------------------------------------------------------------ */
/* Bad blocks                                                                */
/* ------------------------------------------------------------------------ */
bool check_bad_blocks(rdev_t* dev, int passes, uint64_t* bad_count);

/* ------------------------------------------------------------------------ */
/* The main job (equivalent of Rufus' FormatThread)                          */
/* ------------------------------------------------------------------------ */
typedef struct {
	boot_type_t boot_type;
	const char* image_path;
	image_report_t* report;
	image_option_t image_option;
	bool write_as_image;          /* DD mode */
	part_style_t part_style;
	target_t target;
	fs_type_t fs;
	uint32_t cluster_size;        /* 0 = default */
	char label[64];
	bool quick_format;
	bool extended_label;
	int bad_block_passes;         /* 0 = no check */
	bool old_bios_fixes;
	bool use_rufus_mbr;
	int zero_drive;               /* 1 = zero drive (Alt-Z), 2 = zero, skipping empty blocks (Ctrl-Alt-Z) */
	uint64_t persistence_size;    /* 0 = no persistence partition */
	const struct wue_options_s* wue;
	const char* archive_path;     /* Ctrl-SELECT: extra archive to copy to the drive */
} job_options_t;

int run_job(rdev_t* dev, const job_options_t* opts);
bool zero_drive(rdev_t* dev, bool skip_empty);	/* Alt-Z / Ctrl-Alt-Z */

/* ------------------------------------------------------------------------ */
/* Windows User Experience (WUE) - port of src/wue.c                        */
/* ------------------------------------------------------------------------ */
#define UNATTEND_SECUREBOOT_TPM_MINRAM      0x00001
#define UNATTEND_NO_ONLINE_ACCOUNT          0x00004
#define UNATTEND_NO_DATA_COLLECTION         0x00008
#define UNATTEND_OFFLINE_INTERNAL_DRIVES    0x00010
#define UNATTEND_DUPLICATE_LOCALE           0x00020
#define UNATTEND_SET_USER                   0x00040
#define UNATTEND_DISABLE_BITLOCKER          0x00080
#define UNATTEND_FORCE_S_MODE               0x00100
#define UNATTEND_USE_MS2023_BOOTLOADERS     0x00200
#define UNATTEND_APPLY_SKUSIPOLICY          0x00400
#define UNATTEND_SILENT_INSTALL             0x00800
#define UNATTEND_QOL_ENHANCEMENTS           0x01000
#define UNATTEND_WINPE_SETUP_MASK           (UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_SILENT_INSTALL)
#define UNATTEND_SPECIALIZE_DEPLOYMENT_MASK (UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OOBE_SHELL_SETUP_MASK      (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE | UNATTEND_SILENT_INSTALL)
#define UNATTEND_OOBE_INTERNATIONAL_MASK    (UNATTEND_DUPLICATE_LOCALE)
#define UNATTEND_OOBE_MASK                  (UNATTEND_OOBE_SHELL_SETUP_MASK | UNATTEND_OOBE_INTERNATIONAL_MASK | UNATTEND_DISABLE_BITLOCKER | \
                                             UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OFFLINE_SERVICING_MASK     (UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_FORCE_S_MODE)
#define UNATTEND_DEFAULT_SELECTION_MASK     (UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_OFFLINE_INTERNAL_DRIVES)

typedef struct wue_options_s {
	int flags;
	char username[64];
	char locale[32];         /* e.g. "en-US", from the Mac's settings */
	char input_locale[32];
	char timezone[64];       /* Windows time zone name, or empty */
	int edition_index;       /* for silent install */
} wue_options_t;

char* create_unattend_xml(const image_report_t* r, const wue_options_t* w);
bool apply_windows_customization(sink_t* sink, const char* iso_path, const image_report_t* r, const wue_options_t* w);
uint32_t get_windows_build(const char* iso_path, const image_report_t* r, char* version, size_t version_size);
/* Returns the number of editions (DISPLAYNAME per IMAGE INDEX) in the install image */
int get_windows_editions(const char* iso_path, const image_report_t* r, char names[][128], int max);

/* ------------------------------------------------------------------------ */
/* Checksums (MD5, SHA-1, SHA-256, SHA-512)                                 */
/* ------------------------------------------------------------------------ */
typedef struct {
	char md5[33], sha1[41], sha256[65], sha512[129];
} hashes_t;
bool compute_hashes(const char* path, hashes_t* h, bool sha512);

#ifdef __cplusplus
}
#endif
