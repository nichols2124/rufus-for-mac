/*
 * Rufus for macOS: main window (port of the main dialog logic in src/rufus.c)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The layout reproduces IDD_DIALOG from src/rufus.rc: same sections, same
 * controls, same order. Controls are native AppKit ones (which get the
 * Liquid Glass look on macOS 26), while the bottom action bar floats on a
 * glass pane.
 */
#import "MainWindow.h"
#import "Drives.h"
#import "Loc.h"
#import "UI.h"
#import "Panels.h"
#include <sys/resource.h>
#include <pthread.h>
#include "rufus_core.h"

#define WIDTH       440.0
#define MARGIN      18.0
#define COL2_X      (WIDTH / 2 + 8)
#define COL_W       (WIDTH / 2 - MARGIN - 8)
#define SIDE_BTN    30.0

static MainWindow* main_window;

void cluster_sizes(fs_type_t fs, uint64_t disk_size, uint32_t sector_size, uint32_t* allowed, uint32_t* def);

/* ------------------------------------------------------------------------ */
/* Engine callbacks                                                          */
/* ------------------------------------------------------------------------ */
static void log_to_ui(const char* msg)
{
	LogFileWrite(msg);
	[[LogWindow shared] append:[NSString stringWithUTF8String:msg] ?: @"(invalid UTF-8)"];
}

static void progress_to_ui(const char* status, double pct)
{
	NSString* s = [NSString stringWithUTF8String:status] ?: @"";
	static CFAbsoluteTime last = 0;
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	/* Throttle to ~20 updates per second */
	if (pct >= 0 && pct < 100 && now - last < 0.05)
		return;
	last = now;
	dispatch_async(dispatch_get_main_queue(), ^{
		[main_window setProgress:pct text:(pct >= 0 && s.length) ? [NSString stringWithFormat:@"%@ %.1f%%", s, pct] : s];
	});
}

@implementation MainWindow {
	FlippedView* content;
	/* Drive Properties */
	NSTextField *hdrDrive, *lblDevice, *lblBoot, *lblImageOption, *lblPartition, *lblTarget, *lblPersistence;
	NSBox *sepDrive, *sepFormat, *sepStatus;
	NSPopUpButton *devicePopup, *bootPopup, *imageOptionPopup, *partitionPopup, *targetPopup, *persistUnits;
	NSButton *saveButton, *hashButton, *advDriveToggle;
	NSSegmentedControl* selectButton;
	NSSlider* persistSlider;
	NSTextField* persistField;
	NSButton *listUsbHdd, *oldBiosFixes, *uefiValidation;
	/* Format Options */
	NSTextField *hdrFormat, *lblLabel, *lblFS, *lblCluster;
	NSTextField* labelField;
	NSPopUpButton *fsPopup, *clusterPopup, *passesPopup;
	NSButton *advFormatToggle, *quickFormat, *extendedLabel, *badBlocks;
	/* Status */
	NSTextField* hdrStatus;
	RufusProgress* progress;
	NSView* glassBar;
	FlippedView* actionBar;
	NSView* windowGlass;
	BOOL glassWindow;
	NSButton *langButton, *aboutButton, *settingsButton, *logButton, *startButton, *closeButton;
	NSTextField *statusText, *timerText;

	/* State */
	BOOL advDrive, advFormat, busy, userChangedLabel, useDownload;
	boot_type_t bootType;
	NSString* imagePath;
	NSString* archivePath;
	image_report_t report;
	BOOL imageScanned;
	uint64_t persistenceSize;
	NSDate* opStart;
	NSTimer* opTimer;
	RufusDrive* opDrive;
	int opFd;
}

- (instancetype)init
{
	/* Tinted Liquid Glass window on macOS 26+ */
	BOOL glass = UIGlassWindowsEnabled();
	NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, WIDTH, 600)
		styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
			(glass ? NSWindowStyleMaskFullSizeContentView : 0)
		backing:NSBackingStoreBuffered defer:NO];
	if (glass) {
		w.titlebarAppearsTransparent = YES;
		w.opaque = NO;
		w.backgroundColor = NSColor.clearColor;
	}
	if ((self = [super initWithWindow:w])) {
		main_window = self;
		glassWindow = glass;
		opFd = -1;
		bootType = BT_IMAGE;
		w.delegate = self;
		w.restorable = NO;
		w.title = [NSString stringWithFormat:@"Rufus %s", RUFUS_MAC_VERSION];
		[w registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
		advDrive = [[NSUserDefaults standardUserDefaults] boolForKey:@"ShowAdvancedDriveProperties"];
		advFormat = [[NSUserDefaults standardUserDefaults] boolForKey:@"ShowAdvancedFormatOptions"];
		rflags.list_usb_hdd = [[NSUserDefaults standardUserDefaults] boolForKey:@"ListUSBHDD"];
		rflags.use_proper_size_units = [[NSUserDefaults standardUserDefaults] boolForKey:@"UseProperSizeUnits"];
		rflags.enable_sha512 = [[NSUserDefaults standardUserDefaults] boolForKey:@"EnableSHA512"];
		[self buildUI];
		set_log_handler(log_to_ui);
		set_progress_handler(progress_to_ui);

		__weak MainWindow* weakSelf = self;
		[Drives shared].onChange = ^{ [weakSelf refreshDevices]; };
		[self relocalize];
		[self layout:NO];
		[w center];
		[self installKeyMonitor];
	}
	return self;
}

/* ------------------------------------------------------------------------ */
/* UI construction                                                           */
/* ------------------------------------------------------------------------ */
- (NSBox*)separator
{
	NSBox* b = [[NSBox alloc] initWithFrame:NSZeroRect];
	b.boxType = NSBoxSeparator;
	return b;
}

- (void)buildUI
{
	content = [[FlippedView alloc] initWithFrame:NSMakeRect(0, 0, WIDTH, 600)];
	if (glassWindow) {
		if (@available(macOS 26.0, *)) {
			/* One glass pane for the whole window, gently tinted with the user's accent color */
			NSGlassEffectView* g = [[NSGlassEffectView alloc] initWithFrame:content.frame];
			g.style = NSGlassEffectViewStyleRegular;
			g.tintColor = [NSColor.controlAccentColor colorWithAlphaComponent:0.12];
			g.contentView = content;
			windowGlass = g;
			self.window.contentView = g;
		}
	} else {
		self.window.contentView = content;
	}

	hdrDrive = UIHeader(@""); sepDrive = [self separator];
	lblDevice = UILabel(@"");
	devicePopup = UIPopup(self, @selector(deviceChanged:));
	saveButton = UIIconButton(@"square.and.arrow.down", @"💾", @"", self, @selector(saveDriveImage:));
	lblBoot = UILabel(@"");
	bootPopup = UIPopup(self, @selector(bootChanged:));
	hashButton = UIIconButton(@"checkmark.seal", @"✓", @"", self, @selector(computeHashes:));
	selectButton = [NSSegmentedControl segmentedControlWithLabels:@[ @"SELECT", @"" ] trackingMode:NSSegmentSwitchTrackingMomentary
		target:self action:@selector(selectPressed:)];
	[selectButton setWidth:22 forSegment:1];
	NSImage* chevron = UISymbol(@"chevron.down", @"▾");
	if (chevron != nil)
		[selectButton setImage:chevron forSegment:1];
	else
		[selectButton setLabel:@"▾" forSegment:1];
	lblImageOption = UILabel(@"");
	imageOptionPopup = UIPopup(self, @selector(imageOptionChanged:));
	lblPersistence = UILabel(@"");
	persistSlider = [NSSlider sliderWithValue:0 minValue:0 maxValue:100 target:self action:@selector(persistenceChanged:)];
	persistField = [NSTextField textFieldWithString:@"0"];
	persistField.delegate = self;
	persistField.alignment = NSTextAlignmentRight;
	persistUnits = UIPopup(self, @selector(persistenceUnitsChanged:));
	lblPartition = UILabel(@"");
	partitionPopup = UIPopup(self, @selector(partitionChanged:));
	lblTarget = UILabel(@"");
	targetPopup = UIPopup(self, @selector(targetChanged:));
	advDriveToggle = [NSButton buttonWithTitle:@"" target:self action:@selector(toggleAdvDrive:)];
	advDriveToggle.bordered = NO;
	advDriveToggle.imagePosition = NSImageLeft;
	advDriveToggle.alignment = NSTextAlignmentLeft;
	listUsbHdd = UICheckbox(@"", self, @selector(listUsbHddChanged:));
	oldBiosFixes = UICheckbox(@"", self, nil);
	uefiValidation = UICheckbox(@"", self, nil);

	hdrFormat = UIHeader(@""); sepFormat = [self separator];
	lblLabel = UILabel(@"");
	labelField = [NSTextField textFieldWithString:@""];
	labelField.delegate = self;
	lblFS = UILabel(@"");
	fsPopup = UIPopup(self, @selector(fsChanged:));
	lblCluster = UILabel(@"");
	clusterPopup = UIPopup(self, nil);
	advFormatToggle = [NSButton buttonWithTitle:@"" target:self action:@selector(toggleAdvFormat:)];
	advFormatToggle.bordered = NO;
	advFormatToggle.imagePosition = NSImageLeft;
	advFormatToggle.alignment = NSTextAlignmentLeft;
	quickFormat = UICheckbox(@"", self, nil);
	quickFormat.state = NSControlStateValueOn;
	extendedLabel = UICheckbox(@"", self, nil);
	extendedLabel.state = NSControlStateValueOn;
	badBlocks = UICheckbox(@"", self, @selector(badBlocksChanged:));
	passesPopup = UIPopup(self, nil);

	hdrStatus = UIHeader(@""); sepStatus = [self separator];
	progress = [[RufusProgress alloc] initWithFrame:NSZeroRect];

	/* Floating action bar: the "functional layer", on Liquid Glass */
	langButton = UIIconButton(@"globe", @"🌐", @"", self, @selector(languageMenu:));
	aboutButton = UIIconButton(@"info.circle", @"ⓘ", @"", self, @selector(about:));
	settingsButton = UIIconButton(@"gearshape", @"⚙", @"", self, @selector(settings:));
	logButton = UIIconButton(@"list.bullet.rectangle", @"≡", @"", self, @selector(showLog:));
	startButton = [NSButton buttonWithTitle:@"START" target:self action:@selector(start:)];
	startButton.keyEquivalent = @"\r";   /* default button = primary tint prominence */
	closeButton = [NSButton buttonWithTitle:@"CLOSE" target:self action:@selector(closeOrCancel:)];
	UIApplyGlassBezel(startButton);
	UIApplyGlassBezel(closeButton);
	if (@available(macOS 26.0, *)) {
		if (UIHasLiquidGlass()) {
		startButton.controlSize = NSControlSizeLarge;
		closeButton.controlSize = NSControlSizeLarge;
		for (NSButton* b in @[ langButton, aboutButton, settingsButton, logButton ])
			b.controlSize = NSControlSizeLarge;
		}
	}
	actionBar = [[FlippedView alloc] initWithFrame:NSMakeRect(0, 0, WIDTH - 2 * MARGIN + 8, 52)];
	for (NSView* v in @[ langButton, aboutButton, settingsButton, logButton, startButton, closeButton ])
		[actionBar addSubview:v];
	if (glassWindow) {
		/* No glass on glass: the buttons are glass themselves, placed directly on the glass window.
		 * They're deliberately NOT grouped in an NSGlassEffectContainerView: the icon buttons sit
		 * within its merge distance, so it re-rendered their merged shape on every frame, which
		 * cost ~20% of WindowServer CPU while idle. The buttons never move, so there's nothing
		 * to merge anyway. */
		glassBar = actionBar;
	} else {
		glassBar = UIGlassContainer(actionBar, 26);
	}

	statusText = UILabel(@"");
	statusText.textColor = NSColor.secondaryLabelColor;
	timerText = UILabel(@"00:00:00");
	timerText.textColor = NSColor.secondaryLabelColor;
	timerText.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
	statusText.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
	timerText.alignment = NSTextAlignmentRight;

	for (NSView* v in @[ hdrDrive, sepDrive, lblDevice, devicePopup, saveButton, lblBoot, bootPopup, hashButton, selectButton,
		lblImageOption, imageOptionPopup, lblPersistence, persistSlider, persistField, persistUnits, lblPartition, partitionPopup,
		lblTarget, targetPopup, advDriveToggle, listUsbHdd, oldBiosFixes, uefiValidation,
		hdrFormat, sepFormat, lblLabel, labelField, lblFS, fsPopup, lblCluster, clusterPopup, advFormatToggle,
		quickFormat, extendedLabel, badBlocks, passesPopup, hdrStatus, sepStatus, progress, glassBar, statusText, timerText ])
		[content addSubview:v];

	/* Clicking the disclosure text should also toggle, as the Rufus toggle is a single button */
	advDriveToggle.state = advDrive ? NSControlStateValueOn : NSControlStateValueOff;
	advFormatToggle.state = advFormat ? NSControlStateValueOn : NSControlStateValueOff;
	listUsbHdd.state = rflags.list_usb_hdd ? NSControlStateValueOn : NSControlStateValueOff;
}

