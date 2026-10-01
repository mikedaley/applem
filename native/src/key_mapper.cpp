/*
 * key_mapper.cpp - Host keys as the core hears them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "key_mapper.hpp"

namespace a2e::native {

std::optional<HostKey> browserKeyFor(ImGuiKey key, bool macSwap) {
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return HostKey{65 + (key - ImGuiKey_A)};
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return HostKey{48 + (key - ImGuiKey_0)};
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) return HostKey{112 + (key - ImGuiKey_F1)};
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return HostKey{96 + (key - ImGuiKey_Keypad0), LOCATION_NUMPAD};
  }

  // ImGui reports ⌘ as Ctrl and Control as Super on a Mac.
  const int leftCommand = macSwap ? ImGuiKey_LeftCtrl : ImGuiKey_LeftSuper;
  const int rightCommand = macSwap ? ImGuiKey_RightCtrl : ImGuiKey_RightSuper;
  const int leftControl = macSwap ? ImGuiKey_LeftSuper : ImGuiKey_LeftCtrl;
  const int rightControl = macSwap ? ImGuiKey_RightSuper : ImGuiKey_RightCtrl;
  if (key == leftCommand) return HostKey{KEY_META_LEFT, LOCATION_LEFT};
  if (key == rightCommand) return HostKey{KEY_META_RIGHT, LOCATION_RIGHT};
  if (key == leftControl) return HostKey{KEY_CONTROL, LOCATION_LEFT};
  if (key == rightControl) return HostKey{KEY_CONTROL, LOCATION_RIGHT};

  switch (key) {
  case ImGuiKey_LeftShift: return HostKey{KEY_SHIFT, LOCATION_LEFT};
  case ImGuiKey_RightShift: return HostKey{KEY_SHIFT, LOCATION_RIGHT};
  case ImGuiKey_LeftAlt: return HostKey{KEY_ALT, LOCATION_LEFT};
  case ImGuiKey_RightAlt: return HostKey{KEY_ALT, LOCATION_RIGHT};

  case ImGuiKey_Backspace: return HostKey{8};
  case ImGuiKey_Tab: return HostKey{9};
  case ImGuiKey_Enter: return HostKey{13};
  case ImGuiKey_KeypadEnter: return HostKey{13, LOCATION_NUMPAD};
  case ImGuiKey_Pause: return HostKey{19};
  case ImGuiKey_CapsLock: return HostKey{20};
  case ImGuiKey_Escape: return HostKey{27};
  case ImGuiKey_Space: return HostKey{32};
  case ImGuiKey_PageUp: return HostKey{33};
  case ImGuiKey_PageDown: return HostKey{34};
  case ImGuiKey_End: return HostKey{35};
  case ImGuiKey_Home: return HostKey{36};
  case ImGuiKey_LeftArrow: return HostKey{37};
  case ImGuiKey_UpArrow: return HostKey{38};
  case ImGuiKey_RightArrow: return HostKey{39};
  case ImGuiKey_DownArrow: return HostKey{40};
  case ImGuiKey_Insert: return HostKey{45};
  case ImGuiKey_Delete: return HostKey{46};

  case ImGuiKey_KeypadMultiply: return HostKey{106, LOCATION_NUMPAD};
  case ImGuiKey_KeypadAdd: return HostKey{107, LOCATION_NUMPAD};
  case ImGuiKey_KeypadSubtract: return HostKey{109, LOCATION_NUMPAD};
  case ImGuiKey_KeypadDecimal: return HostKey{110, LOCATION_NUMPAD};
  case ImGuiKey_KeypadDivide: return HostKey{111, LOCATION_NUMPAD};
  case ImGuiKey_KeypadEqual: return HostKey{187, LOCATION_NUMPAD};

  case ImGuiKey_Semicolon: return HostKey{186};
  case ImGuiKey_Equal: return HostKey{187};
  case ImGuiKey_Comma: return HostKey{188};
  case ImGuiKey_Minus: return HostKey{189};
  case ImGuiKey_Period: return HostKey{190};
  case ImGuiKey_Slash: return HostKey{191};
  case ImGuiKey_GraveAccent: return HostKey{192};
  case ImGuiKey_LeftBracket: return HostKey{219};
  case ImGuiKey_Backslash: return HostKey{220};
  case ImGuiKey_RightBracket: return HostKey{221};
  case ImGuiKey_Apostrophe: return HostKey{222};
  default: return std::nullopt;
  }
}

// From ImGui's modifier flags rather than the modifier keys' own state: the
// flags come with every key event, so a key sent with Shift in its flags (as
// a synthesised one is) is still shifted though no Shift key went down.
HeldModifiers heldModifiers(bool macSwap) {
  const bool ctrlMod = ImGui::IsKeyDown(ImGuiMod_Ctrl);
  const bool superMod = ImGui::IsKeyDown(ImGuiMod_Super);
  HeldModifiers held;
  held.shift = ImGui::IsKeyDown(ImGuiMod_Shift);
  held.option = ImGui::IsKeyDown(ImGuiMod_Alt);
  held.control = macSwap ? superMod : ctrlMod;
  held.command = macSwap ? ctrlMod : superMod;
  return held;
}

std::optional<CoreKeyEvent> coreKeyEvent(HostKey key, const HeldModifiers &held,
                                         bool commandIsOpenApple, bool release) {
  CoreKeyEvent event;
  event.keyCode = key.keyCode;
  event.location = key.location;
  event.shift = held.shift;
  event.ctrl = held.control;
  const bool isCommand = key.keyCode == KEY_META_LEFT || key.keyCode == KEY_META_RIGHT;

  if (commandIsOpenApple) {
    // ⌘ is Open Apple and either Option is Closed Apple; nothing reaches the
    // core as ⌘.
    if (isCommand) {
      event.keyCode = KEY_ALT;
      event.location = LOCATION_LEFT;
    } else if (key.keyCode == KEY_ALT) {
      event.location = LOCATION_RIGHT;
    }
    event.alt = held.option || held.command;
    event.meta = false;
    return event;
  }

  // ⌘ is the app's: neither the key nor anything typed under it is sent.
  if (isCommand || (held.command && !release)) return std::nullopt;
  event.alt = held.option;
  event.meta = false;
  return event;
}

} // namespace a2e::native
