/*
 * native_menu.hpp - The macOS menu bar, built from the App's menu model
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#import <Cocoa/Cocoa.h>

#include "menu_model.hpp"

// The application menu and the Window menu are the platform's, as on every
// Mac app; File, Edit, Machine and View come from the model. The bar is
// rebuilt only when the model changes, and never while a menu is open: a
// rebuild under the user's pointer would close the menu they are reading.
@interface NativeMenu : NSObject
- (instancetype)initWithChosen:(void (^)(NSString *action))chosen;
- (void)update:(const a2e::native::MenuBar &)bar;
@end
