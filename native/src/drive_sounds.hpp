/*
 * drive_sounds.hpp - The Disk II's stepper, heard as the head moves
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace a2e::native {

// The browser build's seek click (drive-sounds.js): 25ms of a metallic tick,
// a harmonic, a body resonance and a noise transient, through a 6kHz low
// pass, at 0.3 x 0.8 of the main volume. It is rendered once, here, and
// mixed into the output by the audio callback, which must not lock: a click
// is asked for by bumping a counter the callback watches.
class DriveSounds {
public:
  explicit DriveSounds(double sampleRate);

  // From any thread.
  void playSeek() { requested_.fetch_add(1, std::memory_order_relaxed); }
  void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // From the audio callback: add the click, if one is playing, to an
  // interleaved stereo buffer at `gain` (the main volume, zero when muted).
  void mix(float *stereo, size_t frames, float gain);

private:
  std::vector<float> click_;
  std::atomic<unsigned> requested_{0};
  std::atomic<bool> enabled_{true};
  unsigned started_ = 0;
  size_t position_ = 0;
  bool playing_ = false;
};

} // namespace a2e::native
