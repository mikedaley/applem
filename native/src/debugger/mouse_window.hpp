/*
 * mouse_window.hpp - The Mouse window: an Apple Mouse Card, or a //c's own mouse
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "app/machine_poll.hpp"
#include <array>
#include <cstdint>

namespace a2e::native {

class Emulation;

// The browser build's Mouse Card window (mouse-card-window.js), for whichever
// mouse the machine has that a program talks to through slot firmware:
//
// - a Mouse Card in a //e's or II Plus's slot, or in a IIgs's socket: where
//   the card has the mouse, its clamp window, the mode the firmware set,
//   which interrupts are pending, the 6821 PIA's eight registers and the
//   last command the firmware sent through it;
// - a //c's mouse, which is not a card but the IOU: which interrupts it
//   allows, which are pending, and the travel banked to deliver.
//
// Either way, the slot's screen holes too: the position, status and mode the
// firmware keeps there, which is what a program reads. A IIgs's own mouse is
// on the ADB, has no slot firmware, and is not shown.
//
// The machine is read once a frame, under one lock, into a snapshot the
// window draws from, as the Mockingboard window does.
class MouseWindow {
public:
  explicit MouseWindow(Emulation &emulation);

  // Every frame, open or not: whether there is a mouse to show (the Debug
  // menu offers the window only then).
  void update();
  bool available() const { return kind_ != Kind::None; }
  void draw(bool *open);

private:
  enum class Kind { None, Card, IOU };

  struct Card {
    int slot = 4;
    bool iigs = false;
    bool answering = true; // a IIgs's slot set to Your Card
    int x = 0, y = 0;
    bool button = false, moved = false, buttonChanged = false;
    int clampMinX = 0, clampMaxX = 1023, clampMinY = 0, clampMaxY = 1023;
    uint8_t mode = 0;
    bool irq = false, vblPending = false, movePending = false, buttonPending = false;
    bool inVbl = false;
    // DDRA, ORA, IRA, CRA, then the same for port B.
    std::array<uint8_t, 8> pia{};
    uint8_t lastCommand = 0;
    uint8_t responseState = 0;
  };
  struct Iou {
    bool button = false, shift = false;
    bool movementEnabled = false, vblEnabled = false;
    bool xPending = false, yPending = false, vblPending = false;
    bool irq = false;
    int bankedX = 0, bankedY = 0;
  };
  // The slot's screen holes ($0478+n and on): what the firmware keeps.
  struct Holes {
    int x = 0, y = 0;
    uint8_t status = 0, mode = 0;
  };

  void take();
  void drawCard(float width);
  void drawIou(float width);
  void drawHoles(float width);

  Emulation &emulation_;
  MachinePoll updatePoll_;
  MachinePoll takePoll_;
  Kind kind_ = Kind::None;
  Card card_;
  Iou iou_;
  Holes holes_;
};

} // namespace a2e::native
