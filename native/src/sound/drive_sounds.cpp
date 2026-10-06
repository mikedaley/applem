/*
 * drive_sounds.cpp - The Disk II's stepper, heard as the head moves
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "sound/drive_sounds.hpp"

#include <cmath>
#include <random>

namespace a2e::native {

namespace {

// The browser's parameters (drive-sounds.js).
constexpr double DURATION = 0.025;
constexpr double PRIMARY_FREQ = 2200;
constexpr double SECONDARY_FREQ = 3800;
constexpr double BODY_FREQ = 1200;
constexpr double DECAY = 350;
constexpr double CLICK_DECAY = 1200;
constexpr float SEEK_VOLUME = 0.3f;
constexpr double LOWPASS_HZ = 6000;
constexpr double LOWPASS_Q = 0.5;

} // namespace

DriveSounds::DriveSounds(double sampleRate) {
  const size_t count = static_cast<size_t>(std::ceil(sampleRate * DURATION));
  click_.resize(count);
  std::mt19937 random(0xA2E);
  std::uniform_real_distribution<double> noise(-1.0, 1.0);
  for (size_t i = 0; i < count; i++) {
    const double t = static_cast<double>(i) / sampleRate;
    const double envelope = std::exp(-t * DECAY);
    const double click = std::exp(-t * CLICK_DECAY) * noise(random) * 0.4;
    const double tick = std::sin(2 * M_PI * PRIMARY_FREQ * t) * 0.5;
    const double harmonic = std::sin(2 * M_PI * SECONDARY_FREQ * t) * 0.25;
    const double body = std::exp(-t * (DECAY + 150)) * std::sin(2 * M_PI * BODY_FREQ * t) * 0.3;
    click_[i] = static_cast<float>(envelope * (tick + harmonic) + body * envelope + click);
  }

  // The Web Audio BiquadFilterNode's low pass (the RBJ cookbook's), applied
  // once rather than per click.
  const double w0 = 2 * M_PI * LOWPASS_HZ / sampleRate;
  const double alpha = std::sin(w0) / (2 * LOWPASS_Q);
  const double cosw = std::cos(w0);
  const double a0 = 1 + alpha;
  const double b0 = (1 - cosw) / 2 / a0, b1 = (1 - cosw) / a0, b2 = (1 - cosw) / 2 / a0;
  const double a1 = -2 * cosw / a0, a2 = (1 - alpha) / a0;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (float &sample : click_) {
    const double x = sample;
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    sample = static_cast<float>(y) * SEEK_VOLUME * 0.8f;
  }
}

void DriveSounds::mix(float *stereo, size_t frames, float gain) {
  const unsigned requested = requested_.load(std::memory_order_relaxed);
  if (requested != started_) {
    started_ = requested;
    position_ = 0;
    playing_ = enabled_.load(std::memory_order_relaxed);
  }
  if (!playing_) return;
  for (size_t i = 0; i < frames && position_ < click_.size(); i++, position_++) {
    const float sample = click_[position_] * gain;
    stereo[i * 2] += sample;
    stereo[i * 2 + 1] += sample;
  }
  if (position_ >= click_.size()) playing_ = false;
}

} // namespace a2e::native
