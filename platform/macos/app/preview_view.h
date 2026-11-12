// Live preview for the settings window (SFPreviewView of platform/macos/bridge/SFBridge.h):
// a miniature desktop (two windows and a Dock) running a real snow::World with the settings
// being edited, 4x accelerated, drawn with the same Metal renderer as the overlays.
// Main thread.
#pragma once
#import <AppKit/AppKit.h>

#import "SFBridge.h"

@interface SFPreviewViewImpl : NSView <SFPreviewView>
- (instancetype)initWithSettingsJSON:(NSString*)json;
@end
