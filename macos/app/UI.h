/*
 * Rufus for macOS: shared UI helpers
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

/* A view with a top-left origin, which makes Rufus' dialog layout easier to port */
@interface FlippedView : NSView
@end

/* Rufus' progress bar, with the status text drawn inside it */
@interface RufusProgress : NSView
@property (nonatomic, copy) NSString* text;
@property (nonatomic) double value;          /* 0-100, < 0 = indeterminate (marquee) */
@property (nonatomic) BOOL success;
@property (nonatomic) BOOL error;
@end

BOOL UIHasLiquidGlass(void);
/* Tinted glass windows (macOS 26+) */
BOOL UIGlassWindowsEnabled(void);
void UIGlassifyWindow(NSWindow* w);

/* An NSAlert that gets the same tinted glass treatment as the other windows */
@interface GlassAlert : NSAlert
@end
NSImage* UISymbol(NSString* name, NSString* fallback_text);
NSTextField* UILabel(NSString* text);
NSTextField* UIHeader(NSString* text);
NSButton* UICheckbox(NSString* title, id target, SEL action);
NSButton* UIIconButton(NSString* symbol, NSString* fallback, NSString* tooltip, id target, SEL action);
NSPopUpButton* UIPopup(id target, SEL action);
/* Wrap a view in Liquid Glass (macOS 26+) or a visual effect view (older) */
NSView* UIGlassContainer(NSView* content, CGFloat cornerRadius);
void UIApplyGlassBezel(NSButton* button);

/* Log window */
@interface LogWindow : NSWindowController
+ (instancetype)shared;
- (void)append:(NSString*)line;
- (void)toggle;
- (NSString*)text;
@end

/* Log file used for debugging (~/Library/Logs/Rufus/rufus.log) */
void LogFileOpen(BOOL persistent);
void LogFileWrite(const char* msg);
NSString* LogFilePath(void);
