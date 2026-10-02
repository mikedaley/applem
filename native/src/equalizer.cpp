/*
 * equalizer.cpp - A graphic equaliser over the mixed output
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "equalizer.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace a2e::native {

namespace {

constexpr double PI = 3.14159265358979323846;

double fromDb(double db) { return std::pow(10.0, db / 20.0); }

} // namespace

Equalizer::Equalizer(double sampleRate) : sampleRate_(sampleRate) {
  for (auto &g : gainDb_) g.store(0.0f, std::memory_order_relaxed);
}

void Equalizer::set(const Settings &settings) {
  enabled_.store(settings.enabled, std::memory_order_relaxed);
  preampDb_.store(std::clamp(settings.preampDb, -RANGE_DB, RANGE_DB), std::memory_order_relaxed);
  for (int i = 0; i < BANDS; i++) {
    gainDb_[i].store(std::clamp(settings.gainDb[i], -RANGE_DB, RANGE_DB), std::memory_order_relaxed);
  }
  version_.fetch_add(1, std::memory_order_release);
}

Equalizer::Settings Equalizer::settings() const {
  Settings s;
  s.enabled = enabled_.load(std::memory_order_relaxed);
  s.preampDb = preampDb_.load(std::memory_order_relaxed);
  for (int i = 0; i < BANDS; i++) s.gainDb[i] = gainDb_[i].load(std::memory_order_relaxed);
  return s;
}

// The peaking filter from the Audio EQ Cookbook (Bristow-Johnson), with the
// gain split between numerator and denominator so a cut is a boost's mirror.
Equalizer::Coefficients Equalizer::peaking(double sampleRate, double hz, double gainDb) {
  Coefficients c;
  if (std::fabs(gainDb) < 0.01 || hz >= sampleRate * 0.5) return c;
  const double A = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * PI * hz / sampleRate;
  const double alpha = std::sin(w0) / (2.0 * Q);
  const double cosw = std::cos(w0);
  const double a0 = 1.0 + alpha / A;
  c.b0 = (1.0 + alpha * A) / a0;
  c.b1 = -2.0 * cosw / a0;
  c.b2 = (1.0 - alpha * A) / a0;
  c.a1 = -2.0 * cosw / a0;
  c.a2 = (1.0 - alpha / A) / a0;
  c.active = true;
  return c;
}

void Equalizer::recompute() {
  const bool wasActive = active_;
  active_ = enabled_.load(std::memory_order_relaxed);
  preamp_ = static_cast<float>(fromDb(preampDb_.load(std::memory_order_relaxed)));
  for (int i = 0; i < BANDS; i++) {
    coefficients_[i] = peaking(sampleRate_, FREQUENCIES[i], gainDb_[i].load(std::memory_order_relaxed));
  }
  // Switched on, the filters start from rest rather than from wherever they
  // were when they were switched off.
  if (active_ && !wasActive) {
    for (auto &band : state_) band = {};
  }
}

void Equalizer::process(float *stereo, size_t frames) {
  const uint32_t version = version_.load(std::memory_order_acquire);
  if (version != seen_) {
    seen_ = version;
    recompute();
  }
  if (!active_) return;

  for (size_t i = 0; i < frames * 2; i++) {
    const int channel = static_cast<int>(i & 1);
    double x = static_cast<double>(stereo[i]) * preamp_;
    for (int band = 0; band < BANDS; band++) {
      const Coefficients &c = coefficients_[band];
      if (!c.active) continue;
      State &s = state_[band][channel];
      // Direct form II, transposed.
      const double y = c.b0 * x + s.s1;
      s.s1 = c.b1 * x - c.a1 * y + s.s2;
      s.s2 = c.b2 * x - c.a2 * y;
      x = y;
    }
    // A boost can take the mix past full scale; the device would clip it
    // anyway, and clipping it here keeps the number the meter would show
    // honest.
    stereo[i] = static_cast<float>(std::clamp(x, -1.0, 1.0));
  }
}

double Equalizer::responseDb(const Settings &settings, double sampleRate, double hz) {
  if (!settings.enabled) return 0.0;
  const std::complex<double> z = std::polar(1.0, -2.0 * PI * hz / sampleRate);
  double magnitude = fromDb(settings.preampDb);
  for (int i = 0; i < BANDS; i++) {
    const Coefficients c = peaking(sampleRate, FREQUENCIES[i], settings.gainDb[i]);
    if (!c.active) continue;
    const std::complex<double> numerator = c.b0 + c.b1 * z + c.b2 * z * z;
    const std::complex<double> denominator = 1.0 + c.a1 * z + c.a2 * z * z;
    magnitude *= std::abs(numerator / denominator);
  }
  return 20.0 * std::log10(std::max(magnitude, 1e-9));
}

} // namespace a2e::native
