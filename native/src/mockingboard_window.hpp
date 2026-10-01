/*
 * mockingboard_window.hpp - The Mockingboard window: two AY-3-8910s and their VIAs
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <array>
#include <cstdint>

namespace a2e::native {

class Emulation;

// The browser build's Mockingboard window (mockingboard-window.js): for each
// sound chip, a row per channel with its note, tone and noise, level and a
// live waveform, a button to mute it, the envelope and noise generators, and
// the 6522 that drives the chip.
//
// The card is read once a frame, under one lock, into a snapshot that the
// window then draws from: holding the machine for the length of a draw
// would hold up the emulation thread. The waveforms are generated from
// copies of the chips taken in that snapshot, outside the lock, so drawing
// them neither waits on the machine nor disturbs what it plays.
class MockingboardWindow {
public:
  static constexpr int WAVEFORM_SAMPLES = 256;

  explicit MockingboardWindow(Emulation &emulation);
  ~MockingboardWindow();

  // Every frame, open or not: whether a card is fitted (the menu offers the
  // window only then), and the mutes put back on a card that is new, since
  // a rebuilt machine or a refitted slot starts with every channel on.
  void update();
  bool available() const { return fitted_; }
  void draw(bool *open);

  // Which channels are muted, a bit each: PSG 1's A, B, C, then PSG 2's.
  // Remembered between runs, as the browser keeps them with the window.
  int mutes = 0;

private:
  struct Psg {
    std::array<uint8_t, 16> registers{};
    uint32_t writes = 0;
    uint8_t lastRegister = 0;
    uint8_t lastValue = 0;
    std::array<std::array<float, WAVEFORM_SAMPLES>, 3> waveforms{};
  };
  struct Via {
    uint8_t ora = 0, orb = 0, ddra = 0, ddrb = 0;
    uint8_t acr = 0, ifr = 0, ier = 0;
    uint16_t t1Counter = 0, t1Latch = 0;
    bool t1Running = false, t1Fired = false, irq = false;
  };

  void take();
  void setMute(int psg, int channel, bool muted);
  void drawChip(int index);
  void drawChannel(int index, int channel, float width);
  void drawVia(int index, float width);

  Emulation &emulation_;
  bool fitted_ = false;
  // The card the mutes were last applied to, to tell a new one.
  const void *card_ = nullptr;
  bool enabled_ = false;
  std::array<Psg, 2> psgs_;
  std::array<Via, 2> vias_;
};

} // namespace a2e::native
