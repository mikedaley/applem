/*
 * native_toolbar.mm - The unified macOS toolbar
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "native_toolbar.hpp"

namespace {

struct Button {
  NSString *identifier;
  NSString *label;
  NSString *symbol;
  NSString *action;
  NSString *tooltip;
};

const Button BUTTONS[] = {
    {@"power", @"Power", @"power", @"machine.power", @"Switch the machine on or off"},
    {@"reset", @"Reset", @"arrow.uturn.backward.circle", @"machine.ctrlreset", @"Ctrl+Reset"},
    {@"reboot", @"Reboot", @"arrow.clockwise.circle", @"machine.reboot", @"Restart the machine from cold"},
    {@"drives", @"Disks", @"opticaldiscdrive", @"view.drives", @"Disk Drives"},
    {@"harddrives", @"SmartPort", @"externaldrive", @"view.harddrives", @"SmartPort Drives"},
    {@"joystick", @"Joystick", @"gamecontroller", @"view.joystick", @"Joystick"},
    {@"display", @"Display", @"tv", @"view.display", @"Display Settings"},
    {@"states", @"States", @"clock.arrow.circlepath", @"states.show", @"Save States"},
};

NSString *const MACHINE = @"machine";

} // namespace

@implementation NativeToolbar {
  void (^_chosen)(NSString *);
  NSWindow *_window;
  NSMutableDictionary<NSString *, NSToolbarItem *> *_items;
  NSMenuToolbarItem *_machine;
  std::string _signature;
  BOOL _hardDrives;
  BOOL _powered;
  BOOL _poweredKnown;
}

- (instancetype)initWithChosen:(void (^)(NSString *))chosen {
  self = [super init];
  if (!self) return nil;
  _chosen = [chosen copy];
  _items = [NSMutableDictionary dictionary];
  return self;
}

- (void)attachToWindow:(NSWindow *)window {
  _window = window;
  NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"ApplEmToolbar"];
  toolbar.delegate = self;
  toolbar.displayMode = NSToolbarDisplayModeIconOnly;
  toolbar.allowsUserCustomization = YES;
  toolbar.autosavesConfiguration = YES;
  window.toolbar = toolbar;
  window.toolbarStyle = NSWindowToolbarStyleUnified;
  window.titleVisibility = NSWindowTitleVisible;
}

- (void)chosen:(id)sender {
  NSString *action = nil;
  if ([sender isKindOfClass:NSMenuItem.class]) action = [(NSMenuItem *)sender representedObject];
  else if ([sender isKindOfClass:NSToolbarItem.class]) action = [(NSToolbarItem *)sender itemIdentifier];
  if (!action) return;
  for (const Button &button : BUTTONS) {
    if ([button.identifier isEqualToString:action]) action = button.action;
  }
  if (_chosen) _chosen(action);
}

- (BOOL)validateToolbarItem:(NSToolbarItem *)item {
  if ([item.itemIdentifier isEqualToString:@"harddrives"]) return _hardDrives;
  return YES;
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar {
  return @[ @"power", @"reset", @"reboot", NSToolbarFlexibleSpaceItemIdentifier, MACHINE,
            NSToolbarFlexibleSpaceItemIdentifier, @"drives", @"harddrives", @"joystick", @"display", @"states" ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar {
  NSMutableArray *all = [NSMutableArray array];
  for (const Button &button : BUTTONS) [all addObject:button.identifier];
  [all addObjectsFromArray:@[ MACHINE, NSToolbarFlexibleSpaceItemIdentifier, NSToolbarSpaceItemIdentifier ]];
  return all;
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)identifier
    willBeInsertedIntoToolbar:(BOOL)flag {
  if ([identifier isEqualToString:MACHINE]) {
    _machine = [[NSMenuToolbarItem alloc] initWithItemIdentifier:MACHINE];
    _machine.label = @"Machine";
    _machine.toolTip = @"Choose the machine";
    _machine.showsIndicator = YES;
    _machine.menu = [[NSMenu alloc] init];
    _signature.clear(); // fill it at the next update
    return _machine;
  }
  for (const Button &button : BUTTONS) {
    if (![button.identifier isEqualToString:identifier]) continue;
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
    item.label = button.label;
    item.paletteLabel = button.label;
    item.toolTip = button.tooltip;
    item.image = [NSImage imageWithSystemSymbolName:button.symbol accessibilityDescription:button.label];
    item.bordered = YES;
    item.target = self;
    item.action = @selector(chosen:);
    _items[identifier] = item;
    if ([identifier isEqualToString:@"power"]) _poweredKnown = NO;
    return item;
  }
  return nil;
}

- (void)update:(const a2e::native::ToolbarState &)state {
  _hardDrives = state.hardDrives;

  // Power wears green while the machine is on.
  NSToolbarItem *power = _items[@"power"];
  if (power && (!_poweredKnown || _powered != state.powered)) {
    _powered = state.powered;
    _poweredKnown = YES;
    NSImage *symbol = [NSImage imageWithSystemSymbolName:@"power" accessibilityDescription:@"Power"];
    if (state.powered) {
      NSImageSymbolConfiguration *green =
          [NSImageSymbolConfiguration configurationWithHierarchicalColor:NSColor.systemGreenColor];
      symbol = [symbol imageWithSymbolConfiguration:green];
    }
    power.image = symbol;
  }

  // The machine pull-down, rebuilt only when what it shows changes.
  std::string signature = state.machineName;
  for (const auto &machine : state.machines) {
    signature += machine.title + (machine.checked ? "*" : "") + (machine.enabled ? "" : "-");
  }
  if (_machine && signature != _signature) {
    _signature = signature;
    _machine.title = @(state.machineName.c_str());
    NSMenu *menu = [[NSMenu alloc] init];
    menu.autoenablesItems = NO;
    for (const auto &machine : state.machines) {
      NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@(machine.title.c_str())
                                                    action:@selector(chosen:)
                                             keyEquivalent:@""];
      item.target = self;
      item.representedObject = @(machine.action.c_str());
      item.state = machine.checked ? NSControlStateValueOn : NSControlStateValueOff;
      item.enabled = machine.enabled;
      [menu addItem:item];
    }
    _machine.menu = menu;
  }
  NSString *subtitle = @(state.machineName.c_str());
  if (_window && ![_window.subtitle isEqualToString:subtitle]) _window.subtitle = subtitle;
}

@end
