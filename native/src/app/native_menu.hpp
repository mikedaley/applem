/*
 * native_menu.hpp - The macOS menu bar, built from the App's menu model
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#import <Cocoa/Cocoa.h>

#include "app/menu_model.hpp"

// The application menu and the Window menu are the platform's, as on every
// Mac app, with the items the model gives them; the rest come from the
// model. The bar is
// rebuilt only when the model changes, and never while a menu is open: a
// rebuild under the user's pointer would close the menu they are reading.
@interface NativeMenu : NSObject
- (instancetype)initWithChosen:(void (^)(NSString *action))chosen;
// The window full screen is for. AppKit sends toggleFullScreen: to the key
// window, and with a tool window key that did nothing at all.
@property(nonatomic, weak) NSWindow *primaryWindow;
- (void)update:(const a2e::native::MenuBar &)bar;
@end
