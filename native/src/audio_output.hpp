/*
 * audio_output.hpp - The default output device, pulling stereo samples
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace a2e::native {

// Core Audio's default output unit at a fixed rate, interleaved stereo float.
//
// The device's clock is what paces the emulation, as the AudioWorklet's is in
// the browser: `pull` is called on Core Audio's real-time thread whenever the
// device wants samples, and must not block, lock or allocate.
class AudioOutput {
public:
  using Pull = std::function<void(float *stereo, size_t frames)>;

  AudioOutput();
  ~AudioOutput();

  AudioOutput(const AudioOutput &) = delete;
  AudioOutput &operator=(const AudioOutput &) = delete;

  // False if there is no device to play through; the caller then has to
  // pace the machine some other way.
  bool start(double sampleRate, Pull pull);
  void stop();
  bool running() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace a2e::native
