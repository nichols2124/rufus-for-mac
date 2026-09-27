/*
 * Rufus for macOS: drive enumeration and exclusive access
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This replaces Rufus' SetupAPI/IOCTL based dev.c/drive.c on Windows:
 * - DiskArbitration lists drives and notifies us of arrivals/removals.
 * - The same framework unmounts the volumes and vetoes any mount attempts
 *   while a drive is being written (Windows Rufus locks the volumes).
 * - /usr/libexec/authopen shows the standard macOS administrator prompt and
 *   hands us a file descriptor for the raw device, so the app itself never
 *   needs to run as root.
 */
#import "Drives.h"
#import "Loc.h"
#import <DiskArbitration/DiskArbitration.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include "rufus_core.h"

extern char** environ;

NSString* SizeToHuman(uint64_t size, BOOL copy_to_log, BOOL fake_units)
{
	/* Rufus' "fake units": drive manufacturers' sizes, e.g. 32 GB rather than 29.8 GB */
	static NSString* suffix[] = { @"MSG_020", @"MSG_021", @"MSG_022", @"MSG_023", @"MSG_024", @"MSG_025" };
	double hr = (double)size, t;
	int s = 0;
	(void)copy_to_log;
	while (s < 5 && hr >= 1024.0) {
		hr /= 1024.0;
		s++;
	}
	if (s == 0)
		return [NSString stringWithFormat:@"%llu %@", size, L(suffix[0])];
	if (fake_units) {
		/* Round up to the nearest "power of two" size, as marketed */
		for (t = 1.0; t < 2048.0; t *= 2) {
			if (hr < t * 1.07 && hr > t * 0.85)
				return [NSString stringWithFormat:@"%.0f %@", t, L(suffix[s])];
		}
	}
	return [NSString stringWithFormat:(hr - (int)hr < 0.05) ? @"%.0f %@" : @"%.1f %@", hr, L(suffix[s])];
}