/* ------------------------------------------------------------------------ */
/* Layout (mirrors IDD_DIALOG, showing/hiding the optional rows)             */
/* ------------------------------------------------------------------------ */
- (CGFloat)place:(NSView*)v x:(CGFloat)x y:(CGFloat)y w:(CGFloat)w
{
	CGFloat h = v.fittingSize.height;
	if ([v isKindOfClass:[NSPopUpButton class]] || [v isKindOfClass:[NSSegmentedControl class]])
		h = MAX(h, 24);
	if ([v isKindOfClass:[NSTextField class]] && ((NSTextField*)v).isEditable)
		h = MAX(h, 24);
	/* Right to left languages (Arabic, Hebrew, Persian) mirror the layout */
	if ([Loc isRTL])
		x = WIDTH - x - w;
	v.frame = NSMakeRect(x, y, w, h);
	v.hidden = NO;
	return h;
}

- (void)layout:(BOOL)animate
{
	const CGFloat full = WIDTH - 2 * MARGIN;
	const CGFloat label_gap = 3, row_gap = 10, section_gap = 16;
	/* With a full size content view, start below the (transparent) title bar */
	CGFloat y = glassWindow ? 14 + (self.window.frame.size.height - NSHeight(self.window.contentLayoutRect)) : 14, h, x;
	BOOL showImageOption = (bootType == BT_IMAGE) && imageScanned && HAS_WINDOWS(&report) && HAS_WININST(&report) && !IS_DD_ONLY(&report);
	BOOL showPersistence = (bootType == BT_IMAGE) && imageScanned && HAS_PERSISTENCE(&report) && !IS_DD_ONLY(&report);

	/* Section header with a line to its right, like Rufus */
	h = [self place:hdrDrive x:MARGIN y:y w:hdrDrive.fittingSize.width];
	[self place:sepDrive x:MARGIN + hdrDrive.fittingSize.width + 8 y:y + h / 2 w:full - hdrDrive.fittingSize.width - 8];
	y += h + 8;

	y += [self place:lblDevice x:MARGIN y:y w:full] + label_gap;
	h = [self place:devicePopup x:MARGIN y:y w:full - SIDE_BTN - 6];
	[self place:saveButton x:MARGIN + full - SIDE_BTN y:y - 1 w:SIDE_BTN];
	y += h + row_gap;

	y += [self place:lblBoot x:MARGIN y:y w:full] + label_gap;
	h = [self place:bootPopup x:MARGIN y:y w:full - 120 - SIDE_BTN - 12];
	[self place:hashButton x:MARGIN + full - 120 - SIDE_BTN - 6 y:y - 1 w:SIDE_BTN];
	[self place:selectButton x:MARGIN + full - 120 y:y w:120];
	[selectButton setWidth:120 - 22 - 4 forSegment:0];
	y += h + row_gap;

	lblImageOption.hidden = imageOptionPopup.hidden = !showImageOption;
	if (showImageOption) {
		y += [self place:lblImageOption x:MARGIN y:y w:full] + label_gap;
		y += [self place:imageOptionPopup x:MARGIN y:y w:COL_W + MARGIN - 8] + row_gap;
	}
	lblPersistence.hidden = persistSlider.hidden = persistField.hidden = persistUnits.hidden = !showPersistence;
	if (showPersistence) {
		y += [self place:lblPersistence x:MARGIN y:y w:full] + label_gap;
		h = [self place:persistSlider x:MARGIN y:y + 2 w:full - 150];
		[self place:persistField x:MARGIN + full - 142 y:y w:70];
		[self place:persistUnits x:MARGIN + full - 66 y:y w:66];
		y += MAX(h, 24) + row_gap;
	}

	[self place:lblPartition x:MARGIN y:y w:COL_W];
	y += [self place:lblTarget x:COL2_X y:y w:COL_W] + label_gap;
	[self place:partitionPopup x:MARGIN y:y w:COL_W];
	y += [self place:targetPopup x:COL2_X y:y w:COL_W] + 6;

	x = MARGIN - 4;
	h = [self place:advDriveToggle x:x y:y w:full + 4];
	y += h + 2;
	listUsbHdd.hidden = oldBiosFixes.hidden = uefiValidation.hidden = !advDrive;
	if (advDrive) {
		y += [self place:listUsbHdd x:MARGIN + 14 y:y w:full - 14] + 4;
		y += [self place:oldBiosFixes x:MARGIN + 14 y:y w:full - 14] + 4;
		y += [self place:uefiValidation x:MARGIN + 14 y:y w:full - 14] + 4;
	}
	y += section_gap - 6;

	h = [self place:hdrFormat x:MARGIN y:y w:hdrFormat.fittingSize.width];
	[self place:sepFormat x:MARGIN + hdrFormat.fittingSize.width + 8 y:y + h / 2 w:full - hdrFormat.fittingSize.width - 8];
	y += h + 8;
	y += [self place:lblLabel x:MARGIN y:y w:full] + label_gap;
	y += [self place:labelField x:MARGIN y:y w:full] + row_gap;
	[self place:lblFS x:MARGIN y:y w:COL_W];
	y += [self place:lblCluster x:COL2_X y:y w:COL_W] + label_gap;
	[self place:fsPopup x:MARGIN y:y w:COL_W];
	y += [self place:clusterPopup x:COL2_X y:y w:COL_W] + 6;
	h = [self place:advFormatToggle x:x y:y w:full + 4];
	y += h + 2;
	quickFormat.hidden = extendedLabel.hidden = badBlocks.hidden = passesPopup.hidden = !advFormat;
	if (advFormat) {
		y += [self place:quickFormat x:MARGIN + 14 y:y w:full - 14] + 4;
		y += [self place:extendedLabel x:MARGIN + 14 y:y w:full - 14] + 4;
		/* Rufus puts the passes list on the same row; give the (longer) macOS label the room it needs */
		h = [self place:badBlocks x:MARGIN + 14 y:y + 2 w:full - 14 - 140];
		h = MAX(h, [self place:passesPopup x:MARGIN + full - 134 y:y w:134]);
		y += h + 4;
	}
	y += section_gap - 6;

	h = [self place:hdrStatus x:MARGIN y:y w:hdrStatus.fittingSize.width];
	[self place:sepStatus x:MARGIN + hdrStatus.fittingSize.width + 8 y:y + h / 2 w:full - hdrStatus.fittingSize.width - 8];
	y += h + 10;
	progress.frame = NSMakeRect(MARGIN, y, full, 24);
	y += 24 + 16;

	/* Action bar */
	{
		NSView* bar = actionBar;
		CGFloat bw = WIDTH - 2 * MARGIN + 8, bh = 52, bx = 10, btn = 34;
		NSArray* icons = @[ langButton, aboutButton, settingsButton, logButton ];
		BOOL rtl = [Loc isRTL];
		for (NSButton* b in icons) {
			CGFloat px = rtl ? bw - bx - btn : bx;
			b.frame = NSMakeRect(px, (bh - btn) / 2, btn, btn);
			bx += btn + 4;
		}
		[startButton sizeToFit];
		[closeButton sizeToFit];
		CGFloat sw = MAX(startButton.frame.size.width, 96), cw = MAX(closeButton.frame.size.width, 96);
		CGFloat sh = startButton.frame.size.height, ch = closeButton.frame.size.height;
		CGFloat cx = bw - 10 - cw, sx = cx - 8 - sw;
		if (rtl) {
			cx = 10;
			sx = 10 + cw + 8;
		}
		closeButton.frame = NSMakeRect(cx, (bh - ch) / 2, cw, ch);
		startButton.frame = NSMakeRect(sx, (bh - sh) / 2, sw, sh);
		bar.frame = NSMakeRect(0, 0, bw, bh);
		glassBar.frame = NSMakeRect(MARGIN - 4, y, bw, bh);
		y += bh + 10;
	}

	h = [self place:statusText x:MARGIN y:y w:full - 80];
	[self place:timerText x:MARGIN + full - 80 y:y w:80];
	y += h + 12;

	NSRect frame = self.window.frame;
	NSRect cr = [self.window contentRectForFrameRect:frame];
	CGFloat dh = y - cr.size.height;
	frame.size.height += dh;
	frame.origin.y -= dh;   /* keep the top edge in place */
	content.frame = NSMakeRect(0, 0, WIDTH, y);
	if (windowGlass != nil)
		windowGlass.frame = content.frame;
	[self.window setFrame:frame display:YES animate:animate && self.window.isVisible];
}

