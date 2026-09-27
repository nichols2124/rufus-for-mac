/*
 * Rufus for macOS: drive enumeration and exclusive access
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Foundation/Foundation.h>

@interface RufusDrive : NSObject
@property (copy) NSString* bsdName;       /* "disk4" */
@property (copy) NSString* label;         /* first volume's name, if any */
@property (copy) NSString* vendor;
@property (copy) NSString* model;
@property (copy) NSString* protocol;      /* "USB", "Disk Image", "Secure Digital"... */
@property (copy) NSString* partitionScheme;
@property (copy) NSString* fsName;        /* file system of the first volume */
@property (assign) uint64_t size;
@property (assign) uint32_t blockSize;
@property (assign) BOOL removable;
@property (assign) BOOL internal;
@property (assign) BOOL isOptical;
@property (assign) int partitionCount;
- (NSString*)displayName;                 /* "LABEL (disk4) [32 GB]" */
@end

typedef void (^DrivesChangedBlock)(void);

@interface Drives : NSObject
+ (instancetype)shared;
@property (copy) DrivesChangedBlock onChange;
@property (readonly) NSArray<RufusDrive*>* drives;
- (void)refresh;
/* Unmount all volumes and prevent macOS from mounting new ones until -releaseDrive */
- (BOOL)lockDrive:(RufusDrive*)drive error:(NSString**)error;
- (void)releaseDrive:(RufusDrive*)drive;
/* Open /dev/rdiskN for read/write, asking for administrator credentials if needed */
- (int)openRawDrive:(RufusDrive*)drive readOnly:(BOOL)ro error:(NSString**)error;
/* Mount the volumes back (after a write, macOS reprobes the partition table) */
- (void)remountDrive:(RufusDrive*)drive;
- (RufusDrive*)driveForBSDName:(NSString*)bsd;
@end

NSString* SizeToHuman(uint64_t size, BOOL copy_to_log, BOOL fake_units);
