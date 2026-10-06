/*
 * joystick.hpp - The game port: its device, what drives it, and the Joystick window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "controls/game_port.hpp"

#include "imgui.h"

#include <array>
#include <deque>
#include <functional>
#include <vector>

namespace a2e::native {

class Emulation;

// The browser build's Joystick window and gamepad handling (joystick-window.js,
// gamepad-handler.js, game-port.js).
//
// The connector takes one device: the Apple joystick (two paddles and three
// buttons) or Sirius's Joyport (two digital sticks). What drives it is the
// on-screen stick, the cursor keys when that is turned on, and gamepads; each
// frame they are merged and only a change is sent, because the pushbutton
// lines are also the Apple keys, and sending "not pressed" every frame would
// let go of an Apple key the keyboard is holding.
class Joystick {
public:
  explicit Joystick(Emulation &emulation);

  // The connected gamepads, in the order they connected.
  void setGamepadSource(std::function<std::vector<Pad>()> source) { gamepads_ = std::move(source); }

  // The machine was rebuilt (or first built): it starts on an Apple joystick
  // and has to be told the device again.
  void machineRebuilt();

  // Every frame. `screenHasKeyboard` says whether the arrows are the
  // machine's to have; they still reach its keyboard as well.
  void update(bool screenHasKeyboard);
  void draw(bool *open);

  // Settings.
  int device = 0; // 0 the Apple joystick, 1 the Joyport
  bool cursorKeys = false;
  bool gamepadEnabled = true;
  float deadzone = DEFAULT_DEADZONE;

private:
  // What is driving the stick this frame, for the window to say.
  enum class Source { None, Mouse, Keys, Gamepad };

  void sendDevice();
  void drawAppleJoystick();
  void drawJoyport();
  void drawCX40(int stick, ImVec2 origin, float size);
  void drawInputs();

  Emulation &emulation_;
  std::function<std::vector<Pad>()> gamepads_;
  std::vector<Pad> pads_;

  // The on-screen stick, 0..1 each way, and whether it is being held.
  float knobX_ = 0.5f;
  float knobY_ = 0.5f;
  bool dragging_ = false;
  std::array<bool, 3> screenButtons_{};
  // Where the knob has been lately, for a trail that fades behind it.
  std::deque<std::pair<ImVec2, double>> trail_;
  // A Joyport stick pushed with the mouse: its direction switches.
  std::array<int, 2> screenSticks_{};
  Source source_ = Source::None;
  std::array<Source, 2> stickSources_{{Source::None, Source::None}};

  // What the core was last told, so only changes are sent.
  int sentDevice_ = -1;
  std::array<int, 2> sentPaddles_{{-1, -1}};
  std::array<int, 3> sentButtons_{{-1, -1, -1}};
  std::array<int, 2> sentSticks_{{-1, -1}};

  // What is being sent now, for the window to show.
  std::array<int, 2> paddles_{{128, 128}};
  std::array<bool, 3> buttons_{};
  std::array<int, 2> sticks_{};
};

} // namespace a2e::native