/* ------------------------------------------------------------------------ */
/* Localization                                                              */
/* ------------------------------------------------------------------------ */
- (void)relocalize
{
	hdrDrive.stringValue = L(@"IDS_DRIVE_PROPERTIES_TXT");
	lblDevice.stringValue = L(@"IDS_DEVICE_TXT");
	lblBoot.stringValue = L(@"IDS_BOOT_SELECTION_TXT");
	lblImageOption.stringValue = L(@"IDS_IMAGE_OPTION_TXT");
	lblPartition.stringValue = L(@"IDS_PARTITION_TYPE_TXT");
	lblTarget.stringValue = L(@"IDS_TARGET_SYSTEM_TXT");
	lblPersistence.stringValue = L(@"MSG_123");
	listUsbHdd.title = L(@"IDC_LIST_USB_HDD");
	oldBiosFixes.title = L(@"IDC_OLD_BIOS_FIXES");
	uefiValidation.title = L(@"IDC_UEFI_MEDIA_VALIDATION");
	hdrFormat.stringValue = L(@"IDS_FORMAT_OPTIONS_TXT");
	lblLabel.stringValue = L(@"IDS_LABEL_TXT");
	lblFS.stringValue = L(@"IDS_FILE_SYSTEM_TXT");
	lblCluster.stringValue = L(@"IDS_CLUSTER_SIZE_TXT");
	quickFormat.title = L(@"IDC_QUICK_FORMAT");
	extendedLabel.title = L(@"IDC_EXTENDED_LABEL");
	badBlocks.title = L(@"IDC_BAD_BLOCKS");
	hdrStatus.stringValue = L(@"IDS_STATUS_TXT");
	[selectButton setLabel:useDownload ? [L(@"MSG_040") uppercaseString] : L(@"IDC_SELECT").uppercaseString forSegment:0];
	startButton.title = L(@"IDC_START").uppercaseString;
	closeButton.title = (busy ? L(@"MSG_007") : L(@"MSG_006")).uppercaseString;
	[self updateToggleTitles];

	saveButton.toolTip = L(@"MSG_304");
	hashButton.toolTip = L(@"MSG_272");
	langButton.toolTip = L(@"MSG_273");
	aboutButton.toolTip = L(@"MSG_302");
	settingsButton.toolTip = L(@"MSG_301");
	logButton.toolTip = L(@"MSG_303");
	oldBiosFixes.toolTip = L(@"MSG_169");
	listUsbHdd.toolTip = L(@"MSG_170");
	quickFormat.toolTip = L(@"MSG_162");
	extendedLabel.toolTip = L(@"MSG_166");
	badBlocks.toolTip = L(@"MSG_161");
	passesPopup.toolTip = L(@"MSG_316");
	partitionPopup.toolTip = L(@"MSG_163");
	bootPopup.toolTip = L(@"MSG_164");
	fsPopup.toolTip = L(@"MSG_157");
	clusterPopup.toolTip = L(@"MSG_158");
	labelField.toolTip = L(@"MSG_159");
	targetPopup.toolTip = [NSString stringWithFormat:@"%@\n%@\n%@", L(@"MSG_150"), L(@"MSG_151"), L(@"MSG_152")];
	startButton.toolTip = L(@"MSG_171");
	selectButton.toolTip = L(@"MSG_165");
	advDriveToggle.toolTip = advFormatToggle.toolTip = L(@"MSG_160");
	persistSlider.toolTip = L(@"MSG_125");
	persistUnits.toolTip = L(@"MSG_126");
	uefiValidation.toolTip = L(@"MSG_167");
	uefiValidation.enabled = NO;   /* needs the uefi-md5sum bootloaders, not bundled yet */

	[passesPopup removeAllItems];
	/* Same entries as Rufus: 1 pass, 2 passes (SLC), 3 passes (MLC), 4 passes (TLC) */
	static const char* pass_type[] = { "", "", "(SLC)", "(MLC)", "(TLC)" };
	for (int i = 1; i <= 4; i++)
		[passesPopup addItemWithTitle:(i == 1) ? LF(@"MSG_034", 1) : LF(@"MSG_035", i, pass_type[i])];
	[persistUnits removeAllItems];
	[persistUnits addItemsWithTitles:@[ L(@"MSG_022"), L(@"MSG_023") ]];
	[persistUnits selectItemAtIndex:1];

	[self populateBootSelection];
	[self refreshDevices];
	self.window.contentView.userInterfaceLayoutDirection = [Loc isRTL] ?
		NSUserInterfaceLayoutDirectionRightToLeft : NSUserInterfaceLayoutDirectionLeftToRight;
	[self layout:NO];
	if (!busy)
		[self setProgress:0 text:L(@"MSG_210")];
}

- (void)updateToggleTitles
{
	/* Rufus: "Show advanced drive properties" / "Hide advanced drive properties" */
	NSString* d = [NSString stringWithFormat:@"%@", advDrive ? LF(@"MSG_122", [L(@"MSG_119") UTF8String]) :
		LF(@"MSG_121", [L(@"MSG_119") UTF8String])];
	NSString* f = [NSString stringWithFormat:@"%@", advFormat ? LF(@"MSG_122", [L(@"MSG_120") UTF8String]) :
		LF(@"MSG_121", [L(@"MSG_120") UTF8String])];
	/* Rufus uses a button with an arrow icon: a chevron that points down when expanded */
	NSImage* open = UISymbol(@"chevron.down", @"▾"), * closed = UISymbol(@"chevron.right", @"▸");
	advDriveToggle.image = advDrive ? open : closed;
	advFormatToggle.image = advFormat ? open : closed;
	if (open == nil) {
		d = [(advDrive ? @"▾ " : @"▸ ") stringByAppendingString:d];
		f = [(advFormat ? @"▾ " : @"▸ ") stringByAppendingString:f];
	}
	advDriveToggle.attributedTitle = [[NSAttributedString alloc] initWithString:d attributes:@{
		NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightMedium] }];
	advFormatToggle.attributedTitle = [[NSAttributedString alloc] initWithString:f attributes:@{
		NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightMedium] }];
}

/* ------------------------------------------------------------------------ */
/* Devices                                                                   */
/* ------------------------------------------------------------------------ */
- (RufusDrive*)selectedDrive
{
	NSInteger i = devicePopup.indexOfSelectedItem;
	NSArray* d = [Drives shared].drives;
	return (i >= 0 && i < (NSInteger)d.count) ? d[i] : nil;
}

- (void)refreshDevices
{
	if (busy)
		return;
	NSString* prev = [self selectedDrive].bsdName ?: [[NSUserDefaults standardUserDefaults] stringForKey:@"LastDevice"];
	NSArray<RufusDrive*>* drives = [Drives shared].drives;
	[devicePopup removeAllItems];
	for (RufusDrive* d in drives)
		[devicePopup addItemWithTitle:[d displayName]];
	for (NSUInteger i = 0; i < drives.count; i++)
		if ([drives[i].bsdName isEqualToString:prev])
			[devicePopup selectItemAtIndex:i];
	if (drives.count == 0)
		[devicePopup addItemWithTitle:@""];
	devicePopup.enabled = drives.count > 0;
	[self setStatusText:(drives.count == 1) ? LF(@"MSG_208", 1) : LF(@"MSG_209", (int)drives.count)];
	[self deviceChanged:nil];
}

