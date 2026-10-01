/*
 * native_menu.mm - The macOS menu bar, built from the App's menu model
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "native_menu.hpp"

using a2e::native::MenuBar;
using a2e::native::MenuItem;

namespace {

NSString *keyEquivalent(const std::string &key) {
  if (key.empty()) return @"";
  if (key == "Escape") return @"\e";
  if (key.size() > 1 && key[0] == 'F') {
    const int number = std::atoi(key.c_str() + 1);
    if (number >= 1 && number <= 35) {
      const unichar character = static_cast<unichar>(NSF1FunctionKey + number - 1);
      return [NSString stringWithCharacters:&character length:1];
    }
  }
  return @(key.c_str());
}

NSEventModifierFlags modifierMask(unsigned modifiers) {
  NSEventModifierFlags mask = 0;
  if (modifiers & a2e::native::MOD_COMMAND) mask |= NSEventModifierFlagCommand;
  if (modifiers & a2e::native::MOD_SHIFT) mask |= NSEventModifierFlagShift;
  if (modifiers & a2e::native::MOD_OPTION) mask |= NSEventModifierFlagOption;
  if (modifiers & a2e::native::MOD_CONTROL) mask |= NSEventModifierFlagControl;
  return mask;
}

} // namespace

@implementation NativeMenu {
  void (^_chosen)(NSString *);
  std::string _signature;
  BOOL _tracking;
  BOOL _pending;
  MenuBar _pendingBar;
}

- (instancetype)initWithChosen:(void (^)(NSString *))chosen {
  self = [super init];
  if (!self) return nil;
  _chosen = [chosen copy];
  NSNotificationCenter *centre = NSNotificationCenter.defaultCenter;
  [centre addObserver:self selector:@selector(beganTracking:) name:NSMenuDidBeginTrackingNotification object:nil];
  [centre addObserver:self selector:@selector(endedTracking:) name:NSMenuDidEndTrackingNotification object:nil];
  return self;
}

- (void)beganTracking:(NSNotification *)note {
  _tracking = YES;
}

- (void)endedTracking:(NSNotification *)note {
  _tracking = NO;
}

- (void)chosen:(NSMenuItem *)item {
  if (_chosen && [item.representedObject isKindOfClass:NSString.class]) _chosen(item.representedObject);
}

- (NSMenuItem *)itemFor:(const MenuItem &)model {
  if (model.separator) return [NSMenuItem separatorItem];
  NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@(model.title.c_str())
                                                action:nil
                                         keyEquivalent:keyEquivalent(model.key)];
  item.keyEquivalentModifierMask = modifierMask(model.modifiers);
  item.enabled = model.enabled;
  item.state = model.checked ? NSControlStateValueOn : NSControlStateValueOff;
  if (!model.children.empty()) {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@(model.title.c_str())];
    menu.autoenablesItems = NO;
    for (const MenuItem &child : model.children) [menu addItem:[self itemFor:child]];
    item.submenu = menu;
  } else if (model.action == "toggleFullScreen") {
    // AppKit's own, so the system knows the item and titles it itself.
    item.action = @selector(toggleFullScreen:);
  } else if (!model.action.empty()) {
    item.target = self;
    item.action = @selector(chosen:);
    item.representedObject = @(model.action.c_str());
  }
  return item;
}

// The system's About panel, with who made the app and where to find more.
- (void)showAbout:(id)sender {
  NSMutableParagraphStyle *centred = [[NSMutableParagraphStyle alloc] init];
  centred.alignment = NSTextAlignmentCenter;
  NSFont *font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  NSMutableAttributedString *credits = [[NSMutableAttributedString alloc]
      initWithString:@"Mike Daley\n"
          attributes:@{NSFontAttributeName : font, NSForegroundColorAttributeName : NSColor.labelColor,
                       NSParagraphStyleAttributeName : centred}];
  [credits appendAttributedString:[[NSAttributedString alloc]
                                      initWithString:@"www.retrotech71.co.uk"
                                          attributes:@{NSFontAttributeName : font,
                                                       NSLinkAttributeName : [NSURL URLWithString:@"https://www.retrotech71.co.uk"],
                                                       NSParagraphStyleAttributeName : centred}]];
  [NSApp orderFrontStandardAboutPanelWithOptions:@{NSAboutPanelOptionCredits : credits}];
  [NSApp activateIgnoringOtherApps:YES];
}

- (NSMenuItem *)applicationMenu {
  NSMenuItem *appItem = [[NSMenuItem alloc] init];
  NSMenu *menu = [[NSMenu alloc] init];
  NSMenuItem *about = [menu addItemWithTitle:@"About ApplEm" action:@selector(showAbout:) keyEquivalent:@""];
  about.target = self;
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem *services = [menu addItemWithTitle:@"Services" action:nil keyEquivalent:@""];
  services.submenu = [[NSMenu alloc] init];
  NSApp.servicesMenu = services.submenu;
  [menu addItem:[NSMenuItem separatorItem]];
  [menu addItemWithTitle:@"Hide ApplEm" action:@selector(hide:) keyEquivalent:@"h"];
  NSMenuItem *others = [menu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
  others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
  [menu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
  [menu addItem:[NSMenuItem separatorItem]];
  [menu addItemWithTitle:@"Quit ApplEm" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = menu;
  return appItem;
}

- (NSMenuItem *)windowMenu {
  NSMenuItem *windowItem = [[NSMenuItem alloc] init];
  NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Window"];
  [menu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
  [menu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
  [menu addItem:[NSMenuItem separatorItem]];
  [menu addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
  windowItem.submenu = menu;
  NSApp.windowsMenu = menu;
  return windowItem;
}

- (void)update:(const MenuBar &)bar {
  const std::string signature = a2e::native::menuSignature(bar);
  if (signature == _signature) return;
  if (_tracking) return; // next time, once the menu has closed
  _signature = signature;

  NSMenu *main = [[NSMenu alloc] init];
  [main addItem:[self applicationMenu]];
  for (const MenuItem &menu : bar) [main addItem:[self itemFor:menu]];
  [main addItem:[self windowMenu]];
  NSApp.mainMenu = main;
}

@end
