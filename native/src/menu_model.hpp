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
  // Not shown, but its key equivalent still works: a second key for an item
  // that is shown, such as Xcode's F7 beside Step Into's F11.
  bool hidden = false;
  std::vector<MenuItem> children;

  static MenuItem separatorItem() {
    MenuItem item;
    item.separator = true;
    return item;
  }
};

// The top-level menus, File onwards. Three titles are the platform's own
// menus rather than menus of their own: "ApplEm" adds its items to the
// application menu after About, "Window" adds its items to the Window menu
// after Zoom, and "Help" becomes the Help menu, which the system gives its
// search field.
using MenuBar = std::vector<MenuItem>;
inline constexpr const char *APPLICATION_MENU = "ApplEm";
inline constexpr const char *WINDOW_MENU = "Window";
inline constexpr const char *HELP_MENU = "Help";

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
