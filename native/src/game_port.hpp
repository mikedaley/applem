/*
 * game_port.hpp - Gamepads, cursor keys and the on-screen stick, as the core hears them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace a2e::native {

// The browser's mapping (game-port.js, gamepad-handler.js), with its numbers.

// Joyport switch bits, as Joyport::SwitchBit in the core numbers them.
enum JoyportSwitch : int { SWITCH_UP = 1, SWITCH_DOWN = 2, SWITCH_LEFT = 4, SWITCH_RIGHT = 8, SWITCH_FIRE = 16 };

// How far a thumbstick must go before a digital switch closes: well clear of
// the deadzone, because a modern stick rests noisily near centre.
constexpr float DIGITAL_THRESHOLD = 0.5f;
constexpr float DEFAULT_DEADZONE = 0.1f;
constexpr float MAX_DEADZONE = 0.5f;

// A gamepad in the W3C standard layout the browser reads: the left stick on
// axes 0 and 1 (down is positive), A and B on buttons 0 and 1, the D-pad on
// 12 to 15.
struct Pad {
  std::array<float, 4> axes{};
  std::array<bool, 17> buttons{};
  std::string name; // as the system names it, for the window
};
enum PadButton { PAD_A = 0, PAD_B = 1, PAD_UP = 12, PAD_DOWN = 13, PAD_LEFT = 14, PAD_RIGHT = 15 };

// A real stick cannot close left and right at once, so an impossible pair is
// dropped rather than sent: a program that saw both would take whichever it
// tested first.
int switchesFromDirections(bool up, bool down, bool left, bool right, bool fire);

// Inside the deadzone is zero; the range beyond it is rescaled to 0..1.
float applyDeadzone(float value, float deadzone);

// An axis (-1..1) as a paddle reading (0..255).
int paddleFromAxis(float axis, float deadzone);

// A CX40 has four switches and one button, so the D-pad and the left stick
// both close the same lines; either will do. Fire is A or B.
int joyportMask(const Pad &pad, float deadzone);

// The cursor keys as a stick: full deflection each way, centred when let go.
int paddleFromKeys(bool low, bool high);
int joyportMaskFromKeys(bool up, bool down, bool left, bool right);

} // namespace a2e::native
