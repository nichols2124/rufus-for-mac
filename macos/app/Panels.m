/*
 * Rufus for macOS: secondary dialogs (checksums, Windows customization,
 * about, settings and ISO download)
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#import "Panels.h"
#import "Loc.h"
#import "UI.h"

static NSButton* PanelButton(NSString* title, id target, SEL action)
{
	NSButton* b = [NSButton buttonWithTitle:title target:target action:action];
	UIApplyGlassBezel(b);
	return b;
}

/* ------------------------------------------------------------------------ */
/* Checksums (IDD_HASH)                                                      */
/* ------------------------------------------------------------------------ */
@implementation HashPanel
+ (void)showHashes:(hashes_t)h sha512:(BOOL)sha512 forWindow:(NSWindow*)parent
{
	NSArray* names = sha512 ? @[ @"MD5:", @"SHA1:", @"SHA256:", @"SHA512:" ] : @[ @"MD5:", @"SHA1:", @"SHA256:" ];
	NSArray* values = @[ @(h.md5), @(h.sha1), @(h.sha256), @(h.sha512) ];
	CGFloat w = 560, rowh = 26, y = 8;
	NSView* acc = [[FlippedView alloc] initWithFrame:NSMakeRect(0, 0, w, rowh * names.count + (sha512 ? 30 : 8))];
	for (NSUInteger i = 0; i < names.count; i++) {
		NSTextField* l = UILabel(names[i]);
		l.frame = NSMakeRect(0, y + 3, 70, 20);
		NSTextField* v = [NSTextField textFieldWithString:values[i]];
		v.editable = NO;
		v.selectable = YES;
		v.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
		CGFloat vh = (i == 3) ? 44 : 22;
		if (i == 3)
			v.usesSingleLineMode = NO, v.cell.wraps = YES;
		v.frame = NSMakeRect(72, y, w - 72, vh);
		[acc addSubview:l];
		[acc addSubview:v];
		y += vh + 4;
	}
	NSAlert* a = [GlassAlert new];
	a.messageText = L(@"MSG_314");
	a.accessoryView = acc;
	[a addButtonWithTitle:@"OK"];
	[a beginSheetModalForWindow:parent completionHandler:nil];
}
@end

