/*
 * Rufus for macOS: shared UI helpers, progress bar and log window
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Liquid Glass (macOS 26) is only applied to the functional layer, i.e. the
 * floating action bar and its buttons, as recommended by Apple's guidelines.
 * On older versions of macOS, the same views fall back to the regular
 * vibrant materials, so that the app still runs on Catalina and later.
 */
#import "UI.h"
#import "Loc.h"
#include <stdio.h>
#include <pthread.h>

@implementation FlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Developer aid: `-ForceLegacyUI YES` takes the pre-macOS 26 code path, to preview the fallback */
BOOL UIHasLiquidGlass(void)
{
	if ([[NSUserDefaults standardUserDefaults] boolForKey:@"ForceLegacyUI"])
		return NO;
#if RUFUS_GLASS_SDK
	if (@available(macOS 26.0, *))
		return NSClassFromString(@"NSGlassEffectView") != nil;
#endif
	return NO;
}

/* Every window gets the tinted glass look wherever Liquid Glass is available */
BOOL UIGlassWindowsEnabled(void)
{
	return UIHasLiquidGlass();
}

/* Put the window's existing content on a single, accent tinted, glass pane. The
 * title bar becomes transparent and blends into the glass, while the content keeps
 * its original position below it. */
void UIGlassifyWindow(NSWindow* w)
{
	if (!UIGlassWindowsEnabled() || w == nil || [w.contentView isKindOfClass:NSClassFromString(@"NSGlassEffectView")])
		return;
#if RUFUS_GLASS_SDK
	if (@available(macOS 26.0, *)) {
		NSView* old = w.contentView;
		NSRect frame = w.frame;
		NSRect cr = [w contentRectForFrameRect:frame];
		CGFloat top = frame.size.height - cr.size.height;   /* title bar height, if any */
		w.styleMask |= NSWindowStyleMaskFullSizeContentView;
		w.titlebarAppearsTransparent = YES;
		w.opaque = NO;
		w.backgroundColor = NSColor.clearColor;
		NSView* holder = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
		holder.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
		w.contentView = holder;   /* detaches 'old' from the window */
		old.frame = NSMakeRect(0, 0, frame.size.width, frame.size.height - top);
		old.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
		[holder addSubview:old];
		NSGlassEffectView* g = [[NSGlassEffectView alloc] initWithFrame:holder.frame];
		g.style = NSGlassEffectViewStyleRegular;
		g.tintColor = [NSColor.controlAccentColor colorWithAlphaComponent:0.12];
		g.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
		g.contentView = holder;
		w.contentView = g;
	}
#endif
}

/* NSAlert uses Auto Layout and re-runs its layout when shown, so its views must never
 * be moved into another hierarchy (that throws, and crashed the app). Instead, a glass
 * pane is added *behind* the existing content, as a background. */
static void UIGlassBackdrop(NSWindow* w)
{
	if (!UIGlassWindowsEnabled() || w == nil)
		return;
#if RUFUS_GLASS_SDK
	if (@available(macOS 26.0, *)) {
		NSView* cv = w.contentView;
		for (NSView* v in cv.subviews)
			if ([v isKindOfClass:[NSGlassEffectView class]])
				return;   /* already done */
		w.opaque = NO;
		w.backgroundColor = NSColor.clearColor;
		NSGlassEffectView* g = [[NSGlassEffectView alloc] initWithFrame:cv.bounds];
		g.style = NSGlassEffectViewStyleRegular;
		g.tintColor = [NSColor.controlAccentColor colorWithAlphaComponent:0.12];
		g.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
		[cv addSubview:g positioned:NSWindowBelow relativeTo:nil];
	}
#endif
}

@implementation GlassAlert
- (NSModalResponse)runModal
{
	[self layout];
	UIGlassBackdrop(self.window);
	return [super runModal];
}

