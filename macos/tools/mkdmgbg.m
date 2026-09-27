/*
 * Rufus for Mac: renders the DMG window background (drag Rufus to Applications)
 * Usage: mkdmgbg <out.png> <scale>   (640x400 points)
 */
#import <Cocoa/Cocoa.h>

int main(int argc, char** argv)
{
	@autoreleasepool {
		if (argc < 3)
			return 1;
		CGFloat sc = atof(argv[2]), W = 640, H = 400;
		NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)(W * sc)
			pixelsHigh:(NSInteger)(H * sc) bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
			colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
		rep.size = NSMakeSize(W, H);
		[NSGraphicsContext saveGraphicsState];
		[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];

		/* Soft background, same palette as the app icon */
		NSGradient* g = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithCalibratedRed:0.97 green:0.98 blue:1.0 alpha:1]
			endingColor:[NSColor colorWithCalibratedRed:0.84 green:0.89 blue:0.97 alpha:1]];
		[g drawInRect:NSMakeRect(0, 0, W, H) angle:-90];

		/* Arrow between the two icons (icons are centered at x=170 and x=470, y=190 from the top) */
		NSBezierPath* arrow = [NSBezierPath bezierPath];
		CGFloat y = H - 190, x0 = 250, x1 = 390;
		[arrow moveToPoint:NSMakePoint(x0, y)];
		[arrow lineToPoint:NSMakePoint(x1 - 16, y)];
		arrow.lineWidth = 6;
		arrow.lineCapStyle = NSLineCapStyleRound;
		[[NSColor colorWithCalibratedRed:0.25 green:0.35 blue:0.55 alpha:0.55] set];
		[arrow stroke];
		NSBezierPath* head = [NSBezierPath bezierPath];
		[head moveToPoint:NSMakePoint(x1, y)];
		[head lineToPoint:NSMakePoint(x1 - 24, y + 16)];
		[head lineToPoint:NSMakePoint(x1 - 24, y - 16)];
		[head closePath];
		[head fill];

		/* Title and hint */
		NSMutableParagraphStyle* ps = [NSMutableParagraphStyle new];
		ps.alignment = NSTextAlignmentCenter;
		NSDictionary* t = @{ NSFontAttributeName: [NSFont systemFontOfSize:26 weight:NSFontWeightSemibold],
			NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.15 alpha:1], NSParagraphStyleAttributeName: ps };
		NSDictionary* h = @{ NSFontAttributeName: [NSFont systemFontOfSize:15 weight:NSFontWeightRegular],
			NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.35 alpha:1], NSParagraphStyleAttributeName: ps };
		[@"Rufus for Mac" drawInRect:NSMakeRect(0, H - 70, W, 36) withAttributes:t];
		[@"Drag Rufus to the Applications folder to install it" drawInRect:NSMakeRect(0, 52, W, 24) withAttributes:h];

		[NSGraphicsContext restoreGraphicsState];
		NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
		return [png writeToFile:@(argv[1]) atomically:YES] ? 0 : 1;
	}
}