/* ------------------------------------------------------------------------ */
/* Windows User Experience (the "Customize Windows installation?" dialog)    */
/* ------------------------------------------------------------------------ */
@implementation WUEPanel
+ (NSModalResponse)runForBuild:(uint32_t)build imagePath:(NSString*)path report:(image_report_t*)r options:(wue_options_t*)o
{
	typedef struct { NSString* msg; int flag; BOOL show; } wue_item_t;
	BOOL win11 = (build == 0) || (build >= 22000);
	BOOL expert = rflags.expert_mode;
	wue_item_t items[] = {
		{ L(@"MSG_329"), UNATTEND_SECUREBOOT_TPM_MINRAM, win11 },
		{ L(@"MSG_330"), UNATTEND_NO_ONLINE_ACCOUNT, win11 },
		{ L(@"MSG_333"), UNATTEND_SET_USER, YES },
		{ L(@"MSG_334"), UNATTEND_DUPLICATE_LOCALE, YES },
		{ L(@"MSG_331"), UNATTEND_NO_DATA_COLLECTION, YES },
		{ L(@"MSG_335"), UNATTEND_DISABLE_BITLOCKER, YES },
		{ L(@"MSG_324"), UNATTEND_QOL_ENHANCEMENTS, win11 },
		{ L(@"MSG_323"), UNATTEND_APPLY_SKUSIPOLICY, expert },
		{ L(@"MSG_346"), UNATTEND_FORCE_S_MODE, expert },
		{ L(@"MSG_355"), UNATTEND_SILENT_INSTALL, expert },
	};
	const int n = sizeof(items) / sizeof(items[0]);
	NSInteger saved = [[NSUserDefaults standardUserDefaults] objectForKey:@"WUEFlags"] ?
		[[NSUserDefaults standardUserDefaults] integerForKey:@"WUEFlags"] : UNATTEND_DEFAULT_SELECTION_MASK | UNATTEND_SET_USER |
		UNATTEND_DUPLICATE_LOCALE | UNATTEND_NO_DATA_COLLECTION;
	NSMutableArray<NSButton*>* boxes = [NSMutableArray array];
	NSTextField* user = nil;
	NSPopUpButton* editions = nil;
	CGFloat w = 520, y = 0;
	FlippedView* acc = [[FlippedView alloc] initWithFrame:NSMakeRect(0, 0, w, 10)];
	int i;

	for (i = 0; i < n; i++) {
		if (!items[i].show)
			continue;
		NSButton* b = UICheckbox(items[i].msg, nil, nil);
		b.tag = items[i].flag;
		b.state = (saved & items[i].flag) ? NSControlStateValueOn : NSControlStateValueOff;
		if (items[i].flag == UNATTEND_SILENT_INSTALL)
			b.state = NSControlStateValueOff;   /* never pre-selected */
		CGFloat bw = w;
		if (items[i].flag == UNATTEND_SET_USER)
			bw = w - 180;
		if (items[i].flag == UNATTEND_SILENT_INSTALL)
			bw = w - 240;
		b.frame = NSMakeRect(0, y, bw, 22);
		[acc addSubview:b];
		[boxes addObject:b];
		if (items[i].flag == UNATTEND_SET_USER) {
			user = [NSTextField textFieldWithString:[[NSUserDefaults standardUserDefaults] stringForKey:@"WUEUser"] ?: NSUserName()];
			user.frame = NSMakeRect(w - 172, y, 172, 22);
			[acc addSubview:user];
		}
		if (items[i].flag == UNATTEND_SILENT_INSTALL) {
			char names[32][128];
			int ne = get_windows_editions(path.UTF8String, r, names, 32);
			editions = UIPopup(nil, nil);
			for (int e = 0; e < ne; e++)
				[editions addItemWithTitle:@(names[e])];
			if (ne == 0)
				[editions addItemWithTitle:@"1"];
			editions.frame = NSMakeRect(w - 232, y - 1, 232, 24);
			[acc addSubview:editions];
		}
		y += 26;
	}
	acc.frame = NSMakeRect(0, 0, w, y);

	NSAlert* a = [GlassAlert new];
	a.messageText = L(@"MSG_327");
	a.informativeText = L(@"MSG_328");
	a.accessoryView = acc;
	[a addButtonWithTitle:@"OK"];
	[a addButtonWithTitle:L(@"MSG_007")];
	if ([a runModal] != NSAlertFirstButtonReturn)
		return NSModalResponseCancel;

	memset(o, 0, sizeof(*o));
	for (NSButton* b in boxes)
		if (b.state == NSControlStateValueOn)
			o->flags |= (int)b.tag;
	if (user != nil) {
		snprintf(o->username, sizeof(o->username), "%s", user.stringValue.UTF8String);
		[[NSUserDefaults standardUserDefaults] setObject:user.stringValue forKey:@"WUEUser"];
	}
	if ((o->flags & UNATTEND_SET_USER) && o->username[0] == 0)
		o->flags &= ~UNATTEND_SET_USER;
	o->edition_index = (editions != nil) ? (int)editions.indexOfSelectedItem + 1 : 1;
	/* Regional options, from the Mac's settings (Windows wants e.g. "en-US") */
	NSLocale* loc = [NSLocale currentLocale];
	NSString* lang = [loc objectForKey:NSLocaleLanguageCode] ?: @"en";
	NSString* region = [loc objectForKey:NSLocaleCountryCode] ?: @"US";
	snprintf(o->locale, sizeof(o->locale), "%s-%s", lang.UTF8String, region.UTF8String);
	snprintf(o->input_locale, sizeof(o->input_locale), "%s", o->locale);
	[[NSUserDefaults standardUserDefaults] setInteger:(o->flags & ~UNATTEND_SILENT_INSTALL) forKey:@"WUEFlags"];

	if (o->flags & UNATTEND_SILENT_INSTALL) {
		NSAlert* w2 = [GlassAlert new];
		w2.messageText = L(@"MSG_355");
		w2.informativeText = L(@"MSG_356");
		w2.alertStyle = NSAlertStyleCritical;
		[w2 addButtonWithTitle:@"OK"];
		[w2 addButtonWithTitle:L(@"MSG_007")];
		if ([w2 runModal] != NSAlertFirstButtonReturn)
			return NSModalResponseCancel;
	}
	return NSModalResponseOK;
}
@end

/* ------------------------------------------------------------------------ */
/* About / License                                                           */
/* ------------------------------------------------------------------------ */
@implementation AboutPanel
+ (void)showForWindow:(NSWindow*)parent
{
	NSString* blurb = [NSString stringWithFormat:
		@"%@\n%@\nmacOS port %s\n\n"
		@"Copyright © 2011-2026 Pete Batard and contributors\nhttps://rufus.ie\n\n"
		@"macOS port: native AppKit interface, DiskArbitration device handling, FatFs based FAT/exFAT, "
		@"bundled ntfs-3g for NTFS.\n\n"
		@"%@\n"
		@"• libcdio (ISO9660/UDF), GPLv3+\n"
		@"• Bled / Busybox (decompression), GPLv2+\n"
		@"• ms-sys (boot records), GPLv2+\n"
		@"• Syslinux (libfat, libinstaller), GPLv2+\n"
		@"• e2fsprogs libext2fs, GPLv2 / LGPLv2\n"
		@"• ntfs-3g (libntfs-3g, mkntfs), GPLv2+ / LGPLv2+\n"
		@"• FatFs by ChaN, BSD-1-Clause\n"
		@"• FreeDOS, GPL\n"
		@"• UEFI:NTFS, GPLv2+\n\n(claude code slop)",
		L(@"MSG_174"), @"Rufus " @RUFUS_MAC_VERSION, RUFUS_MAC_VERSION, L(@"MSG_178")];
	NSAlert* a = [GlassAlert new];
	a.messageText = L(@"IDD_ABOUTBOX");
	a.informativeText = blurb;
	a.icon = [NSApp applicationIconImage];
	[a addButtonWithTitle:@"OK"];
	[a addButtonWithTitle:L(@"IDC_ABOUT_LICENSE")];
	[a beginSheetModalForWindow:parent completionHandler:^(NSModalResponse r) {
		if (r == NSAlertSecondButtonReturn)
			[self showLicenseForWindow:parent];
	}];
}

