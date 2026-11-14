// Test-only contract between the UI self-test driver (Swift class SFUISelfTest in snowfall_ui)
// and a fake app (platform/macos/ui/demo). Not used by the real app.
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Implemented by the fake SFAppActions so the driver can check what the UI asked for.
@protocol SFSelfTestProbe <NSObject>
/// Every payload passed to -applySettingsJSON:, oldest first.
@property (nonatomic, readonly) NSArray<NSString *> *appliedJSON;
/// Names of the commands invoked so far: "clearSnow", "launchFireworks", "callSnowplow",
/// "togglePause", "openSettings", "quitApp", "checkForUpdates", "installUpdate", "openUpdatePage",
/// "openUpdateInfo".
@property (nonatomic, readonly) NSArray<NSString *> *commands;
/// How often -resetPreview / -setSettingsJSON: reached the preview view.
@property (nonatomic, readonly) NSInteger previewResets;
@property (nonatomic, readonly) NSInteger previewUpdates;
@end

/// Entry point of the driver (looked up with NSClassFromString(@"SFUISelfTest")).
@protocol SFUISelfTestRunner <NSObject>
/// `actions` must also implement SFAppActions (SFBridge.h). Runs every check, writes PNG
/// screenshots + report.txt into `directory`. Returns 0 on success, otherwise the number of
/// failed checks.
+ (NSInteger)runWithActions:(id<SFSelfTestProbe>)actions
               settingsJSON:(NSString *)json
            outputDirectory:(NSString *)directory;
@end

NS_ASSUME_NONNULL_END
