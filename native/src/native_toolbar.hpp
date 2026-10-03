/*
 * native_toolbar.hpp - The unified macOS toolbar
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#import <Cocoa/Cocoa.h>

#include "menu_model.hpp"

// Power, Ctrl+Reset and Reboot; a pull-down naming the machine; and the
// windows a person reaches for, each with its SF Symbol. A button sends the
// same action as the menu item it stands for, so there is one path for each.
// Disks opens the 5.25" drives, or, on a machine with 3.5" drives as well,
// asks which.
@interface NativeToolbar : NSObject <NSToolbarDelegate>
- (instancetype)initWithChosen:(void (^)(NSString *action))chosen;
- (void)attachToWindow:(NSWindow *)window;
- (void)update:(const a2e::native::ToolbarState &)state;
@end