+ (void)showLicenseForWindow:(NSWindow*)parent
{
	NSString* path = [[NSBundle mainBundle] pathForResource:@"LICENSE" ofType:@"txt" inDirectory:@"res"];
	NSString* text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @"GNU GPL v3";
	NSScrollView* sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 560, 360)];
	NSTextView* tv = [[NSTextView alloc] initWithFrame:sv.bounds];
	tv.string = text;
	tv.editable = NO;
	tv.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
	sv.documentView = tv;
	sv.hasVerticalScroller = YES;
	NSAlert* a = [GlassAlert new];
	a.messageText = L(@"IDD_LICENSE");
	a.accessoryView = sv;
	[a addButtonWithTitle:L(@"IDCANCEL")];
	[a beginSheetModalForWindow:parent completionHandler:nil];
}
@end

/* ------------------------------------------------------------------------ */
/* Settings (IDD_UPDATE_POLICY)                                              */
/* ------------------------------------------------------------------------ */
@implementation SettingsPanel
+ (void)showForWindow:(NSWindow*)parent
{
	FlippedView* acc = [[FlippedView alloc] initWithFrame:NSMakeRect(0, 0, 380, 96)];
	NSTextField* l1 = UILabel(L(@"IDS_UPDATE_FREQUENCY_TXT"));
	NSPopUpButton* freq = UIPopup(nil, nil);
	NSButton* betas = UICheckbox(L(@"IDS_INCLUDE_BETAS_TXT"), nil, nil);
	NSTextField* ver = UILabel([NSString stringWithFormat:@"Rufus for Mac %s", RUFUS_MAC_VERSION]);
	[freq addItemsWithTitles:@[ L(@"MSG_013"), L(@"MSG_014"), L(@"MSG_015"), L(@"MSG_016") ]];
	[freq selectItemAtIndex:[[NSUserDefaults standardUserDefaults] integerForKey:@"UpdateFrequency"]];
	betas.state = [[NSUserDefaults standardUserDefaults] boolForKey:@"IncludeBetas"] ? NSControlStateValueOn : NSControlStateValueOff;
	l1.frame = NSMakeRect(0, 4, 170, 20);
	freq.frame = NSMakeRect(176, 0, 204, 26);
	betas.frame = NSMakeRect(0, 36, 380, 22);
	ver.frame = NSMakeRect(0, 70, 380, 20);
	ver.textColor = NSColor.secondaryLabelColor;
	for (NSView* v in @[ l1, freq, betas, ver ])
		[acc addSubview:v];
	NSAlert* a = [GlassAlert new];
	a.messageText = L(@"IDD_UPDATE_POLICY");
	a.accessoryView = acc;
	[a addButtonWithTitle:L(@"IDCANCEL")];
	[a addButtonWithTitle:L(@"IDC_CHECK_NOW")];
	[a beginSheetModalForWindow:parent completionHandler:^(NSModalResponse r) {
		[[NSUserDefaults standardUserDefaults] setInteger:freq.indexOfSelectedItem forKey:@"UpdateFrequency"];
		[[NSUserDefaults standardUserDefaults] setBool:betas.state == NSControlStateValueOn forKey:@"IncludeBetas"];
		if (r == NSAlertSecondButtonReturn)
			[self checkNow:parent];
	}];
}

#define RUFUS_MAC_REPO @"nichols2124/rufus-for-mac"

/* Compare dotted versions ("v4.15", "4.15.1"): <0, 0 or >0 */
static int compare_versions(NSString* a, NSString* b)
{
	NSCharacterSet* nondigit = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789."] invertedSet];
	NSArray* pa = [[a stringByTrimmingCharactersInSet:nondigit] componentsSeparatedByString:@"."];
	NSArray* pb = [[b stringByTrimmingCharactersInSet:nondigit] componentsSeparatedByString:@"."];
	for (NSUInteger i = 0; i < MAX(pa.count, pb.count); i++) {
		NSInteger x = (i < pa.count) ? [pa[i] integerValue] : 0, y = (i < pb.count) ? [pb[i] integerValue] : 0;
		if (x != y)
			return (x < y) ? -1 : 1;
	}
	return 0;
}