- (void)beginSheetModalForWindow:(NSWindow*)w completionHandler:(void (^)(NSModalResponse))handler
{
	[self layout];
	UIGlassBackdrop(self.window);
	[super beginSheetModalForWindow:w completionHandler:handler];
}
@end

NSImage* UISymbol(NSString* name, NSString* fallback_text)
{
	if (@available(macOS 11.0, *)) {
		NSImage* img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:fallback_text];
		if (img != nil)
			return img;
	}
	return nil;
}

NSTextField* UILabel(NSString* text)
{
	NSTextField* l = [NSTextField labelWithString:text ?: @""];
	l.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
	l.lineBreakMode = NSLineBreakByTruncatingTail;
	return l;
}

NSTextField* UIHeader(NSString* text)
{
	NSTextField* l = [NSTextField labelWithString:text ?: @""];
	l.font = [NSFont systemFontOfSize:NSFont.systemFontSize + 4 weight:NSFontWeightSemibold];
	return l;
}

NSButton* UICheckbox(NSString* title, id target, SEL action)
{
	NSButton* b = [NSButton checkboxWithTitle:title ?: @"" target:target action:action];
	b.lineBreakMode = NSLineBreakByTruncatingTail;
	return b;
}

void UIApplyGlassBezel(NSButton* button)
{
	button.bezelStyle = NSBezelStyleRounded;
#if RUFUS_GLASS_SDK
	if (@available(macOS 26.0, *)) {
		if (UIHasLiquidGlass())
			button.bezelStyle = NSBezelStyleGlass;
	}
#endif
}

NSButton* UIIconButton(NSString* symbol, NSString* fallback, NSString* tooltip, id target, SEL action)
{
	NSImage* img = UISymbol(symbol, tooltip);
	NSButton* b = (img != nil) ? [NSButton buttonWithImage:img target:target action:action] :
		[NSButton buttonWithTitle:fallback target:target action:action];
	UIApplyGlassBezel(b);
#if RUFUS_GLASS_SDK
	if (@available(macOS 26.0, *)) {
		if (UIHasLiquidGlass())
			b.borderShape = NSControlBorderShapeCircle;
	}
#endif
	b.toolTip = tooltip;
	b.imagePosition = NSImageOnly;
	return b;
}

NSPopUpButton* UIPopup(id target, SEL action)
{
	NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
	p.target = target;
	p.action = action;
	p.lineBreakMode = NSLineBreakByTruncatingTail;
	return p;
}

NSView* UIGlassContainer(NSView* content, CGFloat cornerRadius)
{
#if RUFUS_GLASS_SDK
	if (@available(macOS 26.0, *)) {
		if (UIHasLiquidGlass()) {
		NSGlassEffectView* glass = [[NSGlassEffectView alloc] initWithFrame:content.frame];
		glass.cornerRadius = cornerRadius;
		glass.contentView = content;
		return glass;
		}
	}
#endif
	NSVisualEffectView* v = [[NSVisualEffectView alloc] initWithFrame:content.frame];
	v.material = NSVisualEffectMaterialHeaderView;
	v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
	v.state = NSVisualEffectStateFollowsWindowActiveState;
	v.wantsLayer = YES;
	v.layer.cornerRadius = cornerRadius;
	v.layer.masksToBounds = YES;
	content.frame = v.bounds;
	content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	[v addSubview:content];
	return v;
}

/* ------------------------------------------------------------------------ */
/* Progress bar with centered status text                                    */
/* ------------------------------------------------------------------------ */
@implementation RufusProgress {
	NSTimer* marquee;
	CGFloat phase;
}

- (void)setValue:(double)value
{
	_value = value;
	if (value < 0 && marquee == nil) {
		marquee = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer* t) {
			self->phase += 0.012;
			if (self->phase > 1.3)
				self->phase = -0.3;
			self.needsDisplay = YES;
		}];
	} else if (value >= 0 && marquee != nil) {
		[marquee invalidate];
		marquee = nil;
	}
	self.needsDisplay = YES;
}

