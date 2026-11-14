#include "app/status_item.h"

#include "app/icon_images.h"
#include "app/strings_mac.h"

using snow::mac::MenuCommand;
using snow::mac::Str;
using snow::mac::tr;

@implementation SFStatusController {
    NSStatusItem* _item;
    std::function<void(MenuCommand, int)> _handler;
    snow::mac::StatusMenuState _state;
    BOOL _hasState;
    NSImage* _iconRunning;
    NSImage* _iconPaused;
}

- (instancetype)initWithHandler:(std::function<void(MenuCommand, int)>)handler {
    if ((self = [super init])) {
        _handler = std::move(handler);
        _iconRunning = snow::mac::makeMenuBarImage(false);
        _iconPaused = snow::mac::makeMenuBarImage(true);
        _item = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
        _item.autosaveName = @"SnowFall";
        _item.button.image = _iconRunning;
        _item.button.toolTip = @"SnowFall";
        _item.button.target = self;
        _item.button.action = @selector(clicked:);
        [_item.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    }
    return self;
}

- (NSStatusBarButton*)button {
    return _item.button;
}

- (void)remove {
    if (_item) [NSStatusBar.systemStatusBar removeStatusItem:_item];
    _item = nil;
}

- (void)setState:(const snow::mac::StatusMenuState&)state {
    if (_hasState && state == _state) return;
    _state = state;
    _hasState = YES;
    _item.button.image = state.paused ? _iconPaused : _iconRunning;
    NSString* tip = [NSString stringWithFormat:@"SnowFall — %@", [NSString stringWithUTF8String:state.statusText.c_str()]];
    if (!state.holdKeys.empty() && !state.paused) {
        NSString* hint = [tr(Str::HoldHintShortFmt) stringByReplacingOccurrencesOfString:@"%ls"
                                                                              withString:[NSString stringWithUTF8String:state.holdKeys.c_str()]];
        tip = [tip stringByAppendingFormat:@"\n%@", hint];
    }
    _item.button.toolTip = tip;
}

- (void)clicked:(id)sender {
    (void)sender;
    NSEvent* ev = NSApp.currentEvent;
    const bool right = ev.type == NSEventTypeRightMouseUp || (ev.modifierFlags & NSEventModifierFlagControl) != 0;
    if (right) {
        [self popUpMenu];
    } else if (_handler) {
        _handler(MenuCommand::LeftClick, 0);
    }
}

- (NSMenuItem*)add:(NSMenu*)menu title:(NSString*)title cmd:(MenuCommand)cmd checked:(BOOL)checked {
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:@selector(menuItem:) keyEquivalent:@""];
    it.target = self;
    it.tag = static_cast<NSInteger>(cmd);
    it.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:it];
    return it;
}