/* Check this port's GitHub releases. 'quiet' = automatic check: only speak up if there's an update. */
+ (void)checkForUpdates:(NSWindow*)parent quiet:(BOOL)quiet
{
	BOOL betas = [[NSUserDefaults standardUserDefaults] boolForKey:@"IncludeBetas"];
	/* /releases/latest skips pre-releases; the full list includes them (newest first) */
	NSString* api = betas ? @"https://api.github.com/repos/" RUFUS_MAC_REPO @"/releases?per_page=10" :
		@"https://api.github.com/repos/" RUFUS_MAC_REPO @"/releases/latest";
	NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:api]];
	[req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
	req.timeoutInterval = 20;
	uprintf("%s", [L(@"MSG_243") UTF8String]);
	[[NSUserDefaults standardUserDefaults] setObject:[NSDate date] forKey:@"LastUpdateCheck"];

	[[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
		id j = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
		NSDictionary* rel = [j isKindOfClass:[NSArray class]] ? [j firstObject] : j;
		NSString* tag = [rel isKindOfClass:[NSDictionary class]] ? rel[@"tag_name"] : nil;
		NSString* page = [rel isKindOfClass:[NSDictionary class]] ? rel[@"html_url"] : nil;
		NSString* dmg = nil;
		for (NSDictionary* asset in ([rel isKindOfClass:[NSDictionary class]] ? rel[@"assets"] : nil))
			if ([asset[@"name"] hasSuffix:@".dmg"])
				dmg = asset[@"browser_download_url"];
		BOOL newer = (tag != nil) && compare_versions(tag, @RUFUS_MAC_VERSION) > 0;
		if (tag == nil)
			uprintf("%s", [(err != nil ? L(@"MSG_244") : L(@"MSG_245")) UTF8String]);
		else
			uprintf("Latest release: %s (this version: %s)", tag.UTF8String, RUFUS_MAC_VERSION);
		if (quiet && !newer)
			return;
		dispatch_async(dispatch_get_main_queue(), ^{
			NSAlert* a = [GlassAlert new];
			a.messageText = newer ? L(@"MSG_246") : L(@"IDD_NEW_VERSION");
			if (tag == nil)
				a.informativeText = (err != nil) ? L(@"MSG_244") : L(@"MSG_245");
			else
				a.informativeText = [NSString stringWithFormat:@"Rufus for Mac %s\n%@ %@", RUFUS_MAC_VERSION,
					newer ? L(@"IDS_NEW_VERSION_AVAIL_TXT") : L(@"MSG_247"), newer ? tag : @""];
			if (newer) {
				[a addButtonWithTitle:L(@"IDC_DOWNLOAD")];
				[a addButtonWithTitle:L(@"IDCANCEL")];
			} else {
				[a addButtonWithTitle:@"OK"];
			}
			void (^done)(NSModalResponse) = ^(NSModalResponse r) {
				if (newer && r == NSAlertFirstButtonReturn)
					[[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:dmg ?: page]];
			};
			if (parent != nil)
				[a beginSheetModalForWindow:parent completionHandler:done];
			else
				done([a runModal]);
		});
	}] resume];
}

+ (void)checkNow:(NSWindow*)parent
{
	[self checkForUpdates:parent quiet:NO];
}

/* Automatic check at startup, following the "Check for updates" frequency (Disabled/Daily/Weekly/Monthly) */
+ (void)checkIfDue:(NSWindow*)parent
{
	static const double interval[] = { 0, 86400, 7 * 86400, 30 * 86400 };
	NSInteger f = [[NSUserDefaults standardUserDefaults] integerForKey:@"UpdateFrequency"];
	NSDate* last = [[NSUserDefaults standardUserDefaults] objectForKey:@"LastUpdateCheck"];
	if (f <= 0 || f > 3)
		return;
	if (last != nil && -[last timeIntervalSinceNow] < interval[f])
		return;
	[self checkForUpdates:parent quiet:YES];
}
@end

/* ------------------------------------------------------------------------ */
/* ISO download - native version of Fido (https://github.com/pbatard/Fido)   */
/* ------------------------------------------------------------------------ */
typedef struct { const char* name; int ids[2]; } fido_edition_t;
typedef struct { const char* name; fido_edition_t editions[4]; } fido_release_t;
typedef struct { const char* name; const char* page; fido_release_t releases[12]; } fido_version_t;