- (void)deviceChanged:(id)sender
{
	RufusDrive* d = [self selectedDrive];
	if (d != nil)
		[[NSUserDefaults standardUserDefaults] setObject:d.bsdName forKey:@"LastDevice"];
	if (sender != nil)
		userChangedLabel = NO;
	[self updatePartitionScheme];
	[self updateFileSystems];
	[self setProposedLabel];
	[self enableControls];
}

/* ------------------------------------------------------------------------ */
/* Boot selection                                                            */
/* ------------------------------------------------------------------------ */
- (void)populateBootSelection
{
	[bootPopup removeAllItems];
	NSString* img = (imagePath != nil) ? imagePath.lastPathComponent :
		LF(@"MSG_281", [L(@"MSG_280") UTF8String]);
	NSArray* items = @[ L(@"MSG_279"), img, @"FreeDOS" ];
	NSArray* tags = @[ @(BT_NON_BOOTABLE), @(BT_IMAGE), @(BT_FREEDOS) ];
	for (NSUInteger i = 0; i < items.count; i++) {
		[bootPopup addItemWithTitle:items[i]];
		bootPopup.lastItem.tag = [tags[i] integerValue];
	}
	/* Extra boot types, shown in advanced mode like Rufus */
	if (advDrive || rflags.expert_mode) {
		[bootPopup addItemWithTitle:@"UEFI:NTFS"];
		bootPopup.lastItem.tag = BT_UEFI_NTFS;
	}
	[bootPopup selectItemWithTag:bootType];
	if (bootPopup.indexOfSelectedItem < 0)
		[bootPopup selectItemWithTag:BT_IMAGE];
}

- (void)bootChanged:(id)sender
{
	bootType = (boot_type_t)bootPopup.selectedTag;
	userChangedLabel = NO;
	[self updatePartitionScheme];
	[self updateFileSystems];
	[self setProposedLabel];
	[self enableControls];
	[self layout:YES];
}

- (void)imageOptionChanged:(id)sender { [self updateFileSystems]; }

/* SetPartitionSchemeAndTargetSystem() from rufus.c */
- (void)updatePartitionScheme
{
	BOOL pt_ok[3] = { YES, YES, NO };      /* MBR, GPT, SFD */
	BOOL tt_ok[3] = { YES, YES, NO };      /* BIOS(CSM), UEFI(non CSM), BIOS or UEFI */
	RufusDrive* drive = [self selectedDrive];
	image_report_t* r = &report;
	NSInteger prev_pt = partitionPopup.indexOfSelectedItem >= 0 ? partitionPopup.selectedTag : -1;
	NSInteger preferred;

	[partitionPopup removeAllItems];
	[targetPopup removeAllItems];
	if (drive == nil)
		return;
	switch (bootType) {
	case BT_NON_BOOTABLE:
		pt_ok[PS_SFD] = YES;
		tt_ok[0] = tt_ok[1] = NO;
		tt_ok[2] = YES;
		break;
	case BT_IMAGE:
		if (!imageScanned)
			break;
		if (!IS_EFI_BOOTABLE(r)) {
			pt_ok[PS_GPT] = NO;
			tt_ok[1] = NO;
			break;
		}
		if (IS_BIOS_BOOTABLE(r)) {
			if (!HAS_WINDOWS(r) || rflags.allow_dual_uefi_bios) {
				tt_ok[0] = NO;
				tt_ok[1] = tt_ok[2] = YES;
			}
		} else {
			tt_ok[0] = NO;
		}
		break;
	case BT_FREEDOS:
		pt_ok[PS_GPT] = NO;
		tt_ok[1] = NO;
		break;
	case BT_UEFI_NTFS:
		tt_ok[0] = NO;
		break;
	default:
		break;
	}
	if (pt_ok[PS_MBR]) { [partitionPopup addItemWithTitle:@"MBR"]; partitionPopup.lastItem.tag = PS_MBR; }
	if (pt_ok[PS_GPT]) { [partitionPopup addItemWithTitle:@"GPT"]; partitionPopup.lastItem.tag = PS_GPT; }
	if (pt_ok[PS_SFD]) { [partitionPopup addItemWithTitle:@"Super Floppy Disk"]; partitionPopup.lastItem.tag = PS_SFD; }

	preferred = (drive.size > 2 * TB) ? PS_GPT : PS_MBR;
	if (bootType == BT_NON_BOOTABLE)
		preferred = (prev_pt >= 0) ? prev_pt : PS_MBR;
	else if (bootType == BT_UEFI_NTFS)
		preferred = PS_GPT;
	else if (bootType == BT_IMAGE && imageScanned && r->is_iso) {
		if (HAS_WINDOWS(r) && r->has_efi)
			preferred = rflags.allow_dual_uefi_bios ? PS_MBR : PS_GPT;
		if (IS_DD_BOOTABLE(r))
			preferred = PS_MBR;
	}
	if (![partitionPopup selectItemWithTag:preferred])
		[partitionPopup selectItemAtIndex:0];
	[self updateTargetSystem:tt_ok];
}

- (void)updateTargetSystem:(BOOL*)tt_ok
{
	NSInteger pt = partitionPopup.selectedTag;
	image_report_t* r = &report;
	[targetPopup removeAllItems];
	if (tt_ok[0] && pt != PS_GPT) {
		[targetPopup addItemWithTitle:L(@"MSG_031")];
		targetPopup.lastItem.tag = TT_BIOS;
	}
	if (tt_ok[1] && !(pt == PS_MBR && bootType == BT_IMAGE && imageScanned && IS_BIOS_BOOTABLE(r) && IS_EFI_BOOTABLE(r))) {
		[targetPopup addItemWithTitle:L(@"MSG_032")];
		targetPopup.lastItem.tag = TT_UEFI;
	}
	if (tt_ok[2] && (pt != PS_GPT || bootType == BT_NON_BOOTABLE)) {
		[targetPopup addItemWithTitle:L(@"MSG_033")];
		targetPopup.lastItem.tag = TT_BIOS;
	}
	[targetPopup selectItemAtIndex:0];
}

- (void)partitionChanged:(id)sender
{
	/* Recompute the allowed targets for the new scheme */
	NSInteger pt = partitionPopup.selectedTag;
	[self updatePartitionScheme];
	[partitionPopup selectItemWithTag:pt];
	BOOL tt[3] = { YES, YES, YES };
	image_report_t* r = &report;
	if (bootType == BT_NON_BOOTABLE) { tt[0] = tt[1] = NO; }
	else if (bootType == BT_FREEDOS) { tt[1] = tt[2] = NO; }
	else if (bootType == BT_UEFI_NTFS) { tt[0] = NO; tt[2] = NO; }
	else if (bootType == BT_IMAGE && imageScanned) {
		if (!IS_EFI_BOOTABLE(r)) { tt[1] = tt[2] = NO; }
		else if (IS_BIOS_BOOTABLE(r)) { if (!HAS_WINDOWS(r) || rflags.allow_dual_uefi_bios) tt[0] = NO; else tt[2] = NO; }
		else { tt[0] = NO; tt[2] = NO; }
	}
	[self updateTargetSystem:tt];
	[self updateFileSystems];
}

- (void)targetChanged:(id)sender { [self updateFileSystems]; }

/* ------------------------------------------------------------------------ */
/* File system and cluster size                                              */
/* ------------------------------------------------------------------------ */
- (void)updateFileSystems
{
	BOOL allowed[FS_MAX] = { NO };
	RufusDrive* drive = [self selectedDrive];
	image_report_t* r = &report;
	NSInteger prev = fsPopup.indexOfSelectedItem >= 0 ? fsPopup.selectedTag : -1;
	NSInteger preferred = -1, default_fs = -1;
	target_t tt = (target_t)targetPopup.selectedTag;
	int i;

	[fsPopup removeAllItems];
	[clusterPopup removeAllItems];
	if (drive == nil)
		return;

	/* SetAllowedFileSystems() */
	switch (bootType) {
	case BT_NON_BOOTABLE:
		for (i = 0; i < FS_MAX; i++)
			allowed[i] = YES;
		allowed[FS_UDF] = allowed[FS_REFS] = allowed[FS_EXT4] = NO;   /* not available on macOS */
		break;
	case BT_FREEDOS:
		allowed[FS_FAT16] = allowed[FS_FAT32] = YES;
		break;
	case BT_IMAGE:
		allowed[FS_NTFS] = YES;
		if (imageScanned && !IS_FAT32_COMPAT(r))
			break;
		if (!HAS_WINDOWS(r) || tt != TT_BIOS || targetPopup.indexOfSelectedItem != 0 || rflags.allow_dual_uefi_bios ||
			[targetPopup.titleOfSelectedItem isEqualToString:L(@"MSG_033")])
			allowed[FS_FAT16] = allowed[FS_FAT32] = YES;
		if (imageScanned && HAS_WINDOWS(r) && tt == TT_BIOS && ![targetPopup.titleOfSelectedItem isEqualToString:L(@"MSG_033")] &&
			!rflags.allow_dual_uefi_bios)
			allowed[FS_FAT16] = allowed[FS_FAT32] = NO;
		break;
	case BT_UEFI_NTFS:
		allowed[FS_NTFS] = allowed[FS_EXFAT] = YES;
		break;
	default:
		break;
	}
	for (i = 0; i < FS_MAX; i++) {
		uint32_t a, d;
		if (!allowed[i])
			continue;
		cluster_sizes((fs_type_t)i, drive.size, drive.blockSize ?: 512, &a, &d);
		if (a == 0)
			continue;
		NSString* name = @(fs_name[i]);
		if (i == FS_FAT32 && (drive.size > 32 * GB || rflags.force_large_fat32))
			name = [@"Large " stringByAppendingString:name];
		if (default_fs < 0) {
			default_fs = i;
			name = LF(@"MSG_030", name.UTF8String);
		}
		[fsPopup addItemWithTitle:name];
		fsPopup.lastItem.tag = i;
	}

	/* SetFSFromISO() */
	if (bootType == BT_IMAGE && imageScanned) {
		if (HAS_SYSLINUX(r) || r->has_kolibrios || (IS_EFI_BOOTABLE(r) && tt == TT_UEFI && !r->has_4GB_file))
			preferred = allowed[FS_FAT32] ? FS_FAT32 : FS_FAT16;
		else if (HAS_BOOTMGR(r) || HAS_WINPE(r))
			preferred = (allowed[FS_FAT32] && !r->has_4GB_file && rflags.allow_dual_uefi_bios) ? FS_FAT32 : FS_NTFS;
	} else if (prev >= 0) {
		preferred = prev;
	}
	if (preferred < 0 || ![fsPopup selectItemWithTag:preferred])
		[fsPopup selectItemAtIndex:0];
	[self fsChanged:nil];
}

