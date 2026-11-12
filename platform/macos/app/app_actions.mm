#include "app/app_actions.h"

#include <cwchar>

#include "app/app_host.h"
#include "app/preview_view.h"
#include "app/strings_mac.h"
#include "core/skins.h"
#include "tracker/window_tracker.h"

@implementation SFAppActionsImpl {
    snow::mac::AppHost* _host;
}

- (instancetype)initWithHost:(snow::mac::AppHost*)host {
    if ((self = [super init])) _host = host;
    return self;
}

- (void)applySettingsJSON:(NSString*)json {
    _host->applySettingsJson(json.UTF8String ?: "");
}

- (void)clearSnow {
    _host->clearSnow();
}

- (void)launchFireworks {
    _host->fireworks();
}

- (void)callSnowplow {
    _host->callSnowplow();
}

- (void)togglePause {
    _host->togglePause();
}

- (void)openSettings {
    _host->openSettings();
}

- (void)quitApp {
    _host->quit();
}

- (NSString*)diagnosticsJSON {
    return [NSString stringWithUTF8String:_host->diagnosticsJson().c_str()] ?: @"{}";
}

- (NSString*)validateHotkey:(NSString*)text {
    return [NSString stringWithUTF8String:_host->validateHotkey(text.UTF8String ?: "").c_str()] ?: @"";
}

- (NSDictionary<NSString*, NSString*>*)uiStrings {
    return snow::mac::uiStringTable();
}

- (NSArray<NSDictionary<NSString*, NSString*>*>*)snowmanSkins {
    NSMutableArray* out = [NSMutableArray array];
    const bool ru = snow::mac::uiRussian();
    for (int i = 0; i < snow::kSnowmanSkinCount; ++i) {
        const snow::SnowmanSkin& s = snow::kSnowmanSkins[i];
        const wchar_t* w = ru ? s.nameRu : s.nameEn;
        NSString* name = [[NSString alloc] initWithBytes:w
                                                  length:wcslen(w) * sizeof(wchar_t)
                                                encoding:NSUTF32LittleEndianStringEncoding] ?: @"";
        [out addObject:@{@"id" : [NSString stringWithUTF8String:s.id], @"name" : name}];
    }
    return out;
}

- (NSArray<NSString*>*)runningAppNames {
    NSMutableArray* out = [NSMutableArray array];
    for (const std::string& n : snow::mac::runningAppNamesWithWindows()) {
        if (NSString* s = [NSString stringWithUTF8String:n.c_str()]) [out addObject:s];
    }
    return out;
}

- (NSView*)makePreviewView {
    return [[SFPreviewViewImpl alloc] initWithSettingsJSON:[NSString stringWithUTF8String:_host->settingsJson().c_str()]];
}

@end
