// SFAppActions implementation (platform/macos/bridge/SFBridge.h): what the SwiftUI side may
// ask of the app shell. Main thread.
#pragma once
#import <AppKit/AppKit.h>

#import "SFBridge.h"

namespace snow::mac {
class AppHost;
}

@interface SFAppActionsImpl : NSObject <SFAppActions>
- (instancetype)initWithHost:(snow::mac::AppHost*)host;
@end
