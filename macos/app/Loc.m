/*
 * Rufus for macOS: localization, using Rufus' own rufus.loc translations
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * rufus.loc format (see src/localization.c and res/loc/pollock):
 *   l "fr-FR" "French (Français)" 0x040c, ...   start of a language
 *   b "en-US"                                    base language
 *   a "r"                                        attributes ('r' = right to left)
 *   g IDD_DIALOG                                 group (dialog)
 *   t KEY "text"                                 translation
 *   <TAB>"more text"                             continuation of the previous string
 */
#import "Loc.h"
#include <stdarg.h>

@implementation RufusLanguage
@end

static NSMutableDictionary<NSString*, NSMutableDictionary<NSString*, NSString*>*>* tables;
static NSMutableArray<RufusLanguage*>* langs;
static NSString* current = @"en-US";

static NSString* unescape(NSString* s)
{
	NSMutableString* out = [NSMutableString stringWithCapacity:s.length];
	for (NSUInteger i = 0; i < s.length; i++) {
		unichar c = [s characterAtIndex:i];
		if (c == '\\' && i + 1 < s.length) {
			unichar n = [s characterAtIndex:++i];
			switch (n) {
			case 'n': [out appendString:@"\n"]; break;
			case 't': [out appendString:@"\t"]; break;
			case '"': [out appendString:@"\""]; break;
			case '\\': [out appendString:@"\\"]; break;
			default: [out appendFormat:@"\\%C", n]; break;
			}
		} else {
			[out appendFormat:@"%C", c];
		}
	}
	return out;
}

/* Extract the first "quoted" string of a line */
static NSString* quoted(NSString* line)
{
	NSRange a = [line rangeOfString:@"\""];
	NSRange b = [line rangeOfString:@"\"" options:NSBackwardsSearch];
	if (a.location == NSNotFound || b.location == a.location)
		return nil;
	return unescape([line substringWithRange:NSMakeRange(a.location + 1, b.location - a.location - 1)]);
}

@implementation Loc

+ (void)loadFrom:(NSString*)path
{
	NSString* data = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
	NSMutableDictionary* table = nil;
	NSString* lastKey = nil;
	RufusLanguage* lang = nil;

	tables = [NSMutableDictionary dictionary];
	langs = [NSMutableArray array];
	for (NSString* line in [data componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
		if (line.length < 2 || [line hasPrefix:@"#"])
			continue;
		unichar c = [line characterAtIndex:0];
		if (c == 'l' && [line characterAtIndex:1] == ' ') {
			NSArray* parts = [line componentsSeparatedByString:@"\""];
			if (parts.count < 5)
				continue;
			lang = [RufusLanguage new];
			lang.code = parts[1];
			lang.name = parts[3];
			[langs addObject:lang];
			table = [NSMutableDictionary dictionary];
			tables[lang.code] = table;
			lastKey = nil;
		} else if (c == 'a' && lang != nil) {
			lang.rtl = [quoted(line) containsString:@"r"];
		} else if (c == 't' && table != nil) {
			NSArray* parts = [line componentsSeparatedByString:@" "];
			if (parts.count < 3)
				continue;
			lastKey = parts[1];
			NSString* v = quoted(line);
			if (v != nil)
				table[lastKey] = v;
		} else if ((c == '\t' || c == ' ') && table != nil && lastKey != nil) {
			NSString* v = quoted(line);
			if (v != nil)
				table[lastKey] = [table[lastKey] stringByAppendingString:v];
		}
	}
	[self selectLanguage:[[NSUserDefaults standardUserDefaults] stringForKey:@"Language"]];
}

+ (NSArray<RufusLanguage*>*)languages { return langs; }
+ (NSString*)currentCode { return current; }

+ (BOOL)isRTL
{
	for (RufusLanguage* l in langs)
		if ([l.code isEqualToString:current])
			return l.rtl;
	return NO;
}

+ (void)selectLanguage:(NSString*)code
{
	if (code == nil) {
		/* Match the system's preferred languages against what rufus.loc provides */
		for (NSString* pref in [NSLocale preferredLanguages]) {
			for (RufusLanguage* l in langs) {
				if ([pref caseInsensitiveCompare:l.code] == NSOrderedSame) {
					current = l.code;
					return;
				}
			}
			NSString* lang = [[pref componentsSeparatedByString:@"-"] firstObject];
			for (RufusLanguage* l in langs) {
				if ([[[l.code componentsSeparatedByString:@"-"] firstObject] caseInsensitiveCompare:lang] == NSOrderedSame) {
					current = l.code;
					return;
				}
			}
		}
		current = @"en-US";
	} else if (tables[code] != nil) {
		current = code;
	}
}

+ (NSString*)str:(NSString*)key
{
	NSString* s = tables[current][key];
	if (s == nil)
		s = tables[@"en-US"][key];
	return s ?: key;
}

@end

NSString* L(NSString* key)
{
	return [Loc str:key];
}

NSString* LF(NSString* key, ...)
{
	char buf[4096];
	va_list args;
	va_start(args, key);
	vsnprintf(buf, sizeof(buf), [L(key) UTF8String], args);
	va_end(args);
	return [NSString stringWithUTF8String:buf] ?: key;
}