- (void)setText:(NSString*)text
{
	_text = [text copy];
	self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirty
{
	NSRect b = NSInsetRect(self.bounds, 0.5, 0.5);
	CGFloat r = b.size.height / 2;
	NSBezierPath* track = [NSBezierPath bezierPathWithRoundedRect:b xRadius:r yRadius:r];
	NSColor* fill = self.error ? NSColor.systemRedColor : self.success ? NSColor.systemGreenColor : NSColor.controlAccentColor;

	[[NSColor.labelColor colorWithAlphaComponent:0.08] setFill];
	[track fill];
	[NSGraphicsContext saveGraphicsState];
	[track addClip];
	if (self.value < 0) {
		NSRect m = NSMakeRect(b.origin.x + b.size.width * phase, b.origin.y, b.size.width * 0.3, b.size.height);
		[[fill colorWithAlphaComponent:0.75] setFill];
		[[NSBezierPath bezierPathWithRoundedRect:m xRadius:r yRadius:r] fill];
	} else if (self.value > 0) {
		NSRect f = b;
		f.size.width = b.size.width * MIN(self.value, 100.0) / 100.0;
		[[fill colorWithAlphaComponent:0.85] setFill];
		NSRectFill(f);
	}
	[NSGraphicsContext restoreGraphicsState];
	[[NSColor.separatorColor colorWithAlphaComponent:0.6] setStroke];
	[track stroke];

	if (self.text.length) {
		NSMutableParagraphStyle* ps = [NSMutableParagraphStyle new];
		ps.alignment = NSTextAlignmentCenter;
		ps.lineBreakMode = NSLineBreakByTruncatingTail;
		NSDictionary* attr = @{ NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightMedium],
			NSForegroundColorAttributeName: NSColor.labelColor, NSParagraphStyleAttributeName: ps };
		NSSize ts = [self.text sizeWithAttributes:attr];
		NSRect tr = NSMakeRect(b.origin.x + 8, NSMidY(b) - ts.height / 2, b.size.width - 16, ts.height);
		[self.text drawInRect:tr withAttributes:attr];
	}
}
@end

/* ------------------------------------------------------------------------ */
/* Log file                                                                  */
/* ------------------------------------------------------------------------ */
static FILE* log_fp = NULL;
static pthread_mutex_t log_fp_mutex = PTHREAD_MUTEX_INITIALIZER;

NSString* LogFilePath(void)
{
	return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Rufus/rufus.log"];
}

void LogFileOpen(BOOL persistent)
{
	NSString* path = LogFilePath();
	[[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent]
		withIntermediateDirectories:YES attributes:nil error:nil];
	pthread_mutex_lock(&log_fp_mutex);
	if (log_fp != NULL)
		fclose(log_fp);
	log_fp = fopen(path.fileSystemRepresentation, persistent ? "a" : "w");
	pthread_mutex_unlock(&log_fp_mutex);
}

void LogFileWrite(const char* msg)
{
	pthread_mutex_lock(&log_fp_mutex);
	if (log_fp != NULL) {
		fprintf(log_fp, "%s\n", msg);
		fflush(log_fp);
	}
	pthread_mutex_unlock(&log_fp_mutex);
}

/* ------------------------------------------------------------------------ */
/* Log window (Ctrl-L / ⌘L)                                                  */
/* ------------------------------------------------------------------------ */
@implementation LogWindow {
	NSTextView* textView;
	NSMutableArray<NSString*>* pending;
	BOOL flushScheduled;
}

+ (instancetype)shared
{
	static LogWindow* w;
	static dispatch_once_t once;
	dispatch_once(&once, ^{ w = [LogWindow new]; });
	return w;
}

