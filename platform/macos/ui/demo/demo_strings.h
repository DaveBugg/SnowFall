// UI strings for the demo harness, built from core/i18n/strings_table.inc (the same table the
// app uses), so the screenshots show the real texts.
#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN
/// StrId name -> text, Russian or English.
NSDictionary<NSString *, NSString *> *SFDemoStrings(BOOL russian);
NS_ASSUME_NONNULL_END