- (NSMenu*)buildMenu {
    const snow::mac::StatusMenuState& s = _state;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"SnowFall"];
    menu.autoenablesItems = NO;

    NSMenuItem* status = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:s.statusText.c_str()]
                                                    action:nil
                                             keyEquivalent:@""];
    status.enabled = NO;
    [menu addItem:status];
    if (!s.updateVersion.empty()) {
        NSString* title = [tr(Str::MenuUpdateAvailableFmt) stringByReplacingOccurrencesOfString:@"%ls"
                                                                                     withString:[NSString stringWithUTF8String:s.updateVersion.c_str()] ?: @""];
        [self add:menu title:title cmd:MenuCommand::OpenUpdateInfo checked:NO];
    }
    [menu addItem:NSMenuItem.separatorItem];

    [self add:menu title:tr(s.paused && !s.autoPaused ? Str::MenuResume : Str::MenuPause) cmd:MenuCommand::TogglePause checked:NO];

    NSMenu* intensity = [[NSMenu alloc] initWithTitle:tr(Str::MenuIntensity)];
    intensity.autoenablesItems = NO;
    [self add:intensity title:tr(Str::PresetLight) cmd:MenuCommand::PresetLight checked:s.preset == snow::WeatherPreset::Light];
    [self add:intensity title:tr(Str::PresetNormal) cmd:MenuCommand::PresetNormal checked:s.preset == snow::WeatherPreset::Normal];
    [self add:intensity title:tr(Str::PresetBlizzard) cmd:MenuCommand::PresetBlizzard checked:s.preset == snow::WeatherPreset::Blizzard];
    [self add:intensity title:tr(Str::PresetThaw) cmd:MenuCommand::PresetThaw checked:s.preset == snow::WeatherPreset::Thaw];
    NSMenuItem* intensityItem = [[NSMenuItem alloc] initWithTitle:tr(Str::MenuIntensity) action:nil keyEquivalent:@""];
    intensityItem.submenu = intensity;
    [menu addItem:intensityItem];

    NSMenu* mine = [[NSMenu alloc] initWithTitle:tr(Str::MenuUserPresets)];
    mine.autoenablesItems = NO;
    if (s.userPresets.empty()) {
        NSMenuItem* none = [[NSMenuItem alloc] initWithTitle:tr(Str::MenuNoPresets) action:nil keyEquivalent:@""];
        none.enabled = NO;
        [mine addItem:none];
    } else {
        for (std::size_t i = 0; i < s.userPresets.size(); ++i) {
            NSMenuItem* it = [self add:mine
                                 title:[NSString stringWithUTF8String:s.userPresets[i].c_str()] ?: @"?"
                                   cmd:MenuCommand::ApplyUserPreset
                               checked:NO];
            it.representedObject = @(static_cast<NSInteger>(i));
        }
    }
    NSMenuItem* mineItem = [[NSMenuItem alloc] initWithTitle:tr(Str::MenuUserPresets) action:nil keyEquivalent:@""];
    mineItem.submenu = mine;
    [menu addItem:mineItem];

    [menu addItem:NSMenuItem.separatorItem];
    [self add:menu title:tr(Str::MenuInteraction) cmd:MenuCommand::ToggleInteraction checked:s.interaction];
    NSMenu* tool = [[NSMenu alloc] initWithTitle:tr(Str::MenuTool)];
    tool.autoenablesItems = NO;
    [self add:tool title:tr(Str::ToolMitten) cmd:MenuCommand::ToolMitten checked:s.tool == snow::Tool::Mitten];
    [self add:tool title:tr(Str::ToolShovel) cmd:MenuCommand::ToolShovel checked:s.tool == snow::Tool::Shovel];
    [self add:tool title:tr(Str::ToolSnowball) cmd:MenuCommand::ToolSnowball checked:s.tool == snow::Tool::Snowball];
    NSMenuItem* toolItem = [[NSMenuItem alloc] initWithTitle:tr(Str::MenuTool) action:nil keyEquivalent:@""];
    toolItem.submenu = tool;
    [menu addItem:toolItem];

    [self add:menu title:tr(Str::MenuClearSnow) cmd:MenuCommand::ClearSnow checked:NO];
    [self add:menu title:tr(Str::MenuCallSnowplow) cmd:MenuCommand::CallSnowplow checked:NO];
    [self add:menu title:tr(Str::MenuFireworks) cmd:MenuCommand::Fireworks checked:NO];
    [self add:menu title:tr(Str::MenuHideAll) cmd:MenuCommand::ToggleHideAll checked:s.hidden];
    [menu addItem:NSMenuItem.separatorItem];
    if (s.hasSettingsWindow) {
        NSMenuItem* settings = [self add:menu title:tr(Str::MenuSettings) cmd:MenuCommand::OpenSettings checked:NO];
        settings.keyEquivalent = @",";
    } else {
        const bool ru = snow::mac::uiRussian();
        [self add:menu title:(ru ? @"Открыть файл настроек…" : @"Open Settings File…") cmd:MenuCommand::OpenSettingsFile checked:NO];
        [self add:menu title:(ru ? @"Перечитать файл настроек" : @"Reload Settings File") cmd:MenuCommand::ReloadSettingsFile checked:NO];
    }
    [self add:menu title:tr(Str::MenuAutostart) cmd:MenuCommand::ToggleAutostart checked:s.autostart];
    [self add:menu title:tr(Str::BtnOpenLogFolder) cmd:MenuCommand::OpenLogFolder checked:NO];
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* quit = [self add:menu title:tr(Str::MenuExit) cmd:MenuCommand::Quit checked:NO];
    quit.keyEquivalent = @"q";
    return menu;
}

- (void)popUpMenu {
    NSMenu* menu = [self buildMenu];
    NSStatusBarButton* b = _item.button;
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, NSHeight(b.bounds) + 4) inView:b];
}

- (void)menuItem:(NSMenuItem*)item {
    const auto cmd = static_cast<MenuCommand>(item.tag);
    int arg = 0;
    if ([item.representedObject isKindOfClass:[NSNumber class]]) arg = [item.representedObject intValue];
    if (_handler) _handler(cmd, arg);
}

@end
