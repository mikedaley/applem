/*
 * equalizer_window.cpp - The Equalizer window: tone controls over the output
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "equalizer_window.hpp"

#include "emulation.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace a2e::native {

namespace {

constexpr float WIDTH = 560;
constexpr float RESPONSE_HEIGHT = 150;
constexpr float SLIDER_HEIGHT = 150;
constexpr float CARD_ROUNDING = 10;
constexpr float PAD = 14;
constexpr double RATE = Emulation::SAMPLE_RATE;
constexpr double LOWEST_HZ = 20.0;
constexpr double HIGHEST_HZ = 20000.0;

constexpr const char *BAND_LABELS[Equalizer::BANDS] = {"31", "63", "125", "250", "500",
                                                       "1k", "2k", "4k", "8k", "16k"};

struct Preset {
  const char *name;
  float gainDb[Equalizer::BANDS];
};

// A few starting points. Flat is a reset.
constexpr Preset PRESETS[] = {
    {"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Bass Boost", {6, 5, 4, 2, 0, 0, 0, 0, 0, 0}},
    {"Treble Boost", {0, 0, 0, 0, 0, 1, 2, 4, 5, 6}},
    {"Loudness", {5, 4, 2, 0, -1, -1, 0, 2, 4, 5}},
    {"Presence", {0, 0, 0, 0, 1, 3, 4, 3, 1, 0}},
    {"Small Speaker", {-12, -9, -4, 0, 2, 3, 2, -2, -6, -10}},
};

ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }
ImU32 well() { return ui::isDark() ? IM_COL32(0, 0, 0, 90) : IM_COL32(0, 0, 0, 18); }
ImU32 grid() { return ui::isDark() ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 20); }

void caption(ImDrawList *draw, ImVec2 at, const char *label, ImU32 colour) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
  draw->AddText(at, colour, label);
  ImGui::PopFont();
}

// Where a frequency sits across a log axis.
float xForHz(double hz, float left, float width) {
  const double t = std::log(hz / LOWEST_HZ) / std::log(HIGHEST_HZ / LOWEST_HZ);
  return left + width * static_cast<float>(std::clamp(t, 0.0, 1.0));
}

} // namespace

void EqualizerWindow::apply() {
  emulation_.equalizer().set(settings);
  changed_ = true;
}

// The curve the settings make, over a grid of octaves and 6dB steps.
void EqualizerWindow::drawResponse(float width, float height) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 end(origin.x + width, origin.y + height);
  draw->AddRectFilled(origin, end, well(), CARD_ROUNDING);

  const float top = origin.y + 10, bottom = end.y - 16;
  const float left = origin.x + 28, right = end.x - 10;
  auto yForDb = [&](double db) {
    const double t = (db + Equalizer::RANGE_DB) / (2.0 * Equalizer::RANGE_DB);
    return bottom - (bottom - top) * static_cast<float>(std::clamp(t, 0.0, 1.0));
  };

  char label[16];
  for (int db = -12; db <= 12; db += 6) {
    const float y = yForDb(db);
    draw->AddLine(ImVec2(left, y), ImVec2(right, y), db == 0 ? grid() : grid(), db == 0 ? 1.5f : 1.0f);
    std::snprintf(label, sizeof(label), "%+d", db);
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.72f);
    const ImVec2 size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(left - 6 - size.x, y - size.y * 0.5f), secondary(), label);
    ImGui::PopFont();
  }
  for (int i = 0; i < Equalizer::BANDS; i++) {
    const float x = xForHz(Equalizer::FREQUENCIES[i], left, right - left);
    draw->AddLine(ImVec2(x, top), ImVec2(x, bottom), grid());
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.72f);
    const ImVec2 size = ImGui::CalcTextSize(BAND_LABELS[i]);
    draw->AddText(ImVec2(x - size.x * 0.5f, bottom + 3), secondary(), BAND_LABELS[i]);
    ImGui::PopFont();
  }

  // The curve, filled to 0dB so a boost reads as a hill and a cut as a dip.
  const int points = 160;
  const float zero = yForDb(0);
  ImVec2 previous;
  for (int p = 0; p <= points; p++) {
    const double hz = LOWEST_HZ * std::pow(HIGHEST_HZ / LOWEST_HZ, static_cast<double>(p) / points);
    const ImVec2 at(xForHz(hz, left, right - left), yForDb(Equalizer::responseDb(settings, RATE, hz)));
    if (p > 0) {
      draw->AddQuadFilled(ImVec2(previous.x, zero), previous, at, ImVec2(at.x, zero), accent(settings.enabled ? 0.18f : 0.06f));
      draw->AddLine(previous, at, settings.enabled ? accent() : secondary(), 2.0f);
    }
    previous = at;
  }
  ImGui::Dummy(ImVec2(width, height));
}

// A slider a band, its gain above and its frequency below.
void EqualizerWindow::drawBands() {
  const float gap = 8;
  const float column = (WIDTH - gap * (Equalizer::BANDS - 1)) / Equalizer::BANDS;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
  const float textHeight = ImGui::GetTextLineHeight();
  ImGui::PopFont();

  for (int i = 0; i < Equalizer::BANDS; i++) {
    const float x = start.x + i * (column + gap);
    char value[16];
    std::snprintf(value, sizeof(value), "%+.0f", std::round(settings.gainDb[i]));
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
    const ImVec2 size = ImGui::CalcTextSize(value);
    draw->AddText(ImVec2(x + (column - size.x) * 0.5f, start.y), settings.gainDb[i] != 0 ? ui::accentText() : secondary(), value);
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(x, start.y + textHeight + 4));
    ImGui::PushID(i);
    ImGui::BeginDisabled(!settings.enabled);
    if (ui::VSliderFloat("##band", &settings.gainDb[i], -Equalizer::RANGE_DB, Equalizer::RANGE_DB,
                         ImVec2(column, SLIDER_HEIGHT), 0.0f)) {
      apply();
    }
    ImGui::EndDisabled();
    ImGui::PopID();

    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
    const ImVec2 labelSize = ImGui::CalcTextSize(BAND_LABELS[i]);
    draw->AddText(ImVec2(x + (column - labelSize.x) * 0.5f, start.y + textHeight + 4 + SLIDER_HEIGHT + 2), secondary(), BAND_LABELS[i]);
    ImGui::PopFont();
  }
  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + textHeight + 4 + SLIDER_HEIGHT + 2 + textHeight + 6));
}

// The preamp, the presets and the switch.
void EqualizerWindow::drawFooter(float width) {
  ImGui::BeginDisabled(!settings.enabled);
  ImGui::SetNextItemWidth(width * 0.45f);
  if (ui::SliderFloat("Preamp", &settings.preampDb, -Equalizer::RANGE_DB, Equalizer::RANGE_DB, "%+.0f dB")) {
    apply();
  }
  ImGui::SameLine(0, PAD);
  int chosen = -1;
  for (int i = 0; i < static_cast<int>(std::size(PRESETS)); i++) {
    if (std::equal(std::begin(PRESETS[i].gainDb), std::end(PRESETS[i].gainDb), settings.gainDb.begin())) chosen = i;
  }
  if (ui::BeginPopUpButton("##preset", chosen >= 0 ? PRESETS[chosen].name : "Custom", 150)) {
    for (int i = 0; i < static_cast<int>(std::size(PRESETS)); i++) {
      if (ImGui::Selectable(PRESETS[i].name, chosen == i)) {
        std::copy(std::begin(PRESETS[i].gainDb), std::end(PRESETS[i].gainDb), settings.gainDb.begin());
        apply();
      }
    }
    ui::EndPopUpButton();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  const float switchWidth = ui::SwitchWidth("Enabled");
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - switchWidth));
  if (ui::Switch("Enabled", &settings.enabled)) apply();
}

void EqualizerWindow::draw(bool *open) {
  changed_ = false;
  if (!open || !*open) return;
  ui::BeforeWindow("Equalizer");
  if (ui::BeginWindow("Equalizer", open, ImGuiWindowFlags_AlwaysAutoResize)) {
    caption(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), "RESPONSE", secondary());
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.72f + 2));
    drawResponse(WIDTH, RESPONSE_HEIGHT);
    ImGui::Dummy(ImVec2(0, 6));
    drawBands();
    ImGui::Dummy(ImVec2(0, 2));
    drawFooter(WIDTH);
  }
  ImGui::End();
}

} // namespace a2e::native
