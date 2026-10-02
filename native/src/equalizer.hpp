/*
 * equalizer.hpp - A graphic equaliser over the mixed output
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace a2e::native {

// Ten octave bands of peaking filters, a preamp and a bypass, applied to the
// stereo mix on its way to the device: the speaker, a Mockingboard, a IIgs's
// Ensoniq and the drives' sounds all pass through it, because it stands in
// for the tone controls on the amplifier a real machine was plugged into.
//
// The device's callback must never block, so the settings reach it without
// a lock: the UI writes each value into an atomic and then bumps a version,
// and the callback recomputes its coefficients when it sees the version has
// moved. A callback that lands between two of the UI's writes uses a mixed
// set for one buffer and corrects itself on the next.
class Equalizer {
public:
  static constexpr int BANDS = 10;
  // Centre frequencies, an octave apart, in Hz.
  static constexpr std::array<float, BANDS> FREQUENCIES = {
      31.25f, 62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};
  static constexpr float RANGE_DB = 12.0f;
  // An octave wide, so the bands meet their neighbours at half gain.
  static constexpr float Q = 1.41f;

  struct Settings {
    bool enabled = false;
    float preampDb = 0.0f;
    std::array<float, BANDS> gainDb{};
  };

  explicit Equalizer(double sampleRate);

  // The UI thread's side.
  void set(const Settings &settings);
  Settings settings() const;

  // The audio thread's side: in place, interleaved stereo.
  void process(float *stereo, size_t frames);

  // The gain the settings give a frequency, in dB: what the window draws.
  static double responseDb(const Settings &settings, double sampleRate, double hz);

private:
  struct Coefficients {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    bool active = false;
  };
  struct State {
    double s1 = 0, s2 = 0;
  };

  static Coefficients peaking(double sampleRate, double hz, double gainDb);

  void recompute();

  const double sampleRate_;
  std::atomic<uint32_t> version_{1};
  std::atomic<bool> enabled_{false};
  std::atomic<float> preampDb_{0.0f};
  std::array<std::atomic<float>, BANDS> gainDb_{};

  // The audio thread's own copies.
  uint32_t seen_ = 0;
  bool active_ = false;
  float preamp_ = 1.0f;
  std::array<Coefficients, BANDS> coefficients_{};
  std::array<std::array<State, 2>, BANDS> state_{};
};

} // namespace a2e::native