/* Same data as the $WindowsVersions table of Fido.ps1 */
static const fido_version_t fido_versions[] = {
	{ "Windows 11", "windows11", {
		{ "25H2 v2 (Build 26200.8037 - 2026.03)", { { "Windows 11 Home/Pro/Edu", { 3321, 3324 } },
			{ "Windows 11 Home China ", { 3322, 3325 } }, { "Windows 11 Pro China ", { 3323, 3326 } } } } } },
	{ "Windows 10", "Windows10ISO", {
		{ "22H2 v1 (Build 19045.2965 - 2023.05)", { { "Windows 10 Home/Pro/Edu", { 2618, 0 } },
			{ "Windows 10 Home China ", { 2378, 0 } } } } } },
	{ "UEFI Shell 2.2", "UEFI_SHELL 2.2", {
		{ "26H1 (edk2-stable202602)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "25H2 (edk2-stable202511)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "25H1 (edk2-stable202505)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "24H2 (edk2-stable202411)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "24H1 (edk2-stable202405)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "23H2 (edk2-stable202311)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "23H1 (edk2-stable202305)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "22H2 (edk2-stable202211)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "22H1 (edk2-stable202205)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "21H2 (edk2-stable202108)", { { "Release", { 0 } }, { "Debug", { 1 } } } },
		{ "21H1 (edk2-stable202105)", { { "Release", { 0 } }, { "Debug", { 1 } } } } } },
};

#define FIDO_ORG_ID      @"y6jn8c31"
#define FIDO_PROFILE_ID  @"606624d44113"
#define FIDO_INSTANCE_ID @"560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175"

@interface FidoPanel () <NSURLSessionDownloadDelegate>
@end

@implementation FidoPanel {
	NSWindow* sheet;
	NSWindow* parent;
	NSPopUpButton *version, *release, *edition, *language, *arch;
	NSTextField *lblVersion, *lblRelease, *lblEdition, *lblLanguage, *lblArch, *status;
	NSButton *next, *back, *browser;
	NSProgressIndicator* spinner;
	int step;
	NSArray<NSDictionary*>* languages;   /* { name, display, skus: [{session, sku}] } */
	NSArray<NSDictionary*>* links;       /* { arch, url } */
	NSMutableArray<NSString*>* sessions;
	NSURLSession* dlSession;
	NSURLSessionDownloadTask* dlTask;
	NSString* dest;
	void (^done)(NSString*);
}

static FidoPanel* current_fido;

+ (void)showForWindow:(NSWindow*)parentWindow completion:(void (^)(NSString*))completion
{
	current_fido = [FidoPanel new];
	[current_fido showForWindow:parentWindow completion:completion];
}

- (NSTextField*)rowLabel:(NSString*)t y:(CGFloat)y in:(NSView*)v
{
	NSTextField* l = UILabel(t);
	l.frame = NSMakeRect(20, y + 4, 110, 20);
	l.alignment = NSTextAlignmentRight;
	[v addSubview:l];
	return l;
}

- (NSPopUpButton*)rowPopup:(CGFloat)y in:(NSView*)v
{
	NSPopUpButton* p = UIPopup(self, @selector(popupChanged:));
	p.frame = NSMakeRect(138, y, 330, 26);
	[v addSubview:p];
	return p;
}

- (void)showForWindow:(NSWindow*)parentWindow completion:(void (^)(NSString*))completion
{
	parent = parentWindow;
	done = [completion copy];
	sessions = [NSMutableArray array];
	sheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 490, 280) styleMask:NSWindowStyleMaskTitled
		backing:NSBackingStoreBuffered defer:NO];
	FlippedView* v = [[FlippedView alloc] initWithFrame:NSMakeRect(0, 0, 490, 280)];
	sheet.contentView = v;
	NSTextField* title = UIHeader(LF(@"MSG_085", "ISO"));
	title.frame = NSMakeRect(20, 14, 450, 24);
	[v addSubview:title];
	lblVersion = [self rowLabel:@"Version" y:50 in:v];   version = [self rowPopup:50 in:v];
	lblRelease = [self rowLabel:@"Release" y:82 in:v];   release = [self rowPopup:82 in:v];
	lblEdition = [self rowLabel:@"Edition" y:114 in:v];  edition = [self rowPopup:114 in:v];
	lblLanguage = [self rowLabel:@"Language" y:146 in:v]; language = [self rowPopup:146 in:v];
	lblArch = [self rowLabel:@"Architecture" y:178 in:v]; arch = [self rowPopup:178 in:v];
	status = UILabel(@"");
	status.frame = NSMakeRect(20, 212, 450, 18);
	status.textColor = NSColor.secondaryLabelColor;
	[v addSubview:status];
	spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 244, 20, 20)];
	spinner.style = NSProgressIndicatorStyleSpinning;
	spinner.displayedWhenStopped = NO;
	[v addSubview:spinner];
	browser = UICheckbox(@"Download using a browser", nil, nil);
	browser.frame = NSMakeRect(46, 244, 200, 22);
	[v addSubview:browser];
	back = PanelButton(L(@"MSG_007"), self, @selector(backPressed:));
	next = PanelButton(@"Continue", self, @selector(nextPressed:));
	next.keyEquivalent = @"\r";
	back.frame = NSMakeRect(258, 238, 100, 32);
	next.frame = NSMakeRect(366, 238, 104, 32);
	[v addSubview:back];
	[v addSubview:next];

	UIGlassifyWindow(sheet);
	for (size_t i = 0; i < sizeof(fido_versions) / sizeof(fido_versions[0]); i++)
		[version addItemWithTitle:@(fido_versions[i].name)];
	step = 0;
	[self updateStep];
	[parent beginSheet:sheet completionHandler:nil];
}

