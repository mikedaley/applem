/*
 * mockingboard_window.cpp - The Mockingboard window: two AY-3-8910s and their VIAs
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "mockingboard_window.hpp"

#include "emulation.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "cards/mockingboard/mockingboard_card.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace a2e::native {

namespace {

// The window, in points.
constexpr float WIDTH = 660;
constexpr float CARD_ROUNDING = 10;
constexpr float ROW_HEIGHT = 40;
constexpr float PAD = 14;

// The AY and the 6522 both run from the bus's clock.
constexpr double CLOCK = 1023000.0;
constexpr int SAMPLE_RATE = 48000;

// The browser's channel colours: a badge, and a brighter trace and meter.
constexpr ImU32 BADGE[3] = {IM_COL32(0x00, 0x6F, 0xA3, 255), IM_COL32(0x3D, 0x8F, 0x2B, 255),
                            IM_COL32(0xB4, 0x2D, 0x31, 255)};
constexpr ImU32 TRACE[3] = {IM_COL32(0x18, 0xAB, 0xEA, 255), IM_COL32(0x6E, 0xC9, 0x4F, 255),
                            IM_COL32(0xE5, 0x50, 0x4F, 255)};
constexpr ImU32 GREEN = IM_COL32(97, 187, 70, 255);
constexpr ImU32 RED = IM_COL32(224, 58, 62, 255);
constexpr ImU32 ORANGE = IM_COL32(245, 130, 31, 255);
constexpr ImU32 PURPLE = IM_COL32(0xB0, 0x5C, 0xB1, 255);

constexpr const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

void card(ImDrawList *draw, ImVec2 a, ImVec2 b) {
  draw->AddRectFilled(a, b, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

// A well for a trace or a meter: darker than the card in either appearance.
ImU32 well() { return ui::isDark() ? IM_COL32(0, 0, 0, 90) : IM_COL32(0, 0, 0, 18); }

// Small capitals over a reading, as the Joystick window labels its dials.
void caption(ImDrawList *draw, ImVec2 at, const char *label, ImU32 colour) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
  draw->AddText(at, colour, label);
  ImGui::PopFont();
}

// A capsule with a word in it, lit or not. Returns its width.
float pill(ImDrawList *draw, ImVec2 at, const char *label, bool lit, ImU32 colour) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.78f);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const ImVec2 end(at.x + size.x + 10, at.y + size.y + 4);
  draw->AddRectFilled(at, end, lit ? colour : text(0.07f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 5, at.y + 2), lit ? ui::textOn(colour) : secondary(), label);
  ImGui::PopFont();
  return end.x - at.x;
}

// A label and a value in the mono face, the value coloured. Returns the width.
float field(ImDrawList *draw, ImVec2 at, const char *label, const char *value, ImU32 colour) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.85f);
  draw->AddText(at, secondary(), label);
  const float lw = ImGui::CalcTextSize(label).x + 4;
  draw->AddText(ImVec2(at.x + lw, at.y), colour, value);
  const float width = lw + ImGui::CalcTextSize(value).x;
  ImGui::PopFont();
  return width;
}

// A speaker, with sound coming out of it or a cross where the sound was.
void speaker(ImDrawList *draw, ImVec2 c, bool muted, ImU32 colour) {
  draw->AddRectFilled(ImVec2(c.x - 6, c.y - 2.5f), ImVec2(c.x - 3, c.y + 2.5f), colour);
  draw->AddTriangleFilled(ImVec2(c.x - 3, c.y - 2.5f), ImVec2(c.x + 1.5f, c.y - 6.5f), ImVec2(c.x + 1.5f, c.y + 6.5f), colour);
  draw->AddTriangleFilled(ImVec2(c.x - 3, c.y - 2.5f), ImVec2(c.x + 1.5f, c.y + 6.5f), ImVec2(c.x - 3, c.y + 2.5f), colour);
  if (muted) {
    draw->AddLine(ImVec2(c.x + 3.5f, c.y - 3), ImVec2(c.x + 8.5f, c.y + 3), colour, 1.6f);
    draw->AddLine(ImVec2(c.x + 8.5f, c.y - 3), ImVec2(c.x + 3.5f, c.y + 3), colour, 1.6f);
  } else {
    draw->PathArcTo(ImVec2(c.x + 1.5f, c.y), 5.0f, -0.9f, 0.9f, 8);
    draw->PathStroke(colour, 0, 1.4f);
  }
}

// One of the chip's sixteen envelope shapes, as the chip runs it: three
// periods of the level, from its four control bits.
void envelopeGlyph(ImDrawList *draw, ImVec2 a, ImVec2 b, uint8_t shape, ImU32 colour) {
  const bool hold = shape & 0x01, alternate = shape & 0x02, attack = shape & 0x04, cont = shape & 0x08;
  const float w = (b.x - a.x) / 3.0f;
  const float low = b.y, high = a.y;
  ImVec2 points[8];
  int n = 0;
  bool up = attack;
  points[n++] = ImVec2(a.x, up ? low : high);
  float x = a.x;
  bool held = false;
  float heldAt = 0;
  for (int segment = 0; segment < 3; segment++) {
    if (held) {
      points[n++] = ImVec2(x + w, heldAt);
      x += w;
      continue;
    }
    points[n++] = ImVec2(x + w, up ? high : low);
    x += w;
    if (segment == 2) break;
    // At the end of a period: stop at zero, hold, or go round again.
    float next;
    if (!cont) {
      next = low;
      held = true;
    } else if (hold) {
      next = alternate ? (up ? low : high) : (up ? high : low);
      held = true;
    } else {
      if (alternate) up = !up;
      next = up ? low : high;
    }
    if (held) heldAt = next;
    points[n++] = ImVec2(x, next);
  }
  draw->AddPolyline(points, n, colour, 0, 1.5f);
}

// What the 6522's port B tells the chip: BC1 and BDIR, and RESET held low.
// A line set as an input floats high, so only an output can reset the chip.
const char *busFunction(uint8_t orb, uint8_t ddrb) {
  if ((ddrb & 0x04) && !(orb & 0x04)) return "RESET";
  static const char *const names[4] = {"INACTIVE", "READ", "WRITE", "LATCH"};
  return names[orb & ddrb & 0x03];
}

} // namespace

MockingboardWindow::MockingboardWindow(Emulation &emulation) : emulation_(emulation) {}

MockingboardWindow::~MockingboardWindow() = default;

void MockingboardWindow::update() {
  if (!emulation_.poll(updatePoll_, [&](host::MachineHost &host) {
    MockingboardCard *mb = host.mockingboard();
    fitted_ = mb != nullptr;
    if (!mb || mb == card_) return;
    card_ = mb;
    for (int psg = 0; psg < 2; psg++) {
      AY8910 &chip = psg == 0 ? mb->getPSG1() : mb->getPSG2();
      for (int ch = 0; ch < 3; ch++) chip.setChannelMute(ch, mutes & (1 << (psg * 3 + ch)));
    }
  })) {
    return;
  }
  if (!fitted_) card_ = nullptr;
}

void MockingboardWindow::setMute(int psg, int channel, bool muted) {
  const int bit = 1 << (psg * 3 + channel);
  mutes = muted ? (mutes | bit) : (mutes & ~bit);
  emulation_.withMachine([&](host::MachineHost &host) {
    if (MockingboardCard *mb = host.mockingboard()) {
      (psg == 0 ? mb->getPSG1() : mb->getPSG2()).setChannelMute(channel, muted);
    }
  });
}

void MockingboardWindow::take() {
  std::optional<AY8910> chips[2];
  emulation_.poll(takePoll_, [&](host::MachineHost &host) {
    const MockingboardCard *mb = host.mockingboard();
    if (!mb) return;
    enabled_ = mb->isEnabled();
    for (int i = 0; i < 2; i++) {
      const AY8910 &chip = i == 0 ? mb->getPSG1() : mb->getPSG2();
      Psg &psg = psgs_[i];
      for (int r = 0; r < 16; r++) psg.registers[r] = chip.getRegister(r);
      psg.writes = chip.getWriteCount();
      psg.lastRegister = chip.getLastWriteReg();
      psg.lastValue = chip.getLastWriteVal();
      chips[i].emplace(chip);

      const VIA6522 &v = i == 0 ? mb->getVIA1() : mb->getVIA2();
      Via &via = vias_[i];
      via.ora = v.getORA();
      via.orb = v.getORB();
      via.ddra = v.getDDRA();
      via.ddrb = v.getDDRB();
      via.acr = v.getACR();
      via.ifr = v.getIFR();
      via.ier = v.getIER();
      via.t1Counter = v.getT1Counter();
      via.t1Latch = v.getT1Latch();
      via.t1Running = v.isT1Running();
      via.t1Fired = v.hasT1Fired();
      via.irq = v.isIRQActive();
    }
  });
  // The next few milliseconds of each channel, from a copy each: running a
  // copy forward plays nothing and leaves the card's own chip where it was.
  for (int i = 0; i < 2; i++) {
    if (!chips[i]) continue;
    for (int ch = 0; ch < 3; ch++) {
      AY8910 copy = *chips[i];
      copy.generateChannelSamples(psgs_[i].waveforms[ch].data(), WAVEFORM_SAMPLES, SAMPLE_RATE, ch);
    }
  }
}

void MockingboardWindow::drawChannel(int index, int channel, float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Psg &psg = psgs_[index];
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float mid = origin.y + ROW_HEIGHT * 0.5f;
  const bool muted = mutes & (1 << (index * 3 + channel));
  const float fade = muted ? 0.45f : 1.0f;
  float x = origin.x;

  // Mute, as a button with a speaker on it.
  ImGui::PushID(channel);
  ImGui::SetCursorScreenPos(ImVec2(x, mid - 12));
  if (ImGui::InvisibleButton("##mute", ImVec2(24, 24))) setMute(index, channel, !muted);
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip(muted ? "Unmute channel %c" : "Mute channel %c", 'A' + channel);
  }
  ImGui::PopID();
  draw->AddRectFilled(ImVec2(x, mid - 12), ImVec2(x + 24, mid + 12),
                      muted ? withAlpha(RED, 0.22f) : text(hovered ? 0.12f : 0.06f), 6.0f);
  speaker(draw, ImVec2(x + 11, mid), muted, muted ? RED : secondary());
  x += 34;

  // The channel's letter.
  draw->AddRectFilled(ImVec2(x, mid - 10), ImVec2(x + 20, mid + 10), withAlpha(BADGE[channel], fade), 5.0f);
  const char letter[2] = {static_cast<char>('A' + channel), 0};
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.85f);
  const ImVec2 ls = ImGui::CalcTextSize(letter);
  draw->AddText(ImVec2(x + 10 - ls.x * 0.5f, mid - ls.y * 0.5f), IM_COL32(255, 255, 255, static_cast<int>(255 * fade)), letter);
  ImGui::PopFont();
  x += 30;

  // The note: the tone counter toggles every TP ticks of the clock over 8,
  // so a whole cycle is the clock over 16 TP.
  const uint8_t mixer = psg.registers[7];
  const int period = psg.registers[channel * 2] | ((psg.registers[channel * 2 + 1] & 0x0F) << 8);
  char note[32] = "--";
  char hz[16] = "";
  if (period > 0) {
    const double freq = CLOCK / (16.0 * period);
    std::snprintf(hz, sizeof(hz), freq >= 1000 ? "%.0f Hz" : "%.1f Hz", freq);
    if (freq >= 20 && freq <= 20000) {
      const int number = static_cast<int>(std::lround(12 * std::log2(freq / 440.0) + 69));
      std::snprintf(note, sizeof(note), "%s%d", NOTE_NAMES[((number % 12) + 12) % 12], number / 12 - 1);
    } else {
      std::snprintf(note, sizeof(note), "--");
    }
  }
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.15f);
  const float noteHeight = ImGui::GetTextLineHeight();
  draw->AddText(ImVec2(x, mid - noteHeight * 0.5f - 6), text(fade), note);
  ImGui::PopFont();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.72f);
  draw->AddText(ImVec2(x, mid + 4), secondary(), hz);
  ImGui::PopFont();
  x += 78;

  // Tone and noise, which the mixer register enables with a zero.
  const float pillY = mid - 8;
  x += pill(draw, ImVec2(x, pillY), "T", !(mixer & (1 << channel)), GREEN) + 4;
  x += pill(draw, ImVec2(x, pillY), "N", !(mixer & (1 << (channel + 3))), ORANGE) + 12;

  // The level: fifteen steps, or the envelope's.
  const uint8_t amplitude = psg.registers[8 + channel];
  const bool envelope = amplitude & 0x10;
  const int level = amplitude & 0x0F;
  const float meterWidth = 90;
  const float segment = meterWidth / 15.0f;
  for (int i = 0; i < 15; i++) {
    const ImVec2 a(x + i * segment, mid - 5);
    const ImVec2 b(x + (i + 1) * segment - 1.5f, mid + 5);
    const ImU32 lit = envelope ? withAlpha(PURPLE, 0.35f + 0.65f * (i / 14.0f)) : withAlpha(TRACE[channel], fade);
    draw->AddRectFilled(a, b, (envelope || i < level) ? lit : text(0.08f), 1.5f);
  }
  char volume[8];
  std::snprintf(volume, sizeof(volume), envelope ? "ENV" : "%d", level);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.78f);
  const ImVec2 vs = ImGui::CalcTextSize(volume);
  draw->AddText(ImVec2(x + meterWidth + 8, mid - vs.y * 0.5f), envelope ? PURPLE : secondary(), volume);
  ImGui::PopFont();
  x += meterWidth + 40;

  // The waveform: what the channel sends to the mixer over the next 5ms.
  const ImVec2 wa(x, origin.y + 3);
  const ImVec2 wb(origin.x + width, origin.y + ROW_HEIGHT - 3);
  draw->AddRectFilled(wa, wb, well(), 6.0f);
  draw->AddLine(ImVec2(wa.x + 4, (wa.y + wb.y) * 0.5f), ImVec2(wb.x - 4, (wa.y + wb.y) * 0.5f), text(0.08f));
  const float plotWidth = wb.x - wa.x - 8;
  const float plotHeight = wb.y - wa.y - 8;
  ImVec2 points[WAVEFORM_SAMPLES];
  for (int i = 0; i < WAVEFORM_SAMPLES; i++) {
    const float sample = std::clamp(psg.waveforms[channel][i], 0.0f, 1.0f);
    points[i] = ImVec2(wa.x + 4 + plotWidth * i / (WAVEFORM_SAMPLES - 1), wb.y - 4 - sample * plotHeight);
  }
  draw->PushClipRect(wa, wb, true);
  draw->AddPolyline(points, WAVEFORM_SAMPLES, withAlpha(TRACE[channel], muted ? 0.35f : 1.0f), 0, 1.4f);
  draw->PopClipRect();

  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(width, ROW_HEIGHT));
}

void MockingboardWindow::drawVia(int index, float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Via &via = vias_[index];
  const Psg &psg = psgs_[index];
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetTextLineHeight() + 6;
  char value[32];
  auto hex2 = [&](uint8_t v) {
    std::snprintf(value, sizeof(value), "$%02X", v);
    return value;
  };

  // The bus to the chip, and what was last written through it.
  float x = origin.x;
  float y = origin.y;
  x += field(draw, ImVec2(x, y), "BUS", busFunction(via.orb, via.ddrb), accent()) + 18;
  std::snprintf(value, sizeof(value), "%u", psg.writes);
  x += field(draw, ImVec2(x, y), "WRITES", value, text()) + 18;
  std::snprintf(value, sizeof(value), "R%d=$%02X", psg.lastRegister, psg.lastValue);
  field(draw, ImVec2(x, y), "LAST", value, text());

  // The ports.
  y += line;
  x = origin.x;
  const struct {
    const char *label;
    uint8_t v;
  } ports[] = {{"ORA", via.ora}, {"ORB", via.orb}, {"DDRA", via.ddra}, {"DDRB", via.ddrb},
               {"ACR", via.acr}, {"IFR", via.ifr}, {"IER", via.ier}};
  for (const auto &port : ports) x += field(draw, ImVec2(x, y), port.label, hex2(port.v), GREEN) + 14;

  // Timer 1, which is what a player's interrupt runs off.
  y += line;
  x = origin.x;
  std::snprintf(value, sizeof(value), "$%04X", via.t1Counter);
  x += field(draw, ImVec2(x, y), "T1", value, PURPLE) + 14;
  std::snprintf(value, sizeof(value), "$%04X", via.t1Latch);
  x += field(draw, ImVec2(x, y), "LATCH", value, PURPLE) + 14;
  if (via.t1Latch > 0) {
    std::snprintf(value, sizeof(value), "%.1f Hz", CLOCK / (via.t1Latch + 2.0));
    x += field(draw, ImVec2(x, y), "RATE", value, text()) + 18;
  }
  const float pillY = y - 1;
  x += pill(draw, ImVec2(x, pillY), "RUN", via.t1Running, ORANGE) + 4;
  x += pill(draw, ImVec2(x, pillY), "FIRED", via.t1Fired, ORANGE) + 4;
  pill(draw, ImVec2(x, pillY), "T1 IRQ", (via.ier & 0x40) && (via.ifr & 0x40), RED);

  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(width, line * 3 - 6));
}

void MockingboardWindow::drawChip(int index) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Psg &psg = psgs_[index];
  const Via &via = vias_[index];
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float inner = WIDTH - PAD * 2;
  const float line = ImGui::GetTextLineHeight();
  const float height = PAD + line + 10 + ROW_HEIGHT * 3 + 6 * 2 + 14 + line + 4 + 14 + (line + 6) * 3 - 6 + PAD;
  card(draw, start, ImVec2(start.x + WIDTH, start.y + height));
  ImGui::PushID(index);

  // The heading: which chip, the VIA in front of it, and its interrupt.
  const float left = start.x + PAD;
  float y = start.y + PAD;
  char title[48];
  std::snprintf(title, sizeof(title), "PSG %d", index + 1);
  draw->AddText(ImVec2(left, y), text(), title);
  const float tw = ImGui::CalcTextSize(title).x;
  std::snprintf(title, sizeof(title), "AY-3-8910  ·  VIA %d at $C4%s", index + 1, index == 0 ? "00" : "80");
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
  draw->AddText(ImVec2(left + tw + 10, y + 2), secondary(), title);
  ImGui::PopFont();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.78f);
  const float irqWidth = ImGui::CalcTextSize("IRQ").x + 10;
  ImGui::PopFont();
  pill(draw, ImVec2(start.x + WIDTH - PAD - irqWidth, y), "IRQ", via.irq, RED);
  y += line + 10;

  for (int ch = 0; ch < 3; ch++) {
    ImGui::SetCursorScreenPos(ImVec2(left, y));
    drawChannel(index, ch, inner);
    y += ROW_HEIGHT + 6;
  }

  // The generators the channels share.
  y += 8;
  draw->AddLine(ImVec2(left, y - 6), ImVec2(left + inner, y - 6), ImGui::GetColorU32(ImGuiCol_Separator));
  float x = left;
  caption(draw, ImVec2(x, y + 2), "ENVELOPE", secondary());
  x += 64;
  const uint8_t shape = psg.registers[13] & 0x0F;
  envelopeGlyph(draw, ImVec2(x, y), ImVec2(x + 54, y + line), shape, PURPLE);
  x += 64;
  // The envelope steps once every 2 EP ticks of the clock over 8, sixteen
  // steps a ramp: the datasheet's 256 EP clocks.
  const int ep = psg.registers[11] | (psg.registers[12] << 8);
  char value[32];
  std::snprintf(value, sizeof(value), "$%X", shape);
  x += field(draw, ImVec2(x, y), "SHAPE", value, PURPLE) + 14;
  std::snprintf(value, sizeof(value), "%.1f ms", 256.0 * std::max(ep, 1) / CLOCK * 1000.0);
  x += field(draw, ImVec2(x, y), "RAMP", value, PURPLE) + 28;
  caption(draw, ImVec2(x, y + 2), "NOISE", secondary());
  x += 46;
  // The noise register shifts once every 2 NP ticks of the clock over 8.
  const int np = psg.registers[6] & 0x1F;
  std::snprintf(value, sizeof(value), "%.0f Hz", CLOCK / (16.0 * std::max(np, 1)));
  field(draw, ImVec2(x, y), "", value, ORANGE);
  y += line + 4 + 14;

  draw->AddLine(ImVec2(left, y - 8), ImVec2(left + inner, y - 8), ImGui::GetColorU32(ImGuiCol_Separator));
  ImGui::SetCursorScreenPos(ImVec2(left, y));
  drawVia(index, inner);

  ImGui::PopID();
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(WIDTH, height));
}

void MockingboardWindow::draw(bool *open) {
  if (!open || !*open || !fitted_) return;
  take();
  ui::BeforeWindow("Mockingboard");
  if (ui::BeginWindow("Mockingboard", open, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!enabled_) {
      ImGui::TextDisabled("The card is fitted but switched off.");
      ImGui::Spacing();
    }
    drawChip(0);
    ImGui::Dummy(ImVec2(0, 4));
    drawChip(1);
  }
  ImGui::End();
}

} // namespace a2e::native
