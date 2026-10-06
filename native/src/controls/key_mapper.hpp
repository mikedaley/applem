/*
 * key_mapper.hpp - Host keys as the core hears them
 *
 * The core's keyboard was written for a browser and takes a browser keycode,
 * the DOM key location and the modifier flags, then does the Apple II's
 * translation itself (keyboard.cpp). So the native front end's whole job is
 * to say which browser keycode a key is, and to make the same choice the
 * browser build makes about which host key is an Apple key
 * (InputHandler.translateAppleKeys).
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "imgui.h"

#include <optional>

namespace a2e::native {

// DOM key locations, as the core's Keyboard::KeyLocation numbers them.
enum KeyLocation { LOCATION_STANDARD = 0, LOCATION_LEFT = 1, LOCATION_RIGHT = 2, LOCATION_NUMPAD = 3 };

// The browser keycodes that matter here by name.
enum BrowserKey {
  KEY_SHIFT = 16,
  KEY_CONTROL = 17,
  KEY_ALT = 18,
  KEY_META_LEFT = 91,
  KEY_META_RIGHT = 93,
};

struct HostKey {
  int keyCode = 0;
  int location = LOCATION_STANDARD;
};

// Which modifier keys are physically held, named for what is printed on a
// Mac keyboard rather than for what ImGui calls them.
struct HeldModifiers {
  bool shift = false;
  bool control = false;
  bool option = false;
  bool command = false;
};

// One key event, ready for MachineHost::handleRawKeyDown/Up.
struct CoreKeyEvent {
  int keyCode = 0;
  int location = LOCATION_STANDARD;
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool meta = false;
};

// The browser keycode for an ImGui key, or nothing for a key the browser
// would not report or the Apple II has no use for.
//
// `macSwap` is ImGui's ConfigMacOSXBehaviors: with it set, ImGui reports the
// physical ⌘ key as ImGuiKey_LeftCtrl/RightCtrl and Control as
// ImGuiKey_LeftSuper/RightSuper, and this undoes that.
std::optional<HostKey> browserKeyFor(ImGuiKey key, bool macSwap);

// The keys whose meaning follows the user's keyboard layout: the letters and
// the punctuation keys. ImGui names keys by where they are on a US keyboard,
// and a browser's keycode for these follows what the key types instead, so
// an AZERTY A, which ImGui calls Q, reaches the machine as an A.
bool followsLayout(ImGuiKey key);

// The browser keycode for the key that types `c` unshifted: a letter is its
// capital, a digit itself, and punctuation the US key that bears it. Nothing
// for anything a US key does not type, which keeps the key's own place.
std::optional<int> browserKeyForCharacter(char32_t c);

// The modifiers held, from ImGui's key state, with the same swap undone.
HeldModifiers heldModifiers(bool macSwap);

// The event the core should see, or nothing if it should see none.
//
// With `commandIsOpenApple` (a IIgs, whose keyboard is a Mac's), ⌘ is sent
// as the left Alt and either Option as the right, and `alt` is whether any
// of them is held. Otherwise the Option keys are the Apple keys, ⌘ is never
// sent, and nothing typed under ⌘ is either: those are the app's shortcuts.
//
// `held` is the state after the event, which is what a browser reports and
// what the core's key-up expects (releasing an Alt reports alt false). A
// release is never dropped for ⌘ being held: a key let go under ⌘ that had
// been sent down must still come up, or the core holds AKD high for ever.
std::optional<CoreKeyEvent> coreKeyEvent(HostKey key, const HeldModifiers &held,
                                         bool commandIsOpenApple, bool release);

} // namespace a2e::native