- (void)updateStep
{
	NSArray* popups = @[ version, release, edition, language, arch ];
	NSArray* labels = @[ lblVersion, lblRelease, lblEdition, lblLanguage, lblArch ];
	for (NSUInteger i = 0; i < popups.count; i++) {
		[popups[i] setHidden:(NSInteger)i > step];
		[labels[i] setHidden:(NSInteger)i > step];
		[popups[i] setEnabled:(NSInteger)i == step];
	}
	next.title = (step == 4) ? L(@"MSG_040") : @"Continue";
	back.title = (step == 0) ? L(@"MSG_007") : @"Back";
	browser.hidden = step != 4;
}

- (void)popupChanged:(id)sender { }

- (void)busy:(BOOL)b text:(NSString*)t
{
	if (b) [spinner startAnimation:nil]; else [spinner stopAnimation:nil];
	next.enabled = !b;
	status.stringValue = t ?: @"";
}

- (void)fail:(NSString*)msg
{
	dispatch_async(dispatch_get_main_queue(), ^{
		[self busy:NO text:[NSString stringWithFormat:@"Error: %@", msg]];
		uprintf("Fido: %s", msg.UTF8String);
	});
}

- (void)backPressed:(id)sender
{
	if (dlTask != nil) {
		[dlTask cancel];
		return;
	}
	if (step == 0) {
		[parent endSheet:sheet];
		current_fido = nil;
		return;
	}
	step--;
	[self updateStep];
}

- (const fido_version_t*)ver { return &fido_versions[version.indexOfSelectedItem]; }
- (BOOL)isShell { return strncmp([self ver]->page, "UEFI_SHELL", 10) == 0; }

- (void)nextPressed:(id)sender
{
	const fido_version_t* v = [self ver];
	switch (step) {
	case 0:
		[release removeAllItems];
		for (int i = 0; i < 12 && v->releases[i].name != NULL; i++)
			[release addItemWithTitle:@(v->releases[i].name)];
		step = 1;
		break;
	case 1:
		[edition removeAllItems];
		for (int i = 0; i < 4 && v->releases[release.indexOfSelectedItem].editions[i].name != NULL; i++)
			[edition addItemWithTitle:@(v->releases[release.indexOfSelectedItem].editions[i].name)];
		step = 2;
		break;
	case 2:
		[self fetchLanguages];
		return;
	case 3:
		[self fetchLinks];
		return;
	case 4:
		[self startDownload];
		return;
	}
	[self updateStep];
}

/* Synchronous HTTP helper, used from a background queue */
- (NSData*)get:(NSString*)url referer:(NSString*)referer error:(NSString**)error
{
	__block NSData* result = nil;
	__block NSString* err = nil;
	dispatch_semaphore_t sem = dispatch_semaphore_create(0);
	NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
	req.timeoutInterval = 30;
	[req setValue:@"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15"
		forHTTPHeaderField:@"User-Agent"];
	if (referer != nil)
		[req setValue:referer forHTTPHeaderField:@"Referer"];
	[[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData* d, NSURLResponse* r, NSError* e) {
		if (e != nil)
			err = e.localizedDescription;
		else
			result = d;
		dispatch_semaphore_signal(sem);
	}] resume];
	dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
	if (error)
		*error = err;
	return result;
}