- (instancetype)init
{
	NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 420)
		styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
		backing:NSBackingStoreBuffered defer:YES];
	if ((self = [super initWithWindow:win])) {
		pending = [NSMutableArray array];
		win.title = L(@"MSG_108");
		win.minSize = NSMakeSize(400, 240);
		win.releasedWhenClosed = NO;
		win.restorable = NO;

		NSScrollView* sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 52, 640, 368)];
		sv.hasVerticalScroller = YES;
		sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
		textView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, sv.contentSize.width, sv.contentSize.height)];
		textView.minSize = NSMakeSize(0, sv.contentSize.height);
		textView.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
		textView.verticallyResizable = YES;
		textView.horizontallyResizable = NO;
		textView.textContainer.containerSize = NSMakeSize(sv.contentSize.width, FLT_MAX);
		textView.textContainer.widthTracksTextView = YES;
		textView.editable = NO;
		/* The log is content, which Apple's guidelines keep off glass. It also changes many times
		 * a second during an operation, and text drawn over glass forces the glass behind it to be
		 * re-rendered every time: an opaque text area keeps that cheap. */
		textView.drawsBackground = YES;
		textView.backgroundColor = NSColor.textBackgroundColor;
		sv.drawsBackground = YES;
		textView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
		textView.autoresizingMask = NSViewWidthSizable;
		textView.textContainerInset = NSMakeSize(6, 6);
		sv.documentView = textView;
		[win.contentView addSubview:sv];

		NSButton* clear = [NSButton buttonWithTitle:L(@"IDC_LOG_CLEAR") target:self action:@selector(clear:)];
		NSButton* save = [NSButton buttonWithTitle:L(@"IDC_LOG_SAVE") target:self action:@selector(save:)];
		NSButton* close = [NSButton buttonWithTitle:L(@"IDCANCEL") target:win action:@selector(performClose:)];
		NSArray* buttons = @[ clear, save, close ];
		CGFloat x = 640 - 12;
		for (NSButton* b in [buttons reverseObjectEnumerator]) {
			UIApplyGlassBezel(b);
			[b sizeToFit];
			NSRect f = b.frame;
			f.size.width = MAX(f.size.width, 90);
			x -= f.size.width;
			f.origin = NSMakePoint(x, 12);
			b.frame = f;
			b.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
			x -= 8;
			[win.contentView addSubview:b];
		}
		[win center];
		UIGlassifyWindow(win);
	}
	return self;
}

- (void)append:(NSString*)line
{
	@synchronized (pending) {
		[pending addObject:line];
		if (flushScheduled)
			return;
		flushScheduled = YES;
	}
	dispatch_async(dispatch_get_main_queue(), ^{
		NSArray* lines;
		@synchronized (self->pending) {
			lines = [self->pending copy];
			[self->pending removeAllObjects];
			self->flushScheduled = NO;
		}
		NSString* chunk = [[lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"];
		NSDictionary* attr = @{ NSFontAttributeName: self->textView.font ?: [NSFont userFixedPitchFontOfSize:11],
			NSForegroundColorAttributeName: NSColor.textColor };
		BOOL at_end = NSMaxY(self->textView.visibleRect) >= NSMaxY(self->textView.bounds) - 20;
		[self->textView.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:chunk attributes:attr]];
		if (at_end)
			[self->textView scrollToEndOfDocument:nil];
	});
}

- (NSString*)text { return textView.string; }

- (void)toggle
{
	if (self.window.isVisible)
		[self.window orderOut:nil];
	else
		[self showWindow:nil];
}

- (void)clear:(id)sender
{
	textView.string = @"";
}

- (void)save:(id)sender
{
	NSSavePanel* p = [NSSavePanel savePanel];
	NSDateFormatter* df = [NSDateFormatter new];
	df.dateFormat = @"yyyyMMdd_HHmmss";
	p.nameFieldStringValue = [NSString stringWithFormat:@"rufus_%@.log", [df stringFromDate:[NSDate date]]];
	[p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
		if (r == NSModalResponseOK)
			[self->textView.string writeToURL:p.URL atomically:YES encoding:NSUTF8StringEncoding error:nil];
	}];
}
@end
