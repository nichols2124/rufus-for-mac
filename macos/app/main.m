/*
 * Rufus for macOS: application entry point
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>
#include <sys/sysctl.h>
#import "MainWindow.h"
#import "Drives.h"
#import "Loc.h"
#import "UI.h"
#import "Panels.h"
#include "rufus_core.h"

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (strong) MainWindow* main;
@end

static void app_log(const char* msg)
{
	LogFileWrite(msg);
	[[LogWindow shared] append:[NSString stringWithUTF8String:msg] ?: @"?"];
}

@implementation AppDelegate

- (NSMenuItem*)item:(NSString*)title action:(SEL)action key:(NSString*)key target:(id)target
{
	NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
	it.target = target;
	return it;
}

- (void)buildMenu
{
	NSMenu* bar = [NSMenu new];
	NSMenu* app = [[NSMenu alloc] initWithTitle:@"Rufus"];
	[app addItemWithTitle:L(@"IDD_ABOUTBOX") action:@selector(about:) keyEquivalent:@""].target = self;
	[app addItem:[NSMenuItem separatorItem]];
	[app addItemWithTitle:[L(@"IDD_UPDATE_POLICY") stringByAppendingString:@"…"] action:@selector(settings:) keyEquivalent:@","].target = self;
	[app addItem:[NSMenuItem separatorItem]];
	[app addItemWithTitle:@"Hide Rufus" action:@selector(hide:) keyEquivalent:@"h"];
	[app addItemWithTitle:@"Quit Rufus" action:@selector(terminate:) keyEquivalent:@"q"];
	NSMenuItem* appItem = [NSMenuItem new];
	appItem.submenu = app;
	[bar addItem:appItem];

	NSMenu* file = [[NSMenu alloc] initWithTitle:@"File"];
	[file addItemWithTitle:[L(@"IDC_SELECT") stringByAppendingString:@"…"] action:@selector(open:) keyEquivalent:@"o"].target = self;
	[file addItem:[NSMenuItem separatorItem]];
	[file addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
	NSMenuItem* fileItem = [NSMenuItem new];
	fileItem.submenu = file;
	[bar addItem:fileItem];

	NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
	[edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
	[edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
	[edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
	[edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
	NSMenuItem* editItem = [NSMenuItem new];
	editItem.submenu = edit;
	[bar addItem:editItem];

	NSMenu* win = [[NSMenu alloc] initWithTitle:@"Window"];
	[win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
	[win addItemWithTitle:L(@"MSG_108") action:@selector(showLog:) keyEquivalent:@"l"].target = self;
	NSMenuItem* winItem = [NSMenuItem new];
	winItem.submenu = win;
	[bar addItem:winItem];
	NSApp.windowsMenu = win;

	NSMenu* help = [[NSMenu alloc] initWithTitle:@"Help"];
	[help addItemWithTitle:@"Rufus FAQ" action:@selector(faq:) keyEquivalent:@""].target = self;
	[help addItemWithTitle:@"Open Log File" action:@selector(openLogFile:) keyEquivalent:@""].target = self;
	NSMenuItem* helpItem = [NSMenuItem new];
	helpItem.submenu = help;
	[bar addItem:helpItem];
	NSApp.helpMenu = help;
	NSApp.mainMenu = bar;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n
{
	NSString* res = [[NSBundle mainBundle].resourcePath stringByAppendingPathComponent:@"res"];
	snprintf(resource_dir, sizeof(resource_dir), "%s", res.fileSystemRepresentation);
	snprintf(helper_dir, sizeof(helper_dir), "%s", [NSBundle mainBundle].executablePath.stringByDeletingLastPathComponent.fileSystemRepresentation);
	LogFileOpen([[NSUserDefaults standardUserDefaults] boolForKey:@"PersistentLog"]);
	set_log_handler(app_log);
	[Loc loadFrom:[res stringByAppendingPathComponent:@"loc/rufus.loc"]];

	NSOperatingSystemVersion v = [NSProcessInfo processInfo].operatingSystemVersion;
	char model[64] = "";
	size_t len = sizeof(model);
	sysctlbyname("hw.model", model, &len, NULL, 0);
	LogFileWrite("");
	uprintf("Rufus for Mac %s (%s)", RUFUS_MAC_VERSION,
#if defined(__arm64__)
		"arm64"
#else
		"x86_64"
#endif
	);
	uprintf("macOS %ld.%ld.%ld on %s, Liquid Glass: %s", (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion,
		model, UIHasLiquidGlass() ? "Yes" : "No");
	uprintf("Locale: %s, UI language: %s", [NSLocale currentLocale].localeIdentifier.UTF8String, [Loc currentCode].UTF8String);
	uprintf("Log file: %s", LogFilePath().UTF8String);

	[self buildMenu];
	self.main = [MainWindow new];
	[self.main showWindow:nil];
	[[Drives shared] refresh];
	[SettingsPanel checkIfDue:self.main.window];
	/* Developer aid: `-RufusShow about|log|settings|download` opens a dialog at startup (for screenshots/tests) */
	NSString* show = [[NSUserDefaults standardUserDefaults] stringForKey:@"RufusShow"];
	if (show != nil) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
			if ([show isEqualToString:@"about"]) [self about:nil];
			else if ([show isEqualToString:@"log"]) [self showLog:nil];
			else if ([show isEqualToString:@"settings"]) [self settings:nil];
			else if ([show isEqualToString:@"download"]) [self.main performSelector:@selector(downloadISO)];
		});
	}
	[[NSNotificationCenter defaultCenter] addObserverForName:@"RufusLanguageChanged" object:nil queue:nil
		usingBlock:^(NSNotification* note) { [self buildMenu]; }];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { return YES; }

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
	return [self.main isBusy] ? NSTerminateCancel : NSTerminateNow;
}

/* Drop an ISO on the Dock icon, or "Open With" */
- (BOOL)application:(NSApplication*)app openFile:(NSString*)filename
{
	[self.main openImageAtPath:filename];
	return YES;
}

- (void)about:(id)sender { [self.main performSelector:@selector(about:) withObject:nil]; }
- (void)settings:(id)sender { [self.main performSelector:@selector(settings:) withObject:nil]; }
- (void)showLog:(id)sender { [[LogWindow shared] toggle]; }
- (void)open:(id)sender { [self.main performSelector:@selector(browseForImage)]; }
- (void)faq:(id)sender { [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://github.com/pbatard/rufus/wiki/FAQ"]]; }
- (void)openLogFile:(id)sender { [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:LogFilePath()]]; }

@end

int main(int argc, const char* argv[])
{
	@autoreleasepool {
		/* Rufus always starts fresh: no window restoration (and no "reopen windows?" prompt) */
		[[NSUserDefaults standardUserDefaults] registerDefaults:@{ @"NSQuitAlwaysKeepsWindows": @NO }];
		NSApplication* app = [NSApplication sharedApplication];
		AppDelegate* d = [AppDelegate new];
		app.delegate = d;
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		[app activateIgnoringOtherApps:YES];
		[app run];
	}
	return 0;
}