- (void)fetchLanguages
{
	const fido_version_t* v = [self ver];
	const fido_edition_t* e = &v->releases[release.indexOfSelectedItem].editions[edition.indexOfSelectedItem];
	if ([self isShell]) {
		[language removeAllItems];
		[language addItemWithTitle:@"English (US)"];
		step = 3;
		[self updateStep];
		return;
	}
	[self busy:YES text:@"Querying Microsoft servers..."];
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		NSMutableArray* langs = [NSMutableArray array];
		NSMutableDictionary* by_name = [NSMutableDictionary dictionary];
		NSString* err = nil;
		[self->sessions removeAllObjects];
		for (int s = 0; s < 2 && e->ids[s] != 0; s++) {
			NSString* sid = [[NSUUID UUID] UUIDString].lowercaseString;
			[self->sessions addObject:sid];
			/* 1. Whitelist the session id */
			[self get:[NSString stringWithFormat:@"https://vlscppe.microsoft.com/tags?org_id=%@&session_id=%@", FIDO_ORG_ID, sid]
				referer:nil error:&err];
			if (err) { [self fail:err]; return; }
			/* 2. ov-df challenge: fetch mdt.js for w & rticks, then reply */
			NSData* js = [self get:[NSString stringWithFormat:@"https://ov-df.microsoft.com/mdt.js?instanceId=%@&PageId=si&session_id=%@",
				FIDO_INSTANCE_ID, sid] referer:nil error:&err];
			NSString* jss = js ? [[NSString alloc] initWithData:js encoding:NSUTF8StringEncoding] : nil;
			NSString *w = nil, *rticks = nil;
			NSRegularExpression* rw = [NSRegularExpression regularExpressionWithPattern:@"[?&]w=([A-F0-9]+)" options:0 error:nil];
			NSRegularExpression* rr = [NSRegularExpression regularExpressionWithPattern:@"rticks=\\\"\\+?(\\d+)" options:0 error:nil];
			NSTextCheckingResult* m1 = jss ? [rw firstMatchInString:jss options:0 range:NSMakeRange(0, jss.length)] : nil;
			NSTextCheckingResult* m2 = jss ? [rr firstMatchInString:jss options:0 range:NSMakeRange(0, jss.length)] : nil;
			if (m1) w = [jss substringWithRange:[m1 rangeAtIndex:1]];
			if (m2) rticks = [jss substringWithRange:[m2 rangeAtIndex:1]];
			if (w == nil || rticks == nil) { [self fail:@"Could not extract ov-df data"]; return; }
			long long now_ms = (long long)([[NSDate date] timeIntervalSince1970] * 1000);
			[self get:[NSString stringWithFormat:@"https://ov-df.microsoft.com/?session_id=%@&CustomerId=%@&PageId=si&w=%@&mdt=%lld&rticks=%@",
				sid, FIDO_INSTANCE_ID, w, now_ms, rticks] referer:nil error:&err];
			if (err) { [self fail:err]; return; }
			/* 3. Languages (SKUs) for this edition */
			NSDictionary* j = nil;
			for (int attempt = 0; attempt < 3 && j == nil; attempt++) {
				if (attempt > 0)
					sleep(2);
				NSData* d = [self get:[NSString stringWithFormat:@"https://www.microsoft.com/software-download-connector/api/"
					"getskuinformationbyproductedition?profile=%@&productEditionId=%d&SKU=undefined&friendlyFileName=undefined"
					"&Locale=en-US&sessionID=%@", FIDO_PROFILE_ID, e->ids[s], sid] referer:nil error:&err];
				j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
				if (![j isKindOfClass:[NSDictionary class]] || j[@"Errors"] != nil || ![j[@"Skus"] isKindOfClass:[NSArray class]])
					j = nil;
			}
			if (j == nil) { [self fail:@"Could not retrieve languages from server"]; return; }
			for (NSDictionary* sku in j[@"Skus"]) {
				NSString* name = sku[@"Language"];
				NSMutableDictionary* l = by_name[name];
				if (l == nil) {
					l = [@{ @"name": name, @"display": sku[@"LocalizedLanguage"] ?: name, @"skus": [NSMutableArray array] } mutableCopy];
					by_name[name] = l;
					[langs addObject:l];
				}
				[l[@"skus"] addObject:@{ @"session": @(s), @"sku": [NSString stringWithFormat:@"%@", sku[@"Id"]] }];
			}
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			self->languages = langs;
			[self->language removeAllItems];
			NSString* pref = [[NSLocale preferredLanguages] firstObject] ?: @"en-US";
			NSInteger sel = 0;
			for (NSUInteger i = 0; i < langs.count; i++) {
				[self->language addItemWithTitle:langs[i][@"display"]];
				NSString* n = langs[i][@"name"];
				if (([pref hasPrefix:@"en-US"] && [n isEqualToString:@"English"]) ||
					(![pref hasPrefix:@"en"] && [n.lowercaseString hasPrefix:[[NSLocale localeWithLocaleIdentifier:@"en"]
						localizedStringForLanguageCode:[pref substringToIndex:2]].lowercaseString ?: @"~"]))
					sel = i;
			}
			[self->language selectItemAtIndex:sel];
			[self busy:NO text:@""];
			self->step = 3;
			[self updateStep];
		});
	});
}

