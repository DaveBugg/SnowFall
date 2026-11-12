// Small Objective-C helpers shared by the shell.
#pragma once
#import <Foundation/Foundation.h>

// Block target for selector-based observers. Needed for distributed notifications: AppKit
// suspends their delivery while the app is inactive (an accessory app is almost never
// active) unless the observer asks for NSNotificationSuspensionBehaviorDeliverImmediately,
// which only the selector-based API accepts.
@interface SFBlockObserver : NSObject
@property(nonatomic, copy) void (^block)(NSNotification* note);
+ (instancetype)observeDistributed:(NSString*)name block:(void (^)(NSNotification* note))block;
- (void)fire:(NSNotification*)note;
- (void)cancel;
@end

// Brings this (accessory) app to the front so its alerts / windows get focus.
#ifdef __cplusplus
extern "C" {
#endif
void sfActivateApp(void);
#ifdef __cplusplus
}
#endif
