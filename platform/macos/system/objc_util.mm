#include "system/objc_util.h"

#import <AppKit/AppKit.h>

@implementation SFBlockObserver

+ (instancetype)observeDistributed:(NSString*)name block:(void (^)(NSNotification*))block {
    SFBlockObserver* o = [SFBlockObserver new];
    o.block = block;
    [NSDistributedNotificationCenter.defaultCenter addObserver:o
                                                      selector:@selector(fire:)
                                                          name:name
                                                        object:nil
                                            suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];
    return o;
}

- (void)fire:(NSNotification*)note {
    if (self.block) self.block(note);
}

- (void)cancel {
    [NSDistributedNotificationCenter.defaultCenter removeObserver:self];
    self.block = nil;
}

@end

void sfActivateApp(void) {
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}