- (void)fetchLinks
{
	const fido_version_t* v = [self ver];
	[self busy:YES text:@"Retrieving download links..."];
	if ([self isShell]) {
		NSString* tag = [[@(v->releases[release.indexOfSelectedItem].name) componentsSeparatedByString:@" "] firstObject];
		NSString* url = [NSString stringWithFormat:@"https://github.com/pbatard/UEFI-Shell/releases/download/%@/UEFI-Shell-2.2-%@-%@.iso",
			tag, tag, edition.indexOfSelectedItem == 0 ? @"RELEASE" : @"DEBUG"];
		links = @[ @{ @"arch": @"x64, ia32, aa64, arm, riscv64, loongarch64", @"url": url } ];
		[self showLinks];
		return;
	}
	NSDictionary* lang = languages[language.indexOfSelectedItem];
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		NSMutableArray* found = [NSMutableArray array];
		static NSString* archs[] = { @"x86", @"x64", @"ARM64" };
		for (NSDictionary* entry in lang[@"skus"]) {
			NSString* err = nil;
			NSString* sid = self->sessions[[entry[@"session"] intValue]];
			NSData* d = [self get:[NSString stringWithFormat:@"https://www.microsoft.com/software-download-connector/api/"
				"GetProductDownloadLinksBySku?profile=%@&productEditionId=undefined&SKU=%@&friendlyFileName=undefined"
				"&Locale=en-US&sessionID=%@", FIDO_PROFILE_ID, entry[@"sku"], sid]
				referer:@"https://www.microsoft.com/software-download/windows11" error:&err];
			NSDictionary* j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
			if (![j isKindOfClass:[NSDictionary class]]) { [self fail:err ?: @"Could not retrieve architectures from server"]; return; }
			if ([j[@"Errors"] isKindOfClass:[NSArray class]] && [j[@"Errors"] count] > 0) {
				NSDictionary* e0 = j[@"Errors"][0];
				[self fail:[e0[@"Type"] intValue] == 9 ?
					[NSString stringWithFormat:@"Microsoft blocked the download for this session (715-123130). Session: %@", sid] :
					[NSString stringWithFormat:@"%@", e0[@"Value"]]];
				return;
			}
			for (NSDictionary* opt in j[@"ProductDownloadOptions"]) {
				int t = [opt[@"DownloadType"] intValue];
				[found addObject:@{ @"arch": (t >= 0 && t <= 2) ? archs[t] : @"Unknown", @"url": opt[@"Uri"] ?: @"" }];
			}
		}
		if (found.count == 0) { [self fail:@"Could not retrieve ISO download links"]; return; }
		dispatch_async(dispatch_get_main_queue(), ^{
			self->links = found;
			[self showLinks];
		});
	});
}

- (void)showLinks
{
	[arch removeAllItems];
	NSInteger sel = 0;
	for (NSUInteger i = 0; i < links.count; i++) {
		[arch addItemWithTitle:links[i][@"arch"]];
		/* Pick the Mac's architecture by default (useful for VMs) */
#if defined(__arm64__)
		if ([links[i][@"arch"] isEqualToString:@"ARM64"]) sel = i;
#else
		if ([links[i][@"arch"] isEqualToString:@"x64"]) sel = i;
#endif
	}
	[arch selectItemAtIndex:sel];
	[self busy:NO text:@""];
	step = 4;
	[self updateStep];
}

- (void)startDownload
{
	NSURL* url = [NSURL URLWithString:links[arch.indexOfSelectedItem][@"url"]];
	if (browser.state == NSControlStateValueOn) {
		[[NSWorkspace sharedWorkspace] openURL:url];
		[parent endSheet:sheet];
		current_fido = nil;
		return;
	}
	NSSavePanel* p = [NSSavePanel savePanel];
	p.nameFieldStringValue = [url.lastPathComponent componentsSeparatedByString:@"?"].firstObject ?: @"windows.iso";
	p.directoryURL = [[NSFileManager defaultManager] URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask].firstObject;
	[p beginSheetModalForWindow:sheet completionHandler:^(NSModalResponse r) {
		if (r != NSModalResponseOK)
			return;
		self->dest = p.URL.path;
		self->dlSession = [NSURLSession sessionWithConfiguration:[NSURLSessionConfiguration defaultSessionConfiguration]
			delegate:self delegateQueue:[NSOperationQueue mainQueue]];
		self->dlTask = [self->dlSession downloadTaskWithURL:url];
		[self->dlTask resume];
		uprintf("%s", [LF(@"MSG_241", url.absoluteString.UTF8String) UTF8String]);
		[self busy:YES text:LF(@"MSG_085", p.URL.lastPathComponent.UTF8String)];
		self->back.title = L(@"MSG_007");
	}];
}

- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t didWriteData:(int64_t)w
	totalBytesWritten:(int64_t)total totalBytesExpectedToWrite:(int64_t)expected
{
	if (expected > 0)
		status.stringValue = [NSString stringWithFormat:@"%@ %.1f%% (%@ / %@)", LF(@"MSG_085", dest.lastPathComponent.UTF8String),
			100.0 * total / expected, [NSByteCountFormatter stringFromByteCount:total countStyle:NSByteCountFormatterCountStyleFile],
			[NSByteCountFormatter stringFromByteCount:expected countStyle:NSByteCountFormatterCountStyleFile]];
}

- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t didFinishDownloadingToURL:(NSURL*)location
{
	[[NSFileManager defaultManager] removeItemAtPath:dest error:nil];
	NSError* err = nil;
	if (![[NSFileManager defaultManager] moveItemAtURL:location toURL:[NSURL fileURLWithPath:dest] error:&err])
		uprintf("Could not save download: %s", err.localizedDescription.UTF8String);
}

- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t didCompleteWithError:(NSError*)error
{
	NSString* path = (error == nil && [[NSFileManager defaultManager] fileExistsAtPath:dest]) ? dest : nil;
	dlTask = nil;
	[dlSession finishTasksAndInvalidate];
	if (error != nil)
		uprintf("%s: %s", [L(@"MSG_242") UTF8String], error.localizedDescription.UTF8String);
	[parent endSheet:sheet];
	if (done != nil)
		done(path);
	current_fido = nil;
}
@end
