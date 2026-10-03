/*
 * ensoniq_window.hpp - The Ensoniq window: a IIgs's 5503 DOC and its sound RAM
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "machine_poll.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace a2e::native {

class Emulation;

// The IIgs's sound chip, as the Mockingboard window shows a card: the chip
// and the window onto it at $C03C-$C03F, its 64K of sound RAM with every
// playing table marked on it, and its thirty-two oscillators in the pairs
// that swap and sync act on. Each oscillator shows whether it runs, its
// mode and interrupt, the pitch its table plays at, its volume, where its
// table is, the table itself drawn from the RAM with the playhead on it,
// and a button to leave it out of the mix.
//
// The chip is read once a frame under one lock into a snapshot, sound RAM
// included (64K is a copy, not a walk), and drawn from that.
class EnsoniqWindow {
public:
  explicit EnsoniqWindow(Emulation &emulation);

  // Every frame, open or not: whether there is a chip (a IIgs is running),
  // and the mutes put back on a chip that is new, since a rebuilt machine
  // starts with every oscillator heard.
  void update();
  bool available() const { return present_; }
  void draw(bool *open);

  // Which oscillators are muted, a bit each. Remembered between runs.
  uint32_t mutes = 0;

private:
  struct Oscillator {
    uint16_t frequency = 0;
    uint8_t volume = 0;
    uint8_t control = 0;
    uint8_t tableSize = 0;
    uint8_t data = 0x80;
    uint32_t start = 0;     // the table's first byte in sound RAM
    uint32_t length = 256;  // bytes
    int resolution = 0;
    uint32_t position = 0;  // where it has got to in the table
    bool interruptPending = false;
  };

  void take();
  void setMute(int index, bool muted);
  void drawChip(float width);
  void drawRam(float width);
  void drawOscillators(float width);
  void drawOscillator(int index, float width, float height);

  Emulation &emulation_;
  MachinePoll updatePoll_;
  MachinePoll takePoll_;
  bool present_ = false;
  // The chip the mutes were last applied to, to tell a new one.
  const void *chip_ = nullptr;

  std::array<Oscillator, 32> oscillators_{};
  std::vector<uint8_t> ram_;
  int enabled_ = 1;
  double sampleRate_ = 0;
  bool irq_ = false;
  uint8_t interruptRegister_ = 0xFF;
  uint8_t control_ = 0;
  uint16_t address_ = 0;
  bool showAll_ = false;
};

} // namespace a2e::native
