/*
 * Rufus for macOS: main window
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

@interface MainWindow : NSWindowController <NSWindowDelegate, NSTextFieldDelegate>
- (void)openImageAtPath:(NSString*)path;
- (void)relocalize;
- (BOOL)isBusy;
- (void)setProgress:(double)pct text:(NSString*)text;
- (void)setStatusText:(NSString*)text;
@end
