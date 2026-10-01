/*
 * game_port.cpp - Gamepads, cursor keys and the on-screen stick, as the core hears them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "game_port.hpp"

#include <cmath>

namespace a2e::native {

int switchesFromDirections(bool up, bool down, bool left, bool right, bool fire) {
  int mask = 0;
  if (up && !down) mask |= SWITCH_UP;
  if (down && !up) mask |= SWITCH_DOWN;
  if (left && !right) mask |= SWITCH_LEFT;
  if (right && !left) mask |= SWITCH_RIGHT;
  if (fire) mask |= SWITCH_FIRE;
  return mask;
}

float applyDeadzone(float value, float deadzone) {
  if (std::fabs(value) < deadzone) return 0.0f;
  const float sign = value > 0 ? 1.0f : -1.0f;
  return sign * (std::fabs(value) - deadzone) / (1.0f - deadzone);
}

int paddleFromAxis(float axis, float deadzone) {
  const float normal = (applyDeadzone(axis, deadzone) + 1.0f) / 2.0f;
  return static_cast<int>(std::lround(normal * 255.0f));
}

int joyportMask(const Pad &pad, float deadzone) {
  const float x = applyDeadzone(pad.axes[0], deadzone);
  const float y = applyDeadzone(pad.axes[1], deadzone);
  return switchesFromDirections(pad.buttons[PAD_UP] || y <= -DIGITAL_THRESHOLD,
                                pad.buttons[PAD_DOWN] || y >= DIGITAL_THRESHOLD,
                                pad.buttons[PAD_LEFT] || x <= -DIGITAL_THRESHOLD,
                                pad.buttons[PAD_RIGHT] || x >= DIGITAL_THRESHOLD,
                                pad.buttons[PAD_A] || pad.buttons[PAD_B]);
}

int paddleFromKeys(bool low, bool high) {
  if (low && !high) return 0;
  if (high && !low) return 255;
  return 128;
}

int joyportMaskFromKeys(bool up, bool down, bool left, bool right) {
  return switchesFromDirections(up, down, left, right, false);
}

} // namespace a2e::native
