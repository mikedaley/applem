/*
 * test_native_equalizer.cpp - The native app's output equaliser
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "../src/equalizer.hpp"

#include <cmath>
#include <vector>

using namespace a2e::native;

namespace {

constexpr double RATE = 48000.0;
constexpr double PI = 3.14159265358979323846;

// The RMS of a sine at `hz` after the equaliser has settled, in dB relative
// to the sine's own RMS.
double gainDbAt(Equalizer &eq, double hz) {
  const size_t frames = 48000;
  std::vector<float> stereo(frames * 2);
  for (size_t i = 0; i < frames; i++) {
    const float v = static_cast<float>(0.25 * std::sin(2.0 * PI * hz * i / RATE));
    stereo[i * 2] = v;
    stereo[i * 2 + 1] = v;
  }
  eq.process(stereo.data(), frames);
  double sum = 0.0;
  const size_t from = frames / 2; // past the filters' settling
  for (size_t i = from; i < frames; i++) sum += stereo[i * 2] * stereo[i * 2];
  const double rms = std::sqrt(sum / (frames - from));
  return 20.0 * std::log10(rms / (0.25 / std::sqrt(2.0)));
}

} // namespace

TEST_CASE("Off, or flat, the equaliser passes the mix through untouched", "[equalizer]") {
  Equalizer eq(RATE);
  std::vector<float> stereo = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f};
  const std::vector<float> original = stereo;

  eq.process(stereo.data(), 3);
  CHECK(stereo == original);

  Equalizer::Settings flat;
  flat.enabled = true;
  eq.set(flat);
  eq.process(stereo.data(), 3);
  for (size_t i = 0; i < stereo.size(); i++) CHECK(stereo[i] == Approx(original[i]).margin(1e-6));
}

TEST_CASE("A band's gain lands on its centre frequency and leaves the far bands alone", "[equalizer]") {
  Equalizer::Settings s;
  s.enabled = true;
  s.gainDb[5] = 12.0f; // 1kHz
  Equalizer eq(RATE);
  eq.set(s);
  CHECK(gainDbAt(eq, 1000.0) == Approx(12.0).margin(0.3));
  CHECK(gainDbAt(eq, 31.25) == Approx(0.0).margin(0.3));
  CHECK(gainDbAt(eq, 16000.0) == Approx(0.0).margin(0.3));

  s.gainDb[5] = -12.0f;
  eq.set(s);
  CHECK(gainDbAt(eq, 1000.0) == Approx(-12.0).margin(0.3));
}

TEST_CASE("The preamp scales everything", "[equalizer]") {
  Equalizer::Settings s;
  s.enabled = true;
  s.preampDb = -6.0f;
  Equalizer eq(RATE);
  eq.set(s);
  CHECK(gainDbAt(eq, 100.0) == Approx(-6.0).margin(0.1));
  CHECK(gainDbAt(eq, 5000.0) == Approx(-6.0).margin(0.1));
}

TEST_CASE("The drawn response is what the filters do", "[equalizer]") {
  Equalizer::Settings s;
  s.enabled = true;
  s.gainDb[2] = 9.0f;   // 125Hz
  s.gainDb[7] = -6.0f;  // 4kHz
  s.preampDb = -3.0f;
  Equalizer eq(RATE);
  eq.set(s);
  for (double hz : {60.0, 125.0, 500.0, 2000.0, 4000.0, 10000.0}) {
    INFO(hz << " Hz");
    CHECK(gainDbAt(eq, hz) == Approx(Equalizer::responseDb(s, RATE, hz)).margin(0.3));
  }
  CHECK(Equalizer::responseDb(Equalizer::Settings{}, RATE, 1000.0) == 0.0);
}

TEST_CASE("Settings are clamped to the range and read back", "[equalizer]") {
  Equalizer::Settings s;
  s.enabled = true;
  s.preampDb = 40.0f;
  s.gainDb[0] = -40.0f;
  Equalizer eq(RATE);
  eq.set(s);
  const Equalizer::Settings back = eq.settings();
  CHECK(back.enabled);
  CHECK(back.preampDb == Equalizer::RANGE_DB);
  CHECK(back.gainDb[0] == -Equalizer::RANGE_DB);
}
