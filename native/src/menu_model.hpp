/*
 * menu_model.hpp - The menu bar, described in C++ and built by Cocoa
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <string>
#include <vector>

namespace a2e::native {

// Modifier keys for a key equivalent, as a menu shows them.
enum MenuModifier : unsigned {
  MOD_COMMAND = 1,
  MOD_SHIFT = 2,
  MOD_OPTION = 4,
  MOD_CONTROL = 8,
};

// One item, or a submenu when it has children, or a separator. The App
// describes its menus with these each frame; the platform rebuilds the real
// menu bar only when the description changes, and hands back the action id
// of whatever was chosen. Built this way so the menus live with the rest of
// the UI in plain C++, as the Tauri build's native menu reads its model from
// the hidden header rather than duplicating it.
struct MenuItem {
  std::string title;
  std::string action; // empty for a submenu or a separator
  std::string key;    // a key equivalent: "o", "1", "F12", "Escape", or empty
  unsigned modifiers = 0;
  bool checked = false;
  bool enabled = true;
  bool separator = false;
  std::vector<MenuItem> children;

  static MenuItem separatorItem() {
    MenuItem item;
    item.separator = true;
    return item;
  }
};

// The top-level menus, File onwards; the application menu and the Window
// menu are the platform's own.
using MenuBar = std::vector<MenuItem>;

// What the toolbar shows. Its buttons send the same actions as the menu
// items they stand for.
struct ToolbarState {
  bool powered = false;
  std::string machineName;
  std::vector<MenuItem> machines; // the Machine menu's choices
  bool hardDrives = false;        // there is a SmartPort to show
  bool expansionSlots = false;    // the machine has sockets (not a //c)
};

// A string that changes whenever anything a menu shows changes, so the
// platform knows when to rebuild.
std::string menuSignature(const MenuBar &bar);

} // namespace a2e::native