- (void)fsChanged:(id)sender
{
	RufusDrive* drive = [self selectedDrive];
	fs_type_t fs = (fs_type_t)fsPopup.selectedTag;
	static const char* cs_label[] = { "256 bytes", "512 bytes", "1024 bytes", "2048 bytes", "4096 bytes", "8192 bytes",
		"16 kilobytes", "32 kilobytes", "64 kilobytes", "128 kilobytes", "256 kilobytes", "512 kilobytes",
		"1024 kilobytes", "2048 kilobytes", "4096 kilobytes", "8192 kilobytes", "16 megabytes", "32 megabytes" };
	uint32_t allowed, def, j;
	int i;

	[clusterPopup removeAllItems];
	if (drive == nil || fsPopup.indexOfSelectedItem < 0)
		return;
	cluster_sizes(fs, drive.size, drive.blockSize ?: 512, &allowed, &def);
	if (allowed == 0x100) {
		[clusterPopup addItemWithTitle:LF(@"MSG_030", [L(@"MSG_029") UTF8String])];
		clusterPopup.lastItem.tag = 0;
		return;
	}
	for (i = 0, j = 0x100; j < 0x10000000; i++, j <<= 1) {
		if (!(j & allowed))
			continue;
		NSString* t = @(cs_label[i]);
		t = [t stringByReplacingOccurrencesOfString:@"kilobytes" withString:L(@"MSG_027")];
		t = [t stringByReplacingOccurrencesOfString:@"megabytes" withString:L(@"MSG_028")];
		t = [t stringByReplacingOccurrencesOfString:@"bytes" withString:L(@"MSG_026")];
		if (j == def)
			t = LF(@"MSG_030", t.UTF8String);
		[clusterPopup addItemWithTitle:t];
		clusterPopup.lastItem.tag = j;
		if (j == def)
			[clusterPopup selectItem:clusterPopup.lastItem];
	}
	[self enableControls];
}

- (void)setProposedLabel
{
	RufusDrive* d = [self selectedDrive];
	if (userChangedLabel)
		return;
	if (bootType == BT_IMAGE && imageScanned && report.label[0] != 0)
		labelField.stringValue = @(report.label);
	else if (d.label.length > 0)
		labelField.stringValue = d.label;
	else if (d != nil)
		labelField.stringValue = [SizeToHuman(d.size, NO, YES) stringByReplacingOccurrencesOfString:@"." withString:@","];
	else
		labelField.stringValue = @"";
}

- (void)controlTextDidChange:(NSNotification*)n
{
	if (n.object == labelField)
		userChangedLabel = YES;
	else if (n.object == persistField)
		[self persistenceFieldChanged];
}

/* EnableBootOptions() */
- (void)enableControls
{
	BOOL has_drive = [self selectedDrive] != nil;
	BOOL enable = has_drive && !busy && !(bootType == BT_IMAGE && !imageScanned);
	BOOL dd_only = bootType == BT_IMAGE && imageScanned && IS_DD_BOOTABLE(&report) && !report.is_iso;

	devicePopup.enabled = !busy && [Drives shared].drives.count > 0;
	bootPopup.enabled = !busy;
	selectButton.enabled = !busy && bootType == BT_IMAGE;
	hashButton.enabled = !busy && bootType == BT_IMAGE && imagePath != nil;
	saveButton.enabled = !busy && has_drive;
	imageOptionPopup.enabled = enable && !dd_only;
	partitionPopup.enabled = targetPopup.enabled = enable && !dd_only && partitionPopup.numberOfItems > 0;
	fsPopup.enabled = clusterPopup.enabled = enable && !dd_only;
	labelField.enabled = enable && !dd_only;
	quickFormat.enabled = extendedLabel.enabled = enable && !dd_only;
	badBlocks.enabled = passesPopup.enabled = has_drive && !busy;
	passesPopup.enabled = passesPopup.enabled && badBlocks.state == NSControlStateValueOn;
	listUsbHdd.enabled = oldBiosFixes.enabled = !busy;
	persistSlider.enabled = enable && !dd_only;
	persistField.enabled = persistUnits.enabled = enable && !dd_only && persistenceSize != 0;
	advDriveToggle.enabled = advFormatToggle.enabled = YES;
	startButton.enabled = !busy && enable;
	langButton.enabled = settingsButton.enabled = !busy;
}

- (void)badBlocksChanged:(id)sender { [self enableControls]; }

- (void)listUsbHddChanged:(id)sender
{
	rflags.list_usb_hdd = listUsbHdd.state == NSControlStateValueOn;
	[[NSUserDefaults standardUserDefaults] setBool:rflags.list_usb_hdd forKey:@"ListUSBHDD"];
	[[Drives shared] refresh];
}

- (void)toggleAdvDrive:(id)sender
{
	advDrive = !advDrive;
	advDriveToggle.state = advDrive ? NSControlStateValueOn : NSControlStateValueOff;
	[[NSUserDefaults standardUserDefaults] setBool:advDrive forKey:@"ShowAdvancedDriveProperties"];
	[self updateToggleTitles];
	[self populateBootSelection];
	[self layout:YES];
}

- (void)toggleAdvFormat:(id)sender
{
	advFormat = !advFormat;
	advFormatToggle.state = advFormat ? NSControlStateValueOn : NSControlStateValueOff;
	[[NSUserDefaults standardUserDefaults] setBool:advFormat forKey:@"ShowAdvancedFormatOptions"];
	[self updateToggleTitles];
	[self layout:YES];
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                               */
/* ------------------------------------------------------------------------ */
- (void)setupPersistence
{
	RufusDrive* d = [self selectedDrive];
	uint64_t max = 0;
	persistenceSize = 0;
	if (d != nil && d.size > report.projected_size + 1 * GB)
		max = d.size - report.projected_size - 512 * MB;
	persistSlider.maxValue = (double)(max / MB);
	persistSlider.doubleValue = 0;
	persistField.stringValue = L(@"MSG_124");
}

- (void)persistenceChanged:(id)sender
{
	persistenceSize = (uint64_t)persistSlider.doubleValue * MB;
	if (persistenceSize < 128 * MB && persistenceSize != 0)
		persistenceSize = 0;
	if (persistenceSize == 0)
		persistField.stringValue = L(@"MSG_124");
	else if (persistUnits.indexOfSelectedItem == 1)
		persistField.stringValue = [NSString stringWithFormat:@"%.1f", (double)persistenceSize / GB];
	else
		persistField.stringValue = [NSString stringWithFormat:@"%llu", persistenceSize / MB];
	[self enableControls];
}

- (void)persistenceUnitsChanged:(id)sender { [self persistenceChanged:nil]; }

- (void)persistenceFieldChanged
{
	double v = persistField.doubleValue;
	persistenceSize = (uint64_t)(v * (persistUnits.indexOfSelectedItem == 1 ? GB : MB));
	persistSlider.doubleValue = (double)(persistenceSize / MB);
}

/* ------------------------------------------------------------------------ */
/* Image selection                                                           */
/* ------------------------------------------------------------------------ */
- (void)selectPressed:(NSSegmentedControl*)sender
{
	BOOL ctrl = (NSEvent.modifierFlags & NSEventModifierFlagControl) != 0;
	if (sender.selectedSegment == 1) {
		NSMenu* m = [NSMenu new];
		[m addItemWithTitle:L(@"IDC_SELECT").uppercaseString action:@selector(chooseSelect:) keyEquivalent:@""].target = self;
		[m addItemWithTitle:[L(@"MSG_040") uppercaseString] action:@selector(chooseDownload:) keyEquivalent:@""].target = self;
		[m popUpMenuPositioningItem:nil atLocation:NSMakePoint(sender.bounds.size.width - 22, sender.bounds.size.height + 4) inView:sender];
		return;
	}
	if (ctrl) {
		/* Ctrl-SELECT: pick an extra archive whose content gets copied to the drive */
		[self selectArchive];
		return;
	}
	if (useDownload)
		[self downloadISO];
	else
		[self browseForImage];
}

- (void)chooseSelect:(id)sender
{
	useDownload = NO;
	[self relocalize];
	[self browseForImage];
}

- (void)chooseDownload:(id)sender
{
	useDownload = YES;
	[self relocalize];
	[self downloadISO];
}

- (void)browseForImage
{
	NSOpenPanel* p = [NSOpenPanel openPanel];
	p.allowsMultipleSelection = NO;
	p.message = L(@"MSG_036");
	NSString* last = [[NSUserDefaults standardUserDefaults] stringForKey:@"LastImageDir"];
	if (last != nil)
		p.directoryURL = [NSURL fileURLWithPath:last];
	[p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
		if (r != NSModalResponseOK)
			return;
		[[NSUserDefaults standardUserDefaults] setObject:p.URL.path.stringByDeletingLastPathComponent forKey:@"LastImageDir"];
		[self openImageAtPath:p.URL.path];
	}];
}