@implementation RufusDrive
- (NSString*)displayName
{
	NSString* name = self.label.length ? self.label :
		[[NSString stringWithFormat:@"%@ %@", self.vendor ?: @"", self.model ?: @""]
			stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
	if (name.length == 0)
		name = L(@"MSG_045");
	return [NSString stringWithFormat:@"%@ (%@) [%@]", name, self.bsdName,
		SizeToHuman(self.size, NO, !rflags.use_proper_size_units)];
}
@end

@interface Drives () {
	DASessionRef session;
	DASessionRef lockSession;
	dispatch_queue_t lockQueue;
	NSMutableDictionary<NSString*, NSDictionary*>* disks;   /* all DA disks, by BSD name */
	NSString* lockedBSD;
}
@property (readwrite) NSArray<RufusDrive*>* drives;
@end

static void disk_changed(DADiskRef disk, void* ctx);
static void disk_desc_changed(DADiskRef disk, CFArrayRef keys, void* ctx);
static void disk_gone(DADiskRef disk, void* ctx);

@implementation Drives

+ (instancetype)shared
{
	static Drives* d;
	static dispatch_once_t once;
	dispatch_once(&once, ^{ d = [Drives new]; });
	return d;
}

- (instancetype)init
{
	if ((self = [super init])) {
		disks = [NSMutableDictionary dictionary];
		_drives = @[];
		session = DASessionCreate(kCFAllocatorDefault);
		DARegisterDiskAppearedCallback(session, NULL, disk_changed, (__bridge void*)self);
		DARegisterDiskDisappearedCallback(session, NULL, disk_gone, (__bridge void*)self);
		DARegisterDiskDescriptionChangedCallback(session, NULL, NULL, disk_desc_changed, (__bridge void*)self);
		DASessionScheduleWithRunLoop(session, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
		lockQueue = dispatch_queue_create("rufus.drive-lock", DISPATCH_QUEUE_SERIAL);
	}
	return self;
}

- (void)diskUpdated:(DADiskRef)disk removed:(BOOL)removed
{
	const char* bsd = DADiskGetBSDName(disk);
	if (bsd == NULL)
		return;
	NSString* name = @(bsd);
	if (removed) {
		[disks removeObjectForKey:name];
	} else {
		CFDictionaryRef desc = DADiskCopyDescription(disk);
		if (desc != NULL) {
			disks[name] = (__bridge_transfer NSDictionary*)desc;
		}
	}
	/* Coalesce bursts of notifications (a drive arrival produces one per partition) */
	[NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(refresh) object:nil];
	[self performSelector:@selector(refresh) withObject:nil afterDelay:0.4];
}

static void disk_changed(DADiskRef disk, void* ctx) { [(__bridge Drives*)ctx diskUpdated:disk removed:NO]; }
static void disk_desc_changed(DADiskRef disk, CFArrayRef keys, void* ctx) { (void)keys; [(__bridge Drives*)ctx diskUpdated:disk removed:NO]; }
static void disk_gone(DADiskRef disk, void* ctx) { [(__bridge Drives*)ctx diskUpdated:disk removed:YES]; }

- (void)refresh
{
	NSMutableArray* list = [NSMutableArray array];
	for (NSString* bsd in [[disks allKeys] sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
		NSDictionary* d = disks[bsd];
		if (![d[(__bridge NSString*)kDADiskDescriptionMediaWholeKey] boolValue])
			continue;
		NSString* protocol = d[(__bridge NSString*)kDADiskDescriptionDeviceProtocolKey];
		NSString* model = [d[(__bridge NSString*)kDADiskDescriptionDeviceModelKey]
			stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
		NSString* vendor = [d[(__bridge NSString*)kDADiskDescriptionDeviceVendorKey]
			stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
		NSString* kind = d[(__bridge NSString*)kDADiskDescriptionMediaKindKey];
		BOOL internal = [d[(__bridge NSString*)kDADiskDescriptionDeviceInternalKey] boolValue];
		BOOL removable = [d[(__bridge NSString*)kDADiskDescriptionMediaRemovableKey] boolValue] ||
			[d[(__bridge NSString*)kDADiskDescriptionMediaEjectableKey] boolValue];
		uint64_t size = [d[(__bridge NSString*)kDADiskDescriptionMediaSizeKey] unsignedLongLongValue];
		BOOL is_image = [protocol isEqualToString:@"Disk Image"] || [protocol isEqualToString:@"Virtual Interface"] ||
			[model isEqualToString:@"Disk Image"];
		BOOL is_optical = [kind isEqualToString:@"IOCDMedia"] || [kind isEqualToString:@"IODVDMedia"] || [kind isEqualToString:@"IOBDMedia"];
		BOOL is_vmware = [vendor rangeOfString:@"VMware" options:NSCaseInsensitiveSearch].location != NSNotFound;
		BOOL is_usb = [protocol isEqualToString:@"USB"];
		BOOL is_sd = [protocol isEqualToString:@"Secure Digital"];
		BOOL list_it;

		if (size == 0 || protocol == nil)
			continue;	/* APFS containers and other synthesized devices */
		if (is_image)
			list_it = rflags.list_virtual_disks;
		else if (is_optical)
			list_it = NO;	/* Only used by Alt-O */
		else if (is_vmware)
			list_it = rflags.list_vmware_disks;
		else if (is_usb)
			list_it = removable || rflags.list_usb_hdd;
		else if (is_sd && removable)
			list_it = YES;	/* Built-in SD card readers are the Mac equivalent of USB card readers */
		else
			list_it = !internal && removable ? rflags.list_non_usb_removable :
				(!internal && rflags.list_non_usb_removable && rflags.list_usb_hdd);
		if (rflags.usb_debug)
			uprintf("Device %s: protocol=%s internal=%d removable=%d kind=%s size=%llu -> %s", bsd.UTF8String,
				protocol.UTF8String, internal, removable, kind.UTF8String, size, list_it ? "listed" : "ignored");
		if (!list_it)
			continue;

		RufusDrive* drive = [RufusDrive new];
		drive.bsdName = bsd;
		drive.vendor = vendor;
		drive.model = model;
		drive.protocol = protocol;
		drive.size = size;
		drive.blockSize = [d[(__bridge NSString*)kDADiskDescriptionMediaBlockSizeKey] unsignedIntValue];
		drive.removable = removable;
		drive.internal = internal;
		drive.isOptical = is_optical;
		drive.partitionScheme = d[(__bridge NSString*)kDADiskDescriptionMediaContentKey];
		/* Label & file system from the first partition that has a volume */
		for (NSString* child in [[disks allKeys] sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
			if (![child hasPrefix:[bsd stringByAppendingString:@"s"]])
				continue;
			drive.partitionCount++;
			NSDictionary* cd = disks[child];
			NSString* vname = cd[(__bridge NSString*)kDADiskDescriptionVolumeNameKey];
			if (drive.label == nil && vname.length > 0 && ![vname isEqualToString:@"EFI"]) {
				drive.label = vname;
				drive.fsName = cd[(__bridge NSString*)kDADiskDescriptionVolumeKindKey];
			}
		}
		[list addObject:drive];
	}
	self.drives = list;
	if (self.onChange)
		self.onChange();
}

- (RufusDrive*)driveForBSDName:(NSString*)bsd
{
	for (RufusDrive* d in self.drives)
		if ([d.bsdName isEqualToString:bsd])
			return d;
	return nil;
}

/* ------------------------------------------------------------------------ */
/* Exclusive access                                                          */
/* ------------------------------------------------------------------------ */
static DADissenterRef mount_approval(DADiskRef disk, void* ctx)
{
	Drives* self = (__bridge Drives*)ctx;
	const char* bsd = DADiskGetBSDName(disk);
	NSString* locked = self->lockedBSD;
	if (bsd != NULL && locked != nil) {
		NSString* name = @(bsd);
		if ([name isEqualToString:locked] || [name hasPrefix:[locked stringByAppendingString:@"s"]]) {
			uprintf("Blocked macOS from mounting %s while the drive is in use", bsd);
			return DADissenterCreate(kCFAllocatorDefault, kDAReturnExclusiveAccess, CFSTR("Rufus is writing to this drive"));
		}
	}
	return NULL;
}

typedef struct {
	dispatch_semaphore_t sem;
	DAReturn status;
	char message[256];
} da_result_t;

static void unmount_done(DADiskRef disk, DADissenterRef dissenter, void* ctx)
{
	da_result_t* r = ctx;
	(void)disk;
	r->status = (dissenter != NULL) ? DADissenterGetStatus(dissenter) : kDAReturnSuccess;
	if (dissenter != NULL) {
		CFStringRef s = DADissenterGetStatusString(dissenter);
		if (s != NULL)
			CFStringGetCString(s, r->message, sizeof(r->message), kCFStringEncodingUTF8);
	}
	dispatch_semaphore_signal(r->sem);
}

- (BOOL)lockDrive:(RufusDrive*)drive error:(NSString**)error
{
	da_result_t res = { dispatch_semaphore_create(0), kDAReturnSuccess, "" };
	DADiskRef disk;

	lockedBSD = drive.bsdName;
	if (lockSession == NULL) {
		lockSession = DASessionCreate(kCFAllocatorDefault);
		DASessionSetDispatchQueue(lockSession, lockQueue);
		DARegisterDiskMountApprovalCallback(lockSession, NULL, mount_approval, (__bridge void*)self);
	}
	if (!rflags.lock_drive) {
		uprintf("Drive locking is disabled (Alt-,) - volumes will not be unmounted");
		return YES;
	}
	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, lockSession, drive.bsdName.UTF8String);
	if (disk == NULL) {
		if (error) *error = @"Could not access the drive";
		return NO;
	}
	uprintf("Unmounting all volumes of %s...", drive.bsdName.UTF8String);
	DADiskUnmount(disk, kDADiskUnmountOptionWhole | kDADiskUnmountOptionForce, unmount_done, &res);
	if (dispatch_semaphore_wait(res.sem, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) {
		res.status = kDAReturnBusy;
		snprintf(res.message, sizeof(res.message), "timeout");
	}
	CFRelease(disk);
	if (res.status != kDAReturnSuccess) {
		uprintf("Could not unmount %s: 0x%x %s", drive.bsdName.UTF8String, res.status, res.message);
		if (error) *error = [NSString stringWithFormat:@"Could not unmount the drive (%s). Is a file open on it?",
			res.message[0] ? res.message : "busy"];
		return NO;
	}
	uprintf("All volumes unmounted");
	return YES;
}

- (void)releaseDrive:(RufusDrive*)drive
{
	(void)drive;
	lockedBSD = nil;
}

- (void)remountDrive:(RufusDrive*)drive
{
	DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, drive.bsdName.UTF8String);
	if (disk != NULL) {
		DADiskMount(disk, NULL, kDADiskMountOptionWhole, NULL, NULL);
		CFRelease(disk);
	}
}

/* ------------------------------------------------------------------------ */
/* Raw device access                                                         */
/* ------------------------------------------------------------------------ */
static int recv_fd(int sock)
{
	char data[64];
	struct iovec iov = { data, sizeof(data) };
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = { 0 };
	struct cmsghdr* cmsg;
	int fd = -1;
	ssize_t n;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	do {
		n = recvmsg(sock, &msg, 0);
	} while (n < 0 && errno == EINTR);
	if (n <= 0)
		return -1;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
			memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
	}
	return fd;
}

- (int)openRawDrive:(RufusDrive*)drive readOnly:(BOOL)ro error:(NSString**)error
{
	NSString* path = [@"/dev/r" stringByAppendingString:drive.bsdName];
	int fd, sv[2], status = 0;
	pid_t pid;
	char flags[8];
	posix_spawn_file_actions_t fa;

	/* Disk images attached by the user (and root) don't need elevation */
	fd = open(path.UTF8String, ro ? O_RDONLY : O_RDWR);
	if (fd >= 0) {
		uprintf("Opened %s directly", path.UTF8String);
		return fd;
	}
	if (errno != EACCES && errno != EPERM) {
		if (error) *error = [NSString stringWithFormat:@"Could not open %@: %s", path, strerror(errno)];
		return -1;
	}

	uprintf("Requesting disk access...");
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		if (error) *error = @"socketpair() failed";
		return -1;
	}
	snprintf(flags, sizeof(flags), "%d", ro ? O_RDONLY : O_RDWR);
	char* argv[] = { "authopen", "-stdoutpipe", "-o", flags, (char*)path.UTF8String, NULL };
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, sv[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&fa, sv[0]);
	status = posix_spawn(&pid, "/usr/libexec/authopen", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(sv[1]);
	if (status != 0) {
		close(sv[0]);
		if (error) *error = [NSString stringWithFormat:@"Could not launch authopen: %s", strerror(status)];
		return -1;
	}
	fd = recv_fd(sv[0]);
	close(sv[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
	if (fd < 0) {
		uprintf("authopen failed (status %d) - access denied or cancelled", WEXITSTATUS(status));
		if (error) *error = L(@"MSG_288");	/* Missing elevated privileges */
		return -1;
	}
	uprintf("Obtained %s access to %s", ro ? "read" : "read/write", path.UTF8String);
	return fd;
}

@end
