#import "demo_strings.h"

#include <cwchar>

namespace {

NSString *fromWide(const wchar_t *text) {
    // wchar_t is UTF-32 on Apple platforms.
    return [[NSString alloc] initWithBytes:text
                                    length:std::wcslen(text) * sizeof(wchar_t)
                                  encoding:NSUTF32LittleEndianStringEncoding];
}

}  // namespace

NSDictionary<NSString *, NSString *> *SFDemoStrings(BOOL russian) {
    NSMutableDictionary<NSString *, NSString *> *table = [NSMutableDictionary dictionary];
#define X(id, ru, en) table[@#id] = fromWide(russian ? ru : en);
#include "i18n/strings_table.inc"
#undef X
    return table;
}