- (void)selectArchive
{
	NSOpenPanel* p = [NSOpenPanel openPanel];
	p.message = @"Select an archive (.zip) whose content will be copied to the drive after the main operation";
	[p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
		if (r != NSModalResponseOK)
			return;
		self->archivePath = p.URL.path;
		uprintf("Using archive: %s", self->archivePath.UTF8String);
	}];
}

- (void)openImageAtPath:(NSString*)path
{
	if (busy)
		return;
	imagePath = path;
	imageScanned = NO;
	bootType = BT_IMAGE;
	userChangedLabel = NO;
	[self populateBootSelection];
	[self setProgress:-1 text:L(@"MSG_202")];
	busy = YES;
	[self enableControls];
	uprintf("Image: %s", path.UTF8String);
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		image_report_t r;
		BOOL ok = image_scan(path.UTF8String, &r);
		dispatch_async(dispatch_get_main_queue(), ^{
			self->busy = NO;
			if (!ok) {
				self->imagePath = nil;
				[self populateBootSelection];
				[self setProgress:0 text:L(@"MSG_203")];
				NSAlert* a = [GlassAlert new];
				a.messageText = L(@"MSG_203");
				a.informativeText = [NSString stringWithFormat:@"%@", path.lastPathComponent];
				a.alertStyle = NSAlertStyleCritical;
				[a beginSheetModalForWindow:self.window completionHandler:nil];
				[self enableControls];
				return;
			}
			self->report = r;
			self->imageScanned = YES;
			if (HAS_PERSISTENCE(&r))
				[self setupPersistence];
			if (HAS_WINDOWS(&r) && HAS_WININST(&r)) {
				[self->imageOptionPopup removeAllItems];
				[self->imageOptionPopup addItemWithTitle:L(@"MSG_117")];
			}
			[self setProgress:0 text:LF(@"MSG_205", path.lastPathComponent.UTF8String)];
			[self setStatusText:LF(@"MSG_205", path.lastPathComponent.UTF8String)];
			[self bootChanged:nil];
			if (r.disable_iso)
				uprintf("This image must be written in DD mode");
		});
	});
}

- (void)downloadISO
{
	[FidoPanel showForWindow:self.window completion:^(NSString* downloadedPath) {
		if (downloadedPath != nil)
			[self openImageAtPath:downloadedPath];
	}];
}

/* ------------------------------------------------------------------------ */
/* Side buttons                                                              */
/* ------------------------------------------------------------------------ */
- (void)computeHashes:(id)sender
{
	if (imagePath == nil)
		return;
	NSString* path = imagePath;
	busy = YES;
	[self enableControls];
	[self startTimer];
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		hashes_t h;
		cancel_requested = false;
		BOOL ok = compute_hashes(path.UTF8String, &h, rflags.enable_sha512);
		dispatch_async(dispatch_get_main_queue(), ^{
			self->busy = NO;
			[self stopTimer];
			[self enableControls];
			[self setProgress:0 text:L(@"MSG_210")];
			if (ok)
				[HashPanel showHashes:h sha512:rflags.enable_sha512 forWindow:self.window];
		});
	});
}

- (void)saveDriveImage:(id)sender
{
	RufusDrive* d = [self selectedDrive];
	if (d == nil)
		return;
	NSSavePanel* p = [NSSavePanel savePanel];
	p.nameFieldStringValue = [NSString stringWithFormat:@"%@.img", d.label.length ? d.label : d.bsdName];
	p.message = L(@"MSG_304");
	[p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
		if (r != NSModalResponseOK)
			return;
		[self runOperation:^int(rdev_t* dev) {
			return [self saveDevice:dev toPath:p.URL.path];
		} readOnly:YES drive:d];
	}];
}

- (int)saveDevice:(rdev_t*)dev toPath:(NSString*)path
{
	const size_t chunk = 4 * MB;
	uint8_t* buf = malloc(chunk);
	FILE* out = fopen(path.fileSystemRepresentation, "wb");
	uint64_t off;
	int ret = -1;
	update_status("Saving drive image...");
	uprintf("Saving %s to '%s'", dev->path, path.UTF8String);
	for (off = 0; buf != NULL && out != NULL && off < dev->size; off += chunk) {
		size_t n = (dev->size - off < chunk) ? (size_t)(dev->size - off) : chunk;
		if (cancel_requested || !rdev_read(dev, off, buf, n) || fwrite(buf, 1, n, out) != n)
			goto done;
		update_progress_bytes(NULL, off + n, dev->size);
	}
	ret = 0;
	uprintf("%s", [LF(@"MSG_216", path.UTF8String) UTF8String]);
done:
	if (out != NULL)
		fclose(out);
	free(buf);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Bottom bar                                                                */
/* ------------------------------------------------------------------------ */
- (void)languageMenu:(NSButton*)sender
{
	NSMenu* m = [NSMenu new];
	for (RufusLanguage* l in [Loc languages]) {
		NSMenuItem* it = [m addItemWithTitle:l.name action:@selector(pickLanguage:) keyEquivalent:@""];
		it.target = self;
		it.representedObject = l.code;
		it.state = [l.code isEqualToString:[Loc currentCode]] ? NSControlStateValueOn : NSControlStateValueOff;
	}
	[m popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, sender.bounds.size.height + 2) inView:sender];
}

- (void)pickLanguage:(NSMenuItem*)item
{
	[Loc selectLanguage:item.representedObject];
	[[NSUserDefaults standardUserDefaults] setObject:item.representedObject forKey:@"Language"];
	[self relocalize];
	[[NSNotificationCenter defaultCenter] postNotificationName:@"RufusLanguageChanged" object:nil];
}

- (void)about:(id)sender { [AboutPanel showForWindow:self.window]; }
- (void)settings:(id)sender { [SettingsPanel showForWindow:self.window]; }
- (void)showLog:(id)sender { [[LogWindow shared] toggle]; }

- (void)closeOrCancel:(id)sender
{
	if (!busy) {
		[self.window performClose:nil];
		return;
	}
	NSAlert* a = [GlassAlert new];
	a.messageText = L(@"MSG_049");
	a.informativeText = L(@"MSG_105");
	a.alertStyle = NSAlertStyleWarning;
	[a addButtonWithTitle:L(@"MSG_008")];
	[a addButtonWithTitle:L(@"MSG_009")];
	[a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
		if (r == NSAlertFirstButtonReturn && self->busy) {
			cancel_requested = true;
			kill_helpers();
			[self setProgress:-1 text:L(@"MSG_201")];
		}
	}];
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
	if (busy) {
		[self closeOrCancel:nil];
		return NO;
	}
	[NSApp terminate:nil];
	return YES;
}

- (BOOL)isBusy { return busy; }

- (void)setProgress:(double)pct text:(NSString*)text
{
	progress.value = pct;
	progress.error = progress.success = NO;
	if (text != nil)
		progress.text = text;
}

- (void)setStatusText:(NSString*)text
{
	statusText.stringValue = text ?: @"";
}

- (void)startTimer
{
	opStart = [NSDate date];
	timerText.stringValue = @"00:00:00";
	[opTimer invalidate];
	opTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer* t) {
		long s = (long)-[self->opStart timeIntervalSinceNow];
		self->timerText.stringValue = [NSString stringWithFormat:@"%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60];
	}];
}

- (void)stopTimer
{
	[opTimer invalidate];
	opTimer = nil;
}

/* ------------------------------------------------------------------------ */
/* START                                                                     */
/* ------------------------------------------------------------------------ */
- (NSModalResponse)alert:(NSString*)title text:(NSString*)text style:(NSAlertStyle)style buttons:(NSArray*)buttons accessory:(NSView*)acc
{
	NSAlert* a = [GlassAlert new];
	a.messageText = title;
	a.informativeText = text ?: @"";
	a.alertStyle = style;
	for (NSString* b in buttons)
		[a addButtonWithTitle:b];
	if (acc != nil)
		a.accessoryView = acc;
	return [a runModal];
}

