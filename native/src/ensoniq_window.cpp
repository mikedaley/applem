/*
 * ensoniq_window.cpp - The Ensoniq window: a IIgs's 5503 DOC and its sound RAM
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "ensoniq_window.hpp"

#include "emulation.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "iigs/iigs_machine.hpp"
#include "iigs/iigs_sound.hpp"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace a2e::native {

namespace {

using iigs::IIgsSound;

constexpr int DOC_OSCILLATORS = iigs::DOC_OSCILLATOR_COUNT;

constexpr float WIDTH = 1000;
constexpr float CARD_ROUNDING = 10;
constexpr float PAD = 14;
constexpr float ROW_HEIGHT = 42;
constexpr float RAM_HEIGHT = 46;

constexpr const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr const char *MODE_NAMES[4] = {"FREE", "ONE", "SYNC", "SWAP"};
constexpr const char *MODE_TIPS[4] = {
    "Free run: loops its table until stopped",
    "One shot: plays its table once and halts",
    ("Sync: an even oscillator restarts the odd one below it at its end; an odd one modulates the even one "
     "above it (AM)"),
    "Swap: halts at the end of its table and starts its partner",
};

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

void card(ImDrawList *draw, ImVec2 a, ImVec2 b) {
  draw->AddRectFilled(a, b, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

ImU32 well() { return ui::isDark() ? IM_COL32(0, 0, 0, 90) : IM_COL32(0, 0, 0, 18); }

void caption(ImDrawList *draw, ImVec2 at, const char *label) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
  draw->AddText(at, secondary(), label);
  ImGui::PopFont();
}

// A capsule with a word in it, lit or not, as wide as `widest` would make
// it so a capsule whose word changes keeps its size, and everything after
// it its place. Returns its width.
float pill(ImDrawList *draw, ImVec2 at, const char *label, bool lit, ImU32 colour, const char *widest = nullptr) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.74f);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const float inner = widest ? std::max(size.x, ImGui::CalcTextSize(widest).x) : size.x;
  const ImVec2 end(at.x + inner + 10, at.y + size.y + 4);
  draw->AddRectFilled(at, end, lit ? colour : text(0.07f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 5 + (inner - size.x) * 0.5f, at.y + 2), lit ? ui::textOn(colour) : secondary(), label);
  ImGui::PopFont();
  return end.x - at.x;
}

float pillHeight() {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.74f);
  const float h = ImGui::GetTextLineHeight() + 4;
  ImGui::PopFont();
  return h;
}

// A label and a value in the mono face, the value coloured. The width is the
// label and the widest value it can hold, whatever it holds now, so the
// fields after it never move. Returns that width.
float field(ImDrawList *draw, ImVec2 at, const char *label, const char *value, ImU32 colour, const char *widest) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.85f);
  draw->AddText(at, secondary(), label);
  const float lw = ImGui::CalcTextSize(label).x + 5;
  draw->AddText(ImVec2(at.x + lw, at.y), colour, value);
  const float width = lw + std::max(ImGui::CalcTextSize(widest).x, ImGui::CalcTextSize(value).x);
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

// An oscillator's colour: the logo's stripes in turn, a pair sharing one so
// the partners read as partners.
ImU32 hueFor(int index) {
  const ui::Palette &p = ui::palette();
  const ImU32 hues[6] = {p.blue, p.green, p.orange, p.purple, p.yellow, p.red};
  return hues[(index / 2) % 6];
}

const char *sizeName(uint32_t length) {
  switch (length) {
  case 256: return "256";
  case 512: return "512";
  case 1024: return "1K";
  case 2048: return "2K";
  case 4096: return "4K";
  case 8192: return "8K";
  case 16384: return "16K";
  case 32768: return "32K";
  }
  return "?";
}

} // namespace

EnsoniqWindow::EnsoniqWindow(Emulation &emulation) : emulation_(emulation) {}

void EnsoniqWindow::update() {
  emulation_.poll(updatePoll_, [&](host::MachineHost &host) {
    iigs::IIgsMachine *gs = host.iigs();
    present_ = gs != nullptr;
    if (!gs) {
      chip_ = nullptr;
      return;
    }
    IIgsSound &sound = gs->memory().sound();
    if (&sound == chip_) return;
    chip_ = &sound;
    for (int i = 0; i < DOC_OSCILLATORS; i++) sound.setOscillatorMuted(i, mutes & (1u << i));
  });
}

void EnsoniqWindow::setMute(int index, bool muted) {
  const uint32_t bit = 1u << index;
  mutes = muted ? (mutes | bit) : (mutes & ~bit);
  emulation_.post([index, muted](host::MachineHost &host) {
    if (iigs::IIgsMachine *gs = host.iigs()) gs->memory().sound().setOscillatorMuted(index, muted);
  });
}

void EnsoniqWindow::take() {
  emulation_.poll(takePoll_, [&](host::MachineHost &host) {
    iigs::IIgsMachine *gs = host.iigs();
    if (!gs) return;
    const IIgsSound &sound = gs->memory().sound();
    enabled_ = sound.activeOscillators();
    sampleRate_ = sound.sampleRate();
    irq_ = sound.interruptPending();
    interruptRegister_ = sound.docRegister(IIgsSound::DOC_INTERRUPT);
    control_ = sound.readControl();
    address_ = sound.address();
    for (int i = 0; i < DOC_OSCILLATORS; i++) {
      const IIgsSound::Oscillator o = sound.oscillator(i);
      Oscillator &osc = oscillators_[i];
      osc.frequency = o.frequency;
      osc.volume = o.volume;
      osc.control = o.control;
      osc.tableSize = o.tableSize;
      osc.data = o.data;
      osc.interruptPending = o.interruptPending;
      // The table: its size and resolution from $C0, and its start from the
      // pointer, the bank bit and as many low bits masked as the table is
      // long, as the chip finds it.
      const int sizeCode = (o.tableSize & IIgsSound::SIZE_TABLE_MASK) >> 3;
      osc.resolution = o.tableSize & IIgsSound::SIZE_RESOLUTION_MASK;
      osc.length = 256u << sizeCode;
      const uint32_t pointer = (static_cast<uint32_t>(o.waveTablePointer) << 8) |
                               ((o.tableSize & IIgsSound::SIZE_BANK) ? 0x10000u : 0u);
      osc.start = pointer & ~(osc.length - 1) & 0xFFFF;
      const int shift = 9 + osc.resolution - sizeCode;
      osc.position = (o.accumulator >> shift) & (osc.length - 1);
    }
    ram_.resize(iigs::SOUND_RAM_SIZE);
    for (size_t a = 0; a < ram_.size(); a++) ram_[a] = sound.soundRam(static_cast<uint16_t>(a));
  });
}

// The chip, and the window the 65816 reaches it through.
void EnsoniqWindow::drawChip(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetTextLineHeight();
  const float height = PAD + line + 10 + line + PAD;
  card(draw, start, ImVec2(start.x + width, start.y + height));

  const float left = start.x + PAD;
  float y = start.y + PAD;
  draw->AddText(ImVec2(left, y), text(), "Ensoniq 5503");
  const float tw = ImGui::CalcTextSize("Ensoniq 5503").x;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.8f);
  draw->AddText(ImVec2(left + tw + 10, y + 2), secondary(), "DOC  \xC2\xB7  64K sound RAM  \xC2\xB7  $C03C-$C03F");
  ImGui::PopFont();

  // On the right: whether every oscillator is shown or only those running.
  const float switchWidth = ui::SwitchWidth("Show all 32");
  ImGui::SetCursorScreenPos(ImVec2(start.x + width - PAD - switchWidth, y - 3));
  ui::Switch("Show all 32", &showAll_);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Every oscillator, not only the %d the chip is scanning", enabled_);
  }
  y += line + 10;

  char value[48];
  float x = left;
  std::snprintf(value, sizeof value, "%d", enabled_);
  x += field(draw, ImVec2(x, y), "OSCILLATORS", value, ui::accentText(), "32") + 18;
  std::snprintf(value, sizeof value, "%.0f Hz", sampleRate_);
  // Fastest with one oscillator: the clock over 8 over 3, six figures.
  x += field(draw, ImVec2(x, y), "RATE", value, text(), "000000 Hz") + 18;
  std::snprintf(value, sizeof value, "$%02X", interruptRegister_);
  x += field(draw, ImVec2(x, y), "$E0", value, p.purple, "$FF") + 10;
  const float py = y - 1;
  x += pill(draw, ImVec2(x, py), "IRQ", irq_, p.red) + 26;

  caption(draw, ImVec2(x, y + 2), "WINDOW");
  x += 54;
  const bool ram = control_ & IIgsSound::CONTROL_RAM;
  x += pill(draw, ImVec2(x, py), ram ? "RAM" : "REGS", true, ram ? p.blue : p.orange, "REGS") + 4;
  x += pill(draw, ImVec2(x, py), "AUTO+", control_ & IIgsSound::CONTROL_AUTO_INCREMENT, p.green) + 12;
  std::snprintf(value, sizeof value, "$%04X", address_);
  x += field(draw, ImVec2(x, y), "ADDR", value, p.blue, "$FFFF") + 18;
  std::snprintf(value, sizeof value, "%d/15", control_ & IIgsSound::CONTROL_VOLUME_MASK);
  field(draw, ImVec2(x, y), "VOL", value, text(), "15/15");

  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, height));
}

// The sound RAM, a column a page, as tall as the loudest byte in it, with
// the table of every running oscillator bracketed under it and its playhead.
void EnsoniqWindow::drawRam(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetTextLineHeight();
  const int shown = showAll_ ? DOC_OSCILLATORS : std::clamp(enabled_, 1, DOC_OSCILLATORS);
  const float lanes = 10;
  const float scale = ImGui::GetFontSize() * 0.72f + 4;
  const float height = PAD + line + 6 + RAM_HEIGHT + 6 + lanes + scale + PAD;
  card(draw, start, ImVec2(start.x + width, start.y + height));

  const float left = start.x + PAD;
  float y = start.y + PAD;
  caption(draw, ImVec2(left, y + 2), "SOUND RAM");
  y += line + 6;

  const float inner = width - PAD * 2;
  const ImVec2 wa(left, y), wb(left + inner, y + RAM_HEIGHT);
  draw->AddRectFilled(wa, wb, well(), 6.0f);
  if (ram_.size() == iigs::SOUND_RAM_SIZE) {
    const float column = inner / 256.0f;
    for (int page = 0; page < 256; page++) {
      int loudest = 0;
      for (int i = 0; i < 256; i += 2) {
        const int b = ram_[page * 256 + i];
        if (b) loudest = std::max(loudest, std::abs(b - 0x80));
      }
      if (!loudest) continue;
      const float h = (RAM_HEIGHT - 6) * (loudest / 128.0f);
      const float mid = (wa.y + wb.y) * 0.5f;
      draw->AddRectFilled(ImVec2(wa.x + page * column, mid - h * 0.5f),
                          ImVec2(wa.x + (page + 1) * column - 0.5f, mid + h * 0.5f), text(0.32f));
    }
  }

  // The tables in use, a lane under the RAM, and where each one is playing.
  const float laneY = wb.y + 4;
  for (int i = 0; i < shown; i++) {
    const Oscillator &o = oscillators_[i];
    if (o.control & IIgsSound::OSC_HALT) continue;
    const float x0 = wa.x + inner * (o.start / 65536.0f);
    const float x1 = wa.x + inner * (std::min<uint32_t>(o.start + o.length, 65536) / 65536.0f);
    const ImU32 colour = hueFor(i);
    draw->AddRectFilled(ImVec2(x0, laneY), ImVec2(std::max(x1, x0 + 2), laneY + 3), colour, 1.5f);
    draw->AddRectFilled(ImVec2(x0, wa.y), ImVec2(std::max(x1, x0 + 1), wb.y), withAlpha(colour, 0.10f));
    const float px = wa.x + inner * ((o.start + o.position) / 65536.0f);
    draw->AddLine(ImVec2(px, wa.y + 2), ImVec2(px, wb.y - 2), colour, 1.5f);
  }

  // The addresses, under the lanes, every 8K.
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.72f);
  for (int k = 0; k < 8; k++) {
    char label[8];
    std::snprintf(label, sizeof label, "$%X000", k * 2);
    const float lx = wa.x + inner * (k / 8.0f);
    draw->AddLine(ImVec2(lx, laneY + lanes - 4), ImVec2(lx, laneY + lanes), text(0.2f));
    draw->AddText(ImVec2(lx + 3, laneY + lanes - 3), secondary(), label);
  }
  ImGui::PopFont();

  // What a page holds, under the pointer.
  ImGui::SetCursorScreenPos(wa);
  ImGui::InvisibleButton("##ram", ImVec2(inner, RAM_HEIGHT + lanes));
  if (ImGui::IsItemHovered()) {
    const int page = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - wa.x) / inner * 256), 0, 255);
    std::string users;
    for (int i = 0; i < shown; i++) {
      const Oscillator &o = oscillators_[i];
      const uint32_t at = static_cast<uint32_t>(page) * 256;
      if (!(o.control & IIgsSound::OSC_HALT) && at >= o.start && at < o.start + o.length) {
        char name[8];
        std::snprintf(name, sizeof name, "%s%d", users.empty() ? "" : ", ", i);
        users += name;
      }
    }
    ImGui::SetTooltip("$%02X00-$%02XFF%s%s", page, page, users.empty() ? "" : "\nPlaying from it: ", users.c_str());
  }

  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, height));
}

void EnsoniqWindow::drawOscillator(int index, float width, float height) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const Oscillator &o = oscillators_[index];
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float mid = origin.y + height * 0.5f;
  const bool scanned = index < enabled_;
  const bool running = scanned && !(o.control & IIgsSound::OSC_HALT);
  const bool muted = mutes & (1u << index);
  const float fade = !scanned ? 0.35f : muted ? 0.5f : 1.0f;
  const ImU32 hue = hueFor(index);
  float x = origin.x;

  // Mute, as a button with a speaker on it.
  ImGui::PushID(index);
  ImGui::SetCursorScreenPos(ImVec2(x, mid - 11));
  if (ImGui::InvisibleButton("##mute", ImVec2(22, 22))) setMute(index, !muted);
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip(muted ? "Hear oscillator %d" : "Leave oscillator %d out of the mix", index);
  }
  ImGui::PopID();
  draw->AddRectFilled(ImVec2(x, mid - 11), ImVec2(x + 22, mid + 11),
                      muted ? withAlpha(p.red, 0.22f) : text(hovered ? 0.12f : 0.06f), 6.0f);
  speaker(draw, ImVec2(x + 10, mid), muted, muted ? p.red : secondary());
  x += 30;

  // Its number, filled while it runs.
  char number[4];
  std::snprintf(number, sizeof number, "%02d", index);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.85f);
  const ImVec2 ns = ImGui::CalcTextSize(number);
  const ImVec2 ba(x, mid - 10), bb(x + ns.x + 10, mid + 10);
  if (running) draw->AddRectFilled(ba, bb, withAlpha(hue, fade), 5.0f);
  else draw->AddRect(ba, bb, withAlpha(hue, 0.6f * fade), 5.0f, 0, 1.2f);
  draw->AddText(ImVec2(ba.x + 5, mid - ns.y * 0.5f), running ? ui::textOn(hue) : withAlpha(secondary(), fade), number);
  ImGui::PopFont();
  x = bb.x + 8;

  // Mode and interrupt.
  const int mode = (o.control & IIgsSound::OSC_MODE_MASK) >> 1;
  const float py = mid - pillHeight() * 0.5f;
  const float modeX = x;
  x += pill(draw, ImVec2(x, py), MODE_NAMES[mode], running, withAlpha(hue, 0.85f), "SWAP") + 4;
  const bool modeHovered = ui::IsHoveringRect(ImVec2(modeX, py), ImVec2(x, py + pillHeight()));
  const float irqX = x;
  const bool irqEnabled = o.control & IIgsSound::OSC_INTERRUPT_ENABLE;
  x += pill(draw, ImVec2(x, py), "I", irqEnabled, o.interruptPending ? p.red : withAlpha(p.purple, 0.85f)) + 10;
  const bool irqHovered = ui::IsHoveringRect(ImVec2(irqX, py), ImVec2(x, py + pillHeight()));
  if (modeHovered) ImGui::SetTooltip("%s", MODE_TIPS[mode]);
  if (irqHovered) {
    ImGui::SetTooltip("Interrupts when it halts: %s%s", irqEnabled ? "yes" : "no",
                      o.interruptPending ? "\nAn interrupt is waiting to be read from $E0" : "");
  }

  // The pitch the table plays at, taking it as one cycle: a step of the
  // frequency register per scan, and 2^(17 + resolution) of them a table,
  // whatever its size.
  const double hz = sampleRate_ * o.frequency / std::ldexp(1.0, 17 + o.resolution);
  char note[16] = "--";
  char freq[24];
  std::snprintf(freq, sizeof freq, "$%04X", o.frequency);
  if (o.frequency) {
    if (hz >= 16 && hz <= 20000) {
      const int n = static_cast<int>(std::lround(12 * std::log2(hz / 440.0) + 69));
      std::snprintf(note, sizeof note, "%s%d", NOTE_NAMES[((n % 12) + 12) % 12], n / 12 - 1);
    }
    std::snprintf(freq, sizeof freq, hz >= 1000 ? "%.0f Hz" : hz >= 10 ? "%.1f Hz" : "%.2f Hz", hz);
  }
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.05f);
  const float noteHeight = ImGui::GetTextLineHeight();
  draw->AddText(ImVec2(x, mid - noteHeight * 0.5f - 6), text(fade), note);
  ImGui::PopFont();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.7f);
  draw->AddText(ImVec2(x, mid + 4), withAlpha(secondary(), fade), freq);
  ImGui::PopFont();
  if (ui::IsHoveringRect(ImVec2(x, mid - 14), ImVec2(x + 62, mid + 14))) {
    ImGui::SetTooltip("Frequency $%04X, resolution %d: the table plays %.2f times a second\n"
                      "(its pitch, if it holds one cycle of a wave)",
                      o.frequency, o.resolution, hz);
  }
  x += 66;

  // Volume, 0 to 255.
  const float meter = 46;
  draw->AddRectFilled(ImVec2(x, mid - 3), ImVec2(x + meter, mid + 3), text(0.08f), 3.0f);
  draw->AddRectFilled(ImVec2(x, mid - 3), ImVec2(x + meter * (o.volume / 255.0f), mid + 3), withAlpha(hue, fade), 3.0f);
  char volume[8];
  std::snprintf(volume, sizeof volume, "%d", o.volume);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.7f);
  draw->AddText(ImVec2(x, mid - 16), withAlpha(secondary(), fade), "VOL");
  draw->AddText(ImVec2(x, mid + 5), withAlpha(text(), fade), volume);
  ImGui::PopFont();
  x += meter + 10;

  // The table itself, from the sound RAM, with the playhead on it.
  const ImVec2 wa(x, origin.y + 2), wb(origin.x + width, origin.y + height - 2);
  draw->AddRectFilled(wa, wb, well(), 6.0f);
  const float w = wb.x - wa.x - 8, h = wb.y - wa.y - 6;
  const float centre = (wa.y + wb.y) * 0.5f;
  draw->AddLine(ImVec2(wa.x + 4, centre), ImVec2(wb.x - 4, centre), text(0.07f));
  if (ram_.size() == iigs::SOUND_RAM_SIZE && o.volume + o.frequency > 0) {
    const int columns = std::max(8, static_cast<int>(w / 2));
    const uint32_t per = std::max<uint32_t>(1, o.length / static_cast<uint32_t>(columns));
    const uint32_t stride = std::max<uint32_t>(1, per / 8);
    const ImU32 colour = withAlpha(hue, running && !muted ? 1.0f : 0.4f);
    for (int c = 0; c < columns; c++) {
      const uint32_t from = static_cast<uint32_t>(c) * o.length / static_cast<uint32_t>(columns);
      int low = 0x80, high = 0x80;
      bool end = false;
      for (uint32_t i = from; i < from + per && i < o.length; i += stride) {
        const int b = ram_[(o.start + i) & 0xFFFF];
        if (!b) {
          end = true;
          continue;
        }
        low = std::min(low, b);
        high = std::max(high, b);
      }
      const float cx = wa.x + 4 + w * (c + 0.5f) / columns;
      if (end) {
        // A zero byte: where the chip stops.
        draw->AddLine(ImVec2(cx, wa.y + 3), ImVec2(cx, wb.y - 3), withAlpha(p.red, 0.5f), 1.0f);
        continue;
      }
      const float yHigh = centre - (high - 0x80) / 128.0f * h * 0.5f;
      const float yLow = centre - (low - 0x80) / 128.0f * h * 0.5f;
      draw->AddLine(ImVec2(cx, yHigh), ImVec2(cx, std::max(yLow, yHigh + 1)), colour, 1.2f);
    }
    if (running) {
      const float px = wa.x + 4 + w * (o.position / static_cast<float>(o.length));
      draw->AddLine(ImVec2(px, wa.y + 2), ImVec2(px, wb.y - 2), text(0.85f), 1.5f);
    }
  }
  char table[32];
  std::snprintf(table, sizeof table, "$%04X  %s", o.start, sizeName(o.length));
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.66f);
  draw->AddText(ImVec2(wa.x + 6, wa.y + 2), withAlpha(secondary(), fade), table);
  ImGui::PopFont();
  if (ui::IsHoveringRect(wa, wb)) {
    ImGui::SetTooltip("Table $%04X-$%04X, %u bytes, resolution %d\nPlaying byte %u of it; it last read $%02X\n"
                      "Channel %d (only a stereo card would separate them)",
                      o.start, (o.start + o.length - 1) & 0xFFFF, o.length, o.resolution, o.position, o.data,
                      (o.control & IIgsSound::OSC_CHANNEL_MASK) >> 4);
  }

  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(width, height));
}

// The oscillators in their pairs, even beside odd: swap and sync both act
// across a pair, so a pair is a row.
void EnsoniqWindow::drawOscillators(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetTextLineHeight();
  const int shown = showAll_ ? DOC_OSCILLATORS : std::clamp(enabled_ + (enabled_ & 1), 2, DOC_OSCILLATORS);
  const int rows = shown / 2;
  const float height = PAD + line + 6 + rows * (ROW_HEIGHT + 6) - 6 + PAD;
  card(draw, start, ImVec2(start.x + width, start.y + height));

  const float left = start.x + PAD;
  float y = start.y + PAD;
  caption(draw, ImVec2(left, y + 2), "OSCILLATORS");
  y += line + 6;
  const float inner = width - PAD * 2;
  const float gap = 18;
  const float column = (inner - gap) * 0.5f;
  for (int row = 0; row < rows; row++) {
    for (int side = 0; side < 2; side++) {
      ImGui::SetCursorScreenPos(ImVec2(left + side * (column + gap), y));
      drawOscillator(row * 2 + side, column, ROW_HEIGHT);
    }
    // The pair's link.
    const float lx = left + column + gap * 0.5f;
    draw->AddLine(ImVec2(lx - 5, y + ROW_HEIGHT * 0.5f), ImVec2(lx + 5, y + ROW_HEIGHT * 0.5f),
                  withAlpha(hueFor(row * 2), 0.5f), 2.0f);
    y += ROW_HEIGHT + 6;
  }

  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, height));
}

void EnsoniqWindow::draw(bool *open) {
  if (!open || !*open || !present_) return;
  take();
  ui::BeforeWindow("Ensoniq");
  // The cards are drawn to a fixed width, so only the height is the user's:
  // shorter than its contents, the window scrolls. The width leaves room for
  // the scrollbar, so the cards never sit under it.
  const ImGuiStyle &style = ImGui::GetStyle();
  const float width = WIDTH + style.WindowPadding.x * 2 + style.ScrollbarSize;
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 240), ImVec2(width, FLT_MAX));
  ImGui::SetNextWindowSize(ImVec2(width, 720), ImGuiCond_FirstUseEver);
  if (ui::BeginWindow("Ensoniq", open)) {
    drawChip(WIDTH);
    ImGui::Dummy(ImVec2(0, 2));
    drawRam(WIDTH);
    ImGui::Dummy(ImVec2(0, 2));
    drawOscillators(WIDTH);
  }
  ImGui::End();
}

} // namespace a2e::native
