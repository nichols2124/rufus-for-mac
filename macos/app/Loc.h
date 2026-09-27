/*
 * Rufus for macOS: localization, using Rufus' own rufus.loc translations
 * Copyright © 2026 Rufus macOS port contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Foundation/Foundation.h>

@interface RufusLanguage : NSObject
@property (copy) NSString* code;         /* e.g. "fr-FR" */
@property (copy) NSString* name;         /* e.g. "French (Français)" */
@property (assign) BOOL rtl;
@end

@interface Loc : NSObject
+ (void)loadFrom:(NSString*)path;
+ (NSArray<RufusLanguage*>*)languages;
+ (NSString*)currentCode;
+ (BOOL)isRTL;
+ (void)selectLanguage:(NSString*)code;   /* nil = follow the system */
+ (NSString*)str:(NSString*)key;          /* raw string (falls back to English, then to the key) */
@end

/* L(@"MSG_210") / LF(@"MSG_208", 3): printf-style, with %s arguments given as C strings */
NSString* L(NSString* key);
NSString* LF(NSString* key, ...);