- (void)start:(id)sender
{
	RufusDrive* drive = [self selectedDrive];
	job_options_t* o;
	BOOL write_as_image = NO;
	wue_options_t* wue = NULL;

	if (drive == nil || busy)
		return;
	if (bootType == BT_IMAGE && !imageScanned)
		return;

	/* Is the image on the target drive? (MSG_358/359) */
	if (bootType == BT_IMAGE) {
		NSURL* url = [NSURL fileURLWithPath:imagePath];
		NSString* vol = nil;
		[url getResourceValue:&vol forKey:NSURLVolumeIdentifierKey error:nil];
		struct stat st;
		if (stat(imagePath.fileSystemRepresentation, &st) == 0) {
			char dev[64];
			devname_r(st.st_dev, S_IFBLK, dev, sizeof(dev));
			if (strncmp(dev, drive.bsdName.UTF8String, drive.bsdName.length) == 0 &&
				(dev[drive.bsdName.length] == 's' || dev[drive.bsdName.length] == 0)) {
				[self alert:L(@"MSG_358") text:L(@"MSG_359") style:NSAlertStyleCritical buttons:@[ @"OK" ] accessory:nil];
				return;
			}
		}
	}

	/* ISOHybrid images: ask ISO or DD mode (MSG_274-277) */
	if (bootType == BT_IMAGE && IS_DD_BOOTABLE(&report)) {
		if (IS_DD_ONLY(&report)) {
			write_as_image = YES;
		} else {
			NSView* acc = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 380, 56)];
			NSButton* iso = [NSButton radioButtonWithTitle:LF(@"MSG_276", [L(@"MSG_036") UTF8String]) target:nil action:@selector(self)];
			NSButton* dd = [NSButton radioButtonWithTitle:LF(@"MSG_277", [L(@"MSG_095") UTF8String]) target:nil action:@selector(self)];
			iso.frame = NSMakeRect(0, 30, 380, 22);
			dd.frame = NSMakeRect(0, 4, 380, 22);
			iso.state = NSControlStateValueOn;
			[acc addSubview:iso];
			[acc addSubview:dd];
			NSString* msg = LF(@"MSG_275", [L(@"MSG_036") UTF8String], [L(@"MSG_095") UTF8String]);
			if ([self alert:LF(@"MSG_274", "ISOHybrid") text:msg style:NSAlertStyleInformational
				buttons:@[ @"OK", L(@"MSG_007") ] accessory:acc] != NSAlertFirstButtonReturn)
				return;
			write_as_image = dd.state == NSControlStateValueOn;
		}
	}

	/* Windows User Experience dialog */
	if (bootType == BT_IMAGE && !write_as_image && HAS_WINDOWS(&report) && HAS_WININST(&report)) {
		wue = calloc(1, sizeof(wue_options_t));
		uint32_t build = get_windows_build(imagePath.UTF8String, &report, NULL, 0);
		NSModalResponse r = [WUEPanel runForBuild:build imagePath:imagePath report:&report options:wue];
		if (r == NSModalResponseCancel) {
			free(wue);
			return;
		}
		if (wue->flags == 0) {
			free(wue);
			wue = NULL;
		}
	}

	/* Final warning (MSG_003) */
	if ([self alert:@"Rufus" text:LF(@"MSG_003", [drive displayName].UTF8String) style:NSAlertStyleCritical
		buttons:@[ @"OK", L(@"MSG_007") ] accessory:nil] != NSAlertFirstButtonReturn) {
		free(wue);
		return;
	}

	o = calloc(1, sizeof(job_options_t));
	o->boot_type = bootType;
	o->image_path = (bootType == BT_IMAGE) ? strdup(imagePath.UTF8String) : NULL;
	if (bootType == BT_IMAGE) {
		o->report = malloc(sizeof(image_report_t));
		memcpy(o->report, &report, sizeof(report));
	}
	o->write_as_image = write_as_image;
	o->part_style = (part_style_t)partitionPopup.selectedTag;
	o->target = (target_t)targetPopup.selectedTag;
	o->fs = (fs_type_t)fsPopup.selectedTag;
	o->cluster_size = (uint32_t)clusterPopup.selectedTag;
	snprintf(o->label, sizeof(o->label), "%s", labelField.stringValue.UTF8String);
	o->quick_format = quickFormat.state == NSControlStateValueOn;
	o->extended_label = extendedLabel.state == NSControlStateValueOn;
	o->bad_block_passes = (badBlocks.state == NSControlStateValueOn) ? (int)passesPopup.indexOfSelectedItem + 1 : 0;
	o->old_bios_fixes = oldBiosFixes.state == NSControlStateValueOn;
	o->persistence_size = persistenceSize;
	o->wue = wue;
	[self runJob:o drive:drive];
}

- (void)runJob:(job_options_t*)o drive:(RufusDrive*)drive
{
	if (archivePath != nil)
		o->archive_path = strdup(archivePath.UTF8String);
	[self runOperation:^int(rdev_t* dev) {
		int r = run_job(dev, o);
		free((void*)o->archive_path);
		free((void*)o->image_path);
		free(o->report);
		free((void*)o->wue);
		free(o);
		return r;
	} readOnly:NO drive:drive];
}

/* Zero drive (Alt-Z / Ctrl-Alt-Z) */
- (void)zeroDrive:(BOOL)fast
{
	RufusDrive* drive = [self selectedDrive];
	if (drive == nil || busy)
		return;
	if ([self alert:@"Rufus" text:LF(@"MSG_003", [drive displayName].UTF8String) style:NSAlertStyleCritical
		buttons:@[ @"OK", L(@"MSG_007") ] accessory:nil] != NSAlertFirstButtonReturn)
		return;
	job_options_t* o = calloc(1, sizeof(job_options_t));
	o->zero_drive = fast ? 2 : 1;
	[self runJob:o drive:drive];
}

/* Common path for everything that needs exclusive access to a drive */
- (void)runOperation:(int (^)(rdev_t* dev))op readOnly:(BOOL)ro drive:(RufusDrive*)drive
{
	NSString* err = nil;
	int fd;

	busy = YES;
	cancel_requested = false;
	closeButton.title = L(@"MSG_007").uppercaseString;
	[self enableControls];
	[self setProgress:-1 text:L(@"MSG_225")];
	[self startTimer];

	if (!ro && ![[Drives shared] lockDrive:drive error:&err])
		goto fail;
	fd = [[Drives shared] openRawDrive:drive readOnly:ro error:&err];
	if (fd < 0)
		goto fail;
	opDrive = drive;
	opFd = fd;

	{
		int priority = rflags.priority_boost;
		dispatch_async(dispatch_get_global_queue(priority > 0 ? QOS_CLASS_USER_INTERACTIVE :
			priority < 0 ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0), ^{
			rdev_t dev;
			int r = -1;
			if (rdev_attach_fd(&dev, fd, [@"/dev/r" stringByAppendingString:drive.bsdName].UTF8String))
				r = op(&dev);
			rdev_sync(&dev);
			close(fd);
			dispatch_async(dispatch_get_main_queue(), ^{ [self operationDone:r drive:drive readOnly:ro]; });
		});
	}
	return;

fail:
	[[Drives shared] releaseDrive:drive];
	[self operationDone:-2 drive:drive readOnly:ro];
	if (err != nil)
		[self alert:L(@"MSG_042") text:err style:NSAlertStyleCritical buttons:@[ @"OK" ] accessory:nil];
}

- (void)operationDone:(int)r drive:(RufusDrive*)drive readOnly:(BOOL)ro
{
	busy = NO;
	opFd = -1;
	[self stopTimer];
	[[Drives shared] releaseDrive:drive];
	if (!ro)
		[[Drives shared] remountDrive:drive];
	closeButton.title = L(@"MSG_006").uppercaseString;
	[self enableControls];
	if (r == 0) {
		[self setProgress:100 text:L(@"MSG_210")];
		progress.success = YES;
		[[NSSound soundNamed:@"Glass"] play];
		[NSApp requestUserAttention:NSInformationalRequest];
	} else if (cancel_requested) {
		[self setProgress:0 text:L(@"MSG_211")];
	} else if (r != -2) {
		[self setProgress:100 text:L(@"MSG_212")];
		progress.error = YES;
		NSAlert* a = [GlassAlert new];
		a.messageText = L(@"MSG_042");
		a.informativeText = [NSString stringWithFormat:@"%@\n\n%@", L(@"MSG_212"), LogFilePath()];
		a.alertStyle = NSAlertStyleCritical;
		[a beginSheetModalForWindow:self.window completionHandler:nil];
	} else {
		[self setProgress:0 text:L(@"MSG_210")];
	}
	cancel_requested = false;
}

/* ------------------------------------------------------------------------ */
/* Drag & drop an image onto the window                                      */
/* ------------------------------------------------------------------------ */
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return busy ? NSDragOperationNone : NSDragOperationCopy; }

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
	NSURL* url = [NSURL URLFromPasteboard:sender.draggingPasteboard];
	if (url.isFileURL) {
		[self openImageAtPath:url.path];
		return YES;
	}
	return NO;
}

/* ------------------------------------------------------------------------ */
/* Rufus' "cheat mode" keyboard shortcuts (Alt = ⌥ Option, Ctrl = ⌃ or ⌘)    */
/* ------------------------------------------------------------------------ */
- (void)toggle:(bool*)flag name:(NSString*)name
{
	*flag = !*flag;
	NSString* msg = *flag ? LF(@"MSG_250", name.UTF8String) : LF(@"MSG_251", name.UTF8String);
	[self setStatusText:msg];
	uprintf("%s", msg.UTF8String);
}

- (void)installKeyMonitor
{
	[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* e) {
		if (e.window != self.window)
			return e;
		return [self handleKey:e] ? nil : e;
	}];
}

