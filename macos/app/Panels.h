/*
 * Rufus for macOS: secondary dialogs
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>
#include "rufus_core.h"

@interface HashPanel : NSObject
+ (void)showHashes:(hashes_t)h sha512:(BOOL)sha512 forWindow:(NSWindow*)parent;
@end

@interface WUEPanel : NSObject
/* Returns NSModalResponseOK or NSModalResponseCancel; fills *options */
+ (NSModalResponse)runForBuild:(uint32_t)build imagePath:(NSString*)path report:(image_report_t*)r options:(wue_options_t*)options;
@end

@interface AboutPanel : NSObject
+ (void)showForWindow:(NSWindow*)parent;
@end

@interface SettingsPanel : NSObject
+ (void)showForWindow:(NSWindow*)parent;
+ (void)checkNow:(NSWindow*)parent;
+ (void)checkIfDue:(NSWindow*)parent;      /* automatic check, per the update frequency setting */
@end

/* Windows / UEFI Shell ISO download (a native port of Fido.ps1) */
@interface FidoPanel : NSObject
+ (void)showForWindow:(NSWindow*)parent completion:(void (^)(NSString* downloadedPath))completion;
@end