- (BOOL)handleKey:(NSEvent*)e
{
	NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
	BOOL alt = (m & NSEventModifierFlagOption) != 0;
	BOOL ctrl = (m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0;
	/* With Option held, characters are transformed (⌥-Z = Ω), so use the base key */
	NSString* k = [e.charactersIgnoringModifiers lowercaseString];
	if (k.length == 0)
		return NO;
	unichar c = [k characterAtIndex:0];

	if (ctrl && !alt) {
		switch (c) {
		case 'l': [[LogWindow shared] toggle]; return YES;
		case 'a':
			if ([LogWindow shared].window.isKeyWindow)
				return NO;
			[[LogWindow shared] showWindow:nil];
			[[LogWindow shared].window.firstResponder tryToPerform:@selector(selectAll:) with:nil];
			return YES;
		case 'p': {
			bool p = [[NSUserDefaults standardUserDefaults] boolForKey:@"PersistentLog"];
			[self toggle:&p name:L(@"MSG_336")];
			[[NSUserDefaults standardUserDefaults] setBool:p forKey:@"PersistentLog"];
			return YES;
		}
		case 't': [self testHashes]; return YES;
		default: return NO;
		}
	}
	if (!alt)
		return NO;
	if (ctrl) {
		switch (c) {
		case 'z': [self zeroDrive:YES]; return YES;
		case 'd': {
			NSAppearance* cur = NSApp.appearance;
			BOOL dark = [cur.name isEqualToString:NSAppearanceNameDarkAqua];
			NSApp.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameAqua : NSAppearanceNameDarkAqua];
			[self setStatusText:dark ? @"Light mode" : @"Dark mode"];
			return YES;
		}
		case 'e': [self toggle:&rflags.expert_mode name:L(@"MSG_347")]; [self populateBootSelection]; return YES;
		case 'f': [self toggle:&rflags.list_non_usb_removable name:L(@"MSG_287")]; [[Drives shared] refresh]; return YES;
		case 'y': [self setStatusText:[NSString stringWithFormat:@"%@: %@", L(@"MSG_259"), L(@"MSG_247")]]; return YES;
		default: return NO;
		}
	}
	switch (c) {
	case '+': case '=': rflags.priority_boost = MIN(rflags.priority_boost + 1, 2);
		[self setStatusText:LF(@"MSG_318", rflags.priority_boost)]; return YES;
	case '-': rflags.priority_boost = MAX(rflags.priority_boost - 1, -2);
		[self setStatusText:LF(@"MSG_318", rflags.priority_boost)]; return YES;
	case '.': [self toggle:&rflags.usb_debug name:L(@"MSG_270")]; [[Drives shared] refresh]; return YES;
	case ',': [self toggle:&rflags.lock_drive name:L(@"MSG_282")]; return YES;
	case 'a': [self toggle:&rflags.use_rufus_mbr name:L(@"MSG_349")]; return YES;
	case 'b': [self toggle:&rflags.detect_fakes name:L(@"MSG_256")]; return YES;
	case 'c': [self cyclePort]; return YES;
	case 'd': [self deleteFilesDir]; return YES;
	case 'e': [self toggle:&rflags.allow_dual_uefi_bios name:L(@"MSG_266")]; [self bootChanged:nil]; return YES;
	case 'f': listUsbHdd.state = rflags.list_usb_hdd ? NSControlStateValueOff : NSControlStateValueOn;
		[self listUsbHddChanged:nil]; [self setStatusText:rflags.list_usb_hdd ? LF(@"MSG_250", [L(@"MSG_253") UTF8String]) :
			LF(@"MSG_251", [L(@"MSG_253") UTF8String])]; return YES;
	case 'g': [self toggle:&rflags.list_virtual_disks name:L(@"MSG_308")]; [[Drives shared] refresh]; return YES;
	case 'h': [self toggle:&rflags.enable_sha512 name:L(@"MSG_312")];
		[[NSUserDefaults standardUserDefaults] setBool:rflags.enable_sha512 forKey:@"EnableSHA512"]; return YES;
	case 'i': [self toggle:&rflags.enable_iso name:L(@"MSG_262")]; return YES;
	case 'j': [self toggle:&rflags.enable_joliet name:L(@"MSG_257")]; return YES;
	case 'k': [self toggle:&rflags.enable_rockridge name:L(@"MSG_258")]; return YES;
	case 'l': [self toggle:&rflags.force_large_fat32 name:L(@"MSG_254")]; [self updateFileSystems]; return YES;
	case 'm': [self toggle:&rflags.ignore_boot_marker name:L(@"MSG_319")]; return YES;
	case 'n': [self toggle:&rflags.enable_ntfs_compression name:L(@"MSG_260")]; return YES;
	case 'o': [self saveOpticalDisc]; return YES;
	case 'p': [self toggleEsp]; return YES;
	case 'q': [self toggle:&rflags.enable_file_indexing name:L(@"MSG_290")]; return YES;
	case 'r':
		[[NSUserDefaults standardUserDefaults] removePersistentDomainForName:[NSBundle mainBundle].bundleIdentifier];
		[self setStatusText:L(@"MSG_248")];
		return YES;
	case 's': [self toggle:&rflags.size_check name:L(@"MSG_252")]; return YES;
	case 't': [self toggle:&rflags.preserve_timestamps name:L(@"MSG_269")]; return YES;
	case 'u': [self toggle:&rflags.use_proper_size_units name:L(@"MSG_263")];
		[[NSUserDefaults standardUserDefaults] setBool:rflags.use_proper_size_units forKey:@"UseProperSizeUnits"];
		[[Drives shared] refresh]; return YES;
	case 'v': [self setStatusText:@"VDS is a Windows service: not applicable on macOS"]; return YES;
	case 'w': [self toggle:&rflags.list_vmware_disks name:L(@"MSG_265")]; [[Drives shared] refresh]; return YES;
	case 'x': [self setStatusText:@"NoDriveTypeAutorun is a Windows setting: not applicable on macOS"]; return YES;
	case 'y': [self setStatusText:[NSString stringWithFormat:@"%@: %@", L(@"MSG_259"), L(@"MSG_247")]]; return YES;
	case 'z': [self zeroDrive:NO]; return YES;
	default: return NO;
	}
}

/* Ctrl-T: hash self test (TestHashes() in Rufus' debug builds) */
- (void)testHashes
{
	NSString* tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"rufus_hash_test.bin"];
	[@"abc" writeToFile:tmp atomically:YES encoding:NSASCIIStringEncoding error:nil];
	hashes_t h;
	compute_hashes(tmp.fileSystemRepresentation, &h, true);
	BOOL ok = strcmp(h.md5, "900150983cd24fb0d6963f7d28e17f72") == 0 &&
		strcmp(h.sha1, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0 &&
		strcmp(h.sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0 &&
		strncmp(h.sha512, "ddaf35a193617aba", 16) == 0;
	uprintf("Hash test: %s", ok ? "PASS" : "FAIL");
	[self setStatusText:[NSString stringWithFormat:@"Hash test: %@", ok ? @"PASS" : @"FAIL"]];
	[[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
	[self setProgress:0 text:L(@"MSG_210")];
}

/* Alt-C: macOS has no user-space way to power-cycle a USB port, so eject and let the user re-plug */
- (void)cyclePort
{
	RufusDrive* d = [self selectedDrive];
	if (d == nil)
		return;
	uprintf("Alt-C: ejecting %s (USB port cycling is not available on macOS; please re-plug the device)", d.bsdName.UTF8String);
	NSTask* t = [NSTask launchedTaskWithLaunchPath:@"/usr/sbin/diskutil" arguments:@[ @"eject", d.bsdName ]];
	[t waitUntilExit];
	[self setStatusText:@"Device ejected - re-plug it to cycle"];
}

/* Alt-D: delete the directory where Rufus keeps its downloaded files */
- (void)deleteFilesDir
{
	NSString* dir = [[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject]
		stringByAppendingPathComponent:@"Rufus"];
	[self setStatusText:LF(@"MSG_264", dir.UTF8String)];
	[[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
}

/* Alt-P: toggle the GPT ESP of the selected drive to/from Basic Data */
- (void)toggleEsp
{
	RufusDrive* d = [self selectedDrive];
	if (d == nil)
		return;
	[self runOperation:^int(rdev_t* dev) {
		return gpt_toggle_esp(dev) ? 0 : -1;
	} readOnly:NO drive:d];
}

/* Alt-O: save an optical disc to an ISO */
- (void)saveOpticalDisc
{
	NSTask* t = [NSTask new];
	NSPipe* pipe = [NSPipe pipe];
	t.launchPath = @"/usr/bin/drutil";
	t.arguments = @[ @"status" ];
	t.standardOutput = pipe;
	[t launch];
	[t waitUntilExit];
	NSString* out = [[NSString alloc] initWithData:[pipe.fileHandleForReading readDataToEndOfFile] encoding:NSUTF8StringEncoding];
	NSRange r = [out rangeOfString:@"/dev/disk"];
	if (r.location == NSNotFound) {
		[self setStatusText:@"No optical disc found"];
		return;
	}
	NSString* bsd = [[[out substringFromIndex:r.location + 5] componentsSeparatedByCharactersInSet:
		[NSCharacterSet whitespaceAndNewlineCharacterSet]] firstObject];
	RufusDrive* d = [RufusDrive new];
	d.bsdName = bsd;
	d.label = @"Optical";
	NSSavePanel* p = [NSSavePanel savePanel];
	p.nameFieldStringValue = @"disc.iso";
	[p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse resp) {
		if (resp == NSModalResponseOK)
			[self runOperation:^int(rdev_t* dev) { return [self saveDevice:dev toPath:p.URL.path]; } readOnly:YES drive:d];
	}];
}

@end
