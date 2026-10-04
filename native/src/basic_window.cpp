/*
 * basic_window.cpp - The Applesoft BASIC window: an editor that runs and debugs
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "basic_window.hpp"

#include "emulation.hpp"
#include "media_store.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "basic/basic_detokenizer.hpp"
#include "basic/basic_tokenizer.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace a2e::native {

namespace {

using ui::Palette;
using ui::palette;

constexpr float CARD_ROUNDING = 10.0f;
constexpr float PAD = 12.0f;
constexpr float GUTTER = 46.0f;
constexpr float SIDEBAR_MIN = 220.0f;
constexpr float EDITOR_MIN = 380.0f;
constexpr double FLASH = 0.6;   // how long a changed value stays lit
constexpr double PULSE = 0.8;   // a hit breakpoint's flash, repeated

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 well() { return ui::isDark() ? IM_COL32(0, 0, 0, 80) : IM_COL32(255, 255, 255, 200); }

void card(ImDrawList *draw, ImVec2 a, ImVec2 b) {
  draw->AddRectFilled(a, b, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 7), CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

void caption(ImDrawList *draw, ImVec2 at, const char *label) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(at, secondary(), label);
  ImGui::PopFont();
}

// A capsule with a word in it, lit or not. Returns its width.
float pill(ImDrawList *draw, ImVec2 at, const char *label, bool lit, ImU32 colour, float scale = ui::SMALL_TEXT) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * scale);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const ImVec2 end(at.x + size.x + 12, at.y + size.y + 4);
  draw->AddRectFilled(at, end, lit ? colour : text(0.07f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 6, at.y + 2), lit ? ui::textOn(colour) : secondary(), label);
  ImGui::PopFont();
  return end.x - at.x;
}

// The language's colours, from the logo's stripes as the theme makes them
// readable: the browser's categories, one colour each.
ImU32 colourFor(basic::Kind kind) {
  const Palette &p = palette();
  using basic::Kind;
  switch (kind) {
  case Kind::LineNumber: return secondary();
  case Kind::Flow: return p.red;
  case Kind::Loop: return p.purple;
  case Kind::InputOutput: return p.blue;
  case Kind::Graphics: return p.green;
  case Kind::Memory: return p.orange;
  case Kind::Function: return p.blue;
  case Kind::Declaration: return p.green;
  case Kind::Misc: return secondary();
  case Kind::Keyword: return p.blue;
  case Kind::String: return p.green;
  case Kind::Number: return p.orange;
  case Kind::Variable: return text();
  case Kind::Operator: return p.purple;
  case Kind::Punctuation: return secondary();
  case Kind::Comment: return ui::faintText();
  case Kind::Plain: return text();
  }
  return text();
}

// Heat from cool to hot: the browser's ramp, teal through yellow to red.
ImU32 heatColour(float h, float alpha) {
  float r, g, b;
  if (h < 0.25f) {
    const float t = h / 0.25f;
    r = 0, g = 100 + 80 * t, b = 200 - 20 * t;
  } else if (h < 0.5f) {
    const float t = (h - 0.25f) / 0.25f;
    r = 0, g = 180, b = 180 - 100 * t;
  } else if (h < 0.75f) {
    const float t = (h - 0.5f) / 0.25f;
    r = 200 * t, g = 180 + 20 * t, b = 80 - 80 * t;
  } else {
    const float t = (h - 0.75f) / 0.25f;
    r = 200 + 20 * t, g = 200 - 140 * t, b = 40 * t;
  }
  return IM_COL32(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b), static_cast<int>(255 * alpha));
}

// The toolbar's icons, drawn so they are the same at any size.
enum class Icon { Run, Pause, Stop, Step, StepLine };

void drawIcon(ImDrawList *draw, Icon icon, ImVec2 c, float s, ImU32 colour) {
  const float t = std::max(1.5f, s * 0.13f);
  switch (icon) {
  case Icon::Run:
    draw->AddTriangleFilled(ImVec2(c.x - s * 0.32f, c.y - s * 0.42f), ImVec2(c.x - s * 0.32f, c.y + s * 0.42f),
                            ImVec2(c.x + s * 0.42f, c.y), colour);
    break;
  case Icon::Pause:
    draw->AddRectFilled(ImVec2(c.x - s * 0.36f, c.y - s * 0.4f), ImVec2(c.x - s * 0.1f, c.y + s * 0.4f), colour, 1.5f);
    draw->AddRectFilled(ImVec2(c.x + s * 0.1f, c.y - s * 0.4f), ImVec2(c.x + s * 0.36f, c.y + s * 0.4f), colour, 1.5f);
    break;
  case Icon::Stop:
    draw->AddRectFilled(ImVec2(c.x - s * 0.36f, c.y - s * 0.36f), ImVec2(c.x + s * 0.36f, c.y + s * 0.36f), colour, 2.5f);
    break;
  case Icon::Step:
    draw->AddLine(ImVec2(c.x, c.y - s * 0.45f), ImVec2(c.x, c.y + s * 0.15f), colour, t);
    draw->AddTriangleFilled(ImVec2(c.x - s * 0.28f, c.y + s * 0.02f), ImVec2(c.x + s * 0.28f, c.y + s * 0.02f),
                            ImVec2(c.x, c.y + s * 0.32f), colour);
    draw->AddCircleFilled(ImVec2(c.x, c.y + s * 0.45f), s * 0.09f, colour);
    break;
  case Icon::StepLine:
    draw->AddLine(ImVec2(c.x - s * 0.1f, c.y - s * 0.45f), ImVec2(c.x - s * 0.1f, c.y + s * 0.2f), colour, t);
    draw->AddTriangleFilled(ImVec2(c.x - s * 0.36f, c.y + s * 0.06f), ImVec2(c.x + s * 0.16f, c.y + s * 0.06f),
                            ImVec2(c.x - s * 0.1f, c.y + s * 0.36f), colour);
    draw->AddLine(ImVec2(c.x + s * 0.2f, c.y + s * 0.44f), ImVec2(c.x + s * 0.46f, c.y + s * 0.44f), colour, t);
    draw->AddLine(ImVec2(c.x - s * 0.46f, c.y + s * 0.44f), ImVec2(c.x - s * 0.24f, c.y + s * 0.44f), colour, t);
    break;
  }
}

bool toolButton(const char *id, Icon icon, const char *label, const char *tip, bool enabled, ImU32 tint = 0,
                bool primary = false) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float height = ImGui::GetFrameHeight() + 6;
  const float width = height + ImGui::CalcTextSize(label).x + 14;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!enabled);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  const bool held = ImGui::IsItemActive();
  ImGui::EndDisabled();
  const ImVec2 end(at.x + width, at.y + height);
  if (primary && enabled) {
    draw->AddRectFilled(at, end, withAlpha(tint ? tint : accent(), held ? 0.75f : hovered ? 0.9f : 1.0f), 7.0f);
  } else if (enabled && (hovered || held)) {
    draw->AddRectFilled(at, end, text(held ? 0.14f : 0.08f), 7.0f);
  }
  const ImU32 base = tint ? tint : text(0.85f);
  const ImU32 colour = !enabled ? text(0.25f) : primary ? ui::textOn(tint ? tint : accent()) : base;
  drawIcon(draw, icon, ImVec2(at.x + height * 0.5f + 2, at.y + height * 0.5f), height * 0.42f, colour);
  draw->AddText(ImVec2(at.x + height + 2, at.y + (height - ImGui::GetTextLineHeight()) * 0.5f),
                !enabled ? text(0.25f) : primary ? ui::textOn(tint ? tint : accent()) : text(0.85f), label);
  if (hovered && tip) ImGui::SetTooltip("%s", tip);
  return pressed && enabled;
}

bool removeButton(const char *id) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float size = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
  if (hovered) draw->AddCircleFilled(c, size * 0.5f, withAlpha(palette().red, 0.2f));
  const float r = size * 0.2f;
  const ImU32 colour = hovered ? palette().red : secondary();
  draw->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), colour, 1.5f);
  draw->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), colour, 1.5f);
  return pressed;
}

// The program in memory, as a hash of its tokens: what changes when the
// program does, and nothing else.
uint32_t programHash(Emulator &e) {
  MMU &mmu = e.getMMU();
  const uint16_t txttab = static_cast<uint16_t>(mmu.readRAM(0x67, false) | (mmu.readRAM(0x68, false) << 8));
  const uint16_t vartab = static_cast<uint16_t>(mmu.readRAM(0x69, false) | (mmu.readRAM(0x6A, false) << 8));
  uint32_t h = 2166136261u;
  if (txttab >= vartab || vartab >= 0xC000) return h;
  for (uint16_t a = txttab; a < vartab; a++) h = (h ^ mmu.readRAM(a, false)) * 16777619u;
  return h;
}

std::string trimLeft(const std::string &s) {
  const size_t i = s.find_first_not_of(' ');
  return i == std::string::npos ? std::string() : s.substr(i);
}

int countLines(const std::vector<std::string> &lines) {
  int n = 0;
  for (const std::string &l : lines) {
    if (l.find_first_not_of(' ') != std::string::npos) n++;
  }
  return n;
}

// The text's length as the browser counts it, line breaks included.
size_t countChars(const std::vector<std::string> &lines) {
  size_t n = lines.empty() ? 0 : lines.size() - 1;
  for (const std::string &l : lines) n += l.size();
  return n;
}

} // namespace

BasicWindow::BasicWindow(Emulation &emulation, Platform &platform) : emulation_(emulation), platform_(platform) {}

// ---------------------------------------------------------------------------
// Reading the machine
// ---------------------------------------------------------------------------

void BasicWindow::update(bool open) {
  open_ = open;
  if (!haveText_) {
    std::string joined;
    for (size_t i = 0; i < pendingText_.size(); i++) joined += (i ? "\n" : "") + pendingText_[i];
    editor_.setText(joined);
    formattedRevision_ = editor_.revision();
    pendingText_.clear();
    haveText_ = true;
  }
  take();
}

void BasicWindow::take() {
  // Closed, the window's one job is its breakpoints: giving them to a new
  // machine and checking a condition when one stops it. With none set there
  // is nothing to do, and nothing is read; what was last seen is forgotten,
  // so opening it again starts from what the machine is doing then rather
  // than mistaking the change for a program that just finished.
  if (!open_ && breakpoints_.empty()) {
    lastRunning_ = false;
    lastHit_ = false;
    return;
  }
  State s;
  const double now = ImGui::GetTime();
  bool resumed = false;
  emulation_.poll(poll_, [&](host::MachineHost &host) {
    Emulator *e = host.emulator();
    available_ = e != nullptr;
    if (!e) {
      machine_ = nullptr;
      return;
    }
    // A machine built since the breakpoints were given to one starts with
    // none.
    if (e != machine_) {
      machine_ = e;
      e->clearBasicBreakpoints();
      for (const Breakpoint &b : breakpoints_) {
        if (b.enabled && !b.isRule()) e->addBasicBreakpoint(static_cast<uint16_t>(b.line), b.statement);
      }
      e->clearBasicConditionRules();
      for (const Breakpoint &b : breakpoints_) {
        if (b.enabled && b.isRule() && !b.condition.empty()) e->addBasicConditionRule(b.statement, b.condition.c_str());
      }
      heatApplied_ = false;
    }
    if (heat_ != heatApplied_) {
      e->setBasicHeatMapEnabled(heat_);
      if (!heat_) e->clearBasicHeatMap();
      heatApplied_ = heat_;
    }

    MMU &mmu = e->getMMU();
    auto word = [&](uint16_t a) { return static_cast<uint16_t>(mmu.readRAM(a, false) | (mmu.readRAM(a + 1, false) << 8)); };
    s.valid = true;
    s.powered = emulation_.powered();
    // Waiting for a line at ] (the prompt character alone stays ] while a
    // program, or machine code started from the prompt, runs).
    s.atPrompt = e->isAtBasicPrompt();
    s.running = e->isBasicProgramRunning();
    s.paused = host.isPaused();
    s.hit = e->isBasicBreakpointHit();
    s.breakLine = e->getBasicBreakLine();
    s.ruleHit = e->getBasicConditionRuleHitId();
    s.curlin = word(0x75);
    s.txtptr = e->getBasicTxtptr();
    // Which statement means walking the program's lines, so it is asked for
    // only to be shown, or to match a stop against a statement breakpoint.
    const bool newStop = s.paused && s.hit && !lastHit_;
    if ((s.curlin >> 8) != 0xFF && (open_ || newStop)) {
      s.statement = e->getBasicStatementIndexForLine(s.curlin, s.txtptr);
      s.statements = e->getBasicStatementCountForLine(s.curlin);
    }

    // A new stop: a step finishing, or a breakpoint, whose condition is
    // checked here, as the browser checks it. A step is never sent back:
    // where it lands is where the user asked to look, condition or none.
    if (newStop) {
      const bool wasStep = stepRequested_;
      stepRequested_ = false;
      int entry = -1;
      if (s.ruleHit >= 0) {
        for (size_t i = 0; i < breakpoints_.size(); i++) {
          if (breakpoints_[i].isRule() && breakpoints_[i].statement == s.ruleHit) entry = static_cast<int>(i);
        }
      } else {
        for (int stmt : {s.statement, -1}) {
          for (size_t i = 0; i < breakpoints_.size() && entry < 0; i++) {
            const Breakpoint &b = breakpoints_[i];
            if (!b.isRule() && b.line == s.breakLine && b.statement == stmt) entry = static_cast<int>(i);
          }
        }
      }
      Breakpoint *b = entry >= 0 ? &breakpoints_[static_cast<size_t>(entry)] : nullptr;
      if (!wasStep && b && !b->isRule() && !b->condition.empty() && !host.evaluateCondition(b->condition)) {
        host.setPaused(false);
        e->clearBasicBreakpointHit();
        resumed = true;
        return;
      }
      if (b && !wasStep) {
        b->hits++;
        pulse_ = entry;
        pulseAt_ = now;
      }
      stopReason_ = wasStep ? (stopReason_ == "Pausing" ? "Paused" : "Step") : b && b->isRule() ? "Rule" : "Breakpoint";
      stoppedAtBreak_ = !wasStep;
      currentLine_ = s.breakLine;
      currentStatement_ = s.statement;
      if (const int l = textLineFor(s.breakLine); l >= 0) editor_.scrollToLine(l);
    }

    // The program has finished: the error it ended on, if any, and the
    // machine let go of anything it was holding for the window.
    if (lastRunning_ && !s.running) {
      if (e->isBasicErrorHit()) {
        Error err;
        err.line = e->getBasicErrorLine();
        err.statement = e->getBasicStatementIndexForLine(static_cast<uint16_t>(err.line), e->getBasicErrorTxtptr());
        err.message = basic::errorMessage(e->getBasicErrorCode());
        const int l = textLineFor(err.line);
        if (l >= 0) {
          err.code = editor_.lines()[static_cast<size_t>(l)];
          editor_.scrollToLine(l);
        }
        error_ = err;
        e->clearBasicError();
      }
      host.setPaused(false);
      e->clearBasicBreakpointHit();
      stepRequested_ = false;
      s.paused = false;
      s.hit = false;
    }

    // The program's hash, now and then, to see it change under the editor.
    if (open_ && now - hashedAt_ > 0.5 && !s.running) {
      s.programHash = programHash(*e);
      hashedAt_ = now;
    } else {
      s.programHash = state_.programHash;
    }

    // The heat, while it runs.
    if (open_ && heat_ && s.running && !s.paused && now - heatAt_ > 0.1) {
      const auto &counts = e->getBasicHeatMap();
      uint32_t most = 1;
      for (const auto &[line, n] : counts) most = std::max(most, n);
      for (auto &[line, level] : heatLevels_) level *= 0.92f;
      for (const auto &[line, n] : counts) {
        const float h = std::log1p(static_cast<float>(n)) / std::log1p(static_cast<float>(most));
        float &level = heatLevels_[line];
        level = std::max(level, h);
      }
      for (auto it = heatLevels_.begin(); it != heatLevels_.end();) {
        it = it->second < 0.005f ? heatLevels_.erase(it) : std::next(it);
      }
      heatAt_ = now;
    }

    // The variables: while it runs, a few times a second, and once more as
    // it stops.
    const bool settled = !s.running || s.paused;
    if (open_ && s.powered && (now - variablesAt_ > (settled ? 0.5 : 0.12))) {
      const VarMemReadFn reader = [e](uint16_t a) { return e->peekMemory(a); };
      variables_ = ApplesoftVarReader::readVariables(reader);
      arrays_ = ApplesoftVarReader::readArrays(reader);
      variablesAt_ = now;
      for (const BasicVariableInfo &v : variables_) {
        std::string value = v.type == BasicVarType::String ? v.stringValue
                            : v.type == BasicVarType::Integer ? std::to_string(v.intValue)
                                                              : basic::formatReal(v.realValue);
        auto it = lastValues_.find(v.name);
        if (it != lastValues_.end() && it->second != value) changedAt_[v.name] = now;
        lastValues_[v.name] = value;
      }
      // An open array's cells flash as a variable's value does.
      for (const BasicArrayInfo &a : arrays_) {
        if (!expandedArrays_.count(a.name)) continue;
        for (uint32_t i = 0; i < a.elementCount; i++) {
          std::string value = a.type == BasicVarType::String  ? (i < a.stringValues.size() ? a.stringValues[i] : "")
                              : a.type == BasicVarType::Integer ? (i < a.intValues.size() ? std::to_string(a.intValues[i]) : "")
                                                                : (i < a.realValues.size() ? basic::formatReal(a.realValues[i]) : "");
          const std::string key = a.name + "#" + std::to_string(i);
          auto it = lastValues_.find(key);
          if (it != lastValues_.end() && it->second != value) changedAt_[key] = now;
          lastValues_[key] = std::move(value);
        }
      }
    }
  });
  if (resumed || !s.valid) {
    if (resumed) lastHit_ = false;
    return;
  }
  lastHit_ = s.paused && s.hit;
  lastRunning_ = s.running;
  state_ = s;

  // Where it is: the line stopped on, or the line running when Trace is on.
  if (s.running && s.paused && (s.curlin >> 8) != 0xFF) {
    currentLine_ = s.curlin;
    currentStatement_ = s.statements > 1 ? s.statement : -1;
  } else if (s.running && !s.paused) {
    stoppedAtBreak_ = false;
    currentStatement_ = -1;
    if (trace_ && (s.curlin >> 8) != 0xFF) {
      if (!currentLine_ || *currentLine_ != s.curlin) {
        currentLine_ = s.curlin;
        if (const int l = textLineFor(s.curlin); l >= 0) editor_.scrollToLine(l);
      }
    } else {
      currentLine_.reset();
    }
  } else if (!s.running) {
    currentLine_.reset();
    currentStatement_ = -1;
    stoppedAtBreak_ = false;
  }
}

// The core holds exactly the enabled breakpoints and rules.
void BasicWindow::applyBreakpoints() {
  emulation_.withMachine([&](host::MachineHost &host) {
    Emulator *e = host.emulator();
    if (!e) return;
    e->clearBasicBreakpoints();
    for (const Breakpoint &b : breakpoints_) {
      if (b.enabled && !b.isRule()) e->addBasicBreakpoint(static_cast<uint16_t>(b.line), b.statement);
    }
    e->clearBasicConditionRules();
    for (const Breakpoint &b : breakpoints_) {
      if (b.enabled && b.isRule() && !b.condition.empty()) e->addBasicConditionRule(b.statement, b.condition.c_str());
    }
  });
  ImGui::MarkIniSettingsDirty();
}

void BasicWindow::message(const std::string &text, bool problem) {
  message_ = text;
  messageProblem_ = problem;
  messageAt_ = ImGui::GetTime();
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

// Run: a paused program carries on; otherwise the editor's program goes into
// memory if it has changed (the browser ran whatever was in memory, which
// was rarely what was in front of the user) and RUN is typed.
void BasicWindow::run() {
  if (!state_.powered) {
    message("The machine is off", true);
    return;
  }
  pulse_ = -2;
  if (state_.running && state_.paused) {
    emulation_.withMachine([](host::MachineHost &host) { host.setPaused(false); });
    lastHit_ = false;
    return;
  }
  // Command-R reaches here whatever the button shows.
  if (!state_.atPrompt) {
    message("The machine must be at the ] prompt to run a program", true);
    return;
  }
  if (!editor_.empty() && (!syncedText_ || *syncedText_ != editor_.text())) {
    if (!write()) return;
  }
  error_.reset();
  heatLevels_.clear();
  stepRequested_ = false;
  emulation_.withMachine([](host::MachineHost &host) {
    Emulator *e = host.emulator();
    if (!e) return;
    e->clearBasicHeatMap();
    host.setPaused(false);
    e->clearBasicBreakpointHit();
    host.pasteText("RUN\r");
  });
  lastHit_ = false;
}

// Pause at the next statement, as the browser does: a step that stops
// wherever the program has got to.
void BasicWindow::pause() {
  pulse_ = -2;
  stepRequested_ = true;
  stopReason_ = "Pausing";
  emulation_.withMachine([](host::MachineHost &host) {
    if (Emulator *e = host.emulator()) e->stepBasicStatement();
  });
}

// Stop, as Control-C stops it: Applesoft says BREAK IN and returns to ].
void BasicWindow::stop() {
  pulse_ = -2;
  stepRequested_ = false;
  emulation_.withMachine([](host::MachineHost &host) {
    Emulator *e = host.emulator();
    if (!e) return;
    if (host.isPaused()) host.setPaused(false);
    e->keyDown(3);
  });
}

void BasicWindow::step(bool wholeLine) {
  pulse_ = -2;
  if (!state_.running || (state_.curlin >> 8) == 0xFF) {
    emulation_.withMachine([](host::MachineHost &host) {
      host.setPaused(false);
      if (Emulator *e = host.emulator()) e->clearBasicBreakpointHit();
    });
    return;
  }
  stepRequested_ = true;
  stopReason_ = "Step";
  lastHit_ = false;
  emulation_.withMachine([wholeLine](host::MachineHost &host) {
    Emulator *e = host.emulator();
    if (!e) return;
    if (wholeLine) e->stepBasicLine();
    else e->stepBasicStatement();
  });
}

// The program in memory, into the editor.
bool BasicWindow::read() {
  if (!state_.powered || !state_.atPrompt) {
    message("The machine must be at the ] prompt to read a program", true);
    return false;
  }
  std::string listing;
  uint32_t hash = 0;
  emulation_.withMachine([&](host::MachineHost &host) {
    Emulator *e = host.emulator();
    if (!e) return;
    MMU &mmu = e->getMMU();
    const uint16_t txttab = static_cast<uint16_t>(mmu.readRAM(0x67, false) | (mmu.readRAM(0x68, false) << 8));
    const uint16_t vartab = static_cast<uint16_t>(mmu.readRAM(0x69, false) | (mmu.readRAM(0x6A, false) << 8));
    if (txttab >= vartab || vartab >= 0xC000) return;
    std::vector<uint8_t> bytes(vartab - txttab);
    for (size_t i = 0; i < bytes.size(); i++) bytes[i] = mmu.readRAM(static_cast<uint16_t>(txttab + i), false);
    const char *text = BasicDetokenizer::detokenizeApplesoft(bytes.data(), static_cast<int>(bytes.size()), false);
    listing = text ? text : "";
    hash = programHash(*e);
  });
  if (listing.find_first_not_of(" \n\r") == std::string::npos) {
    message("There is no BASIC program in memory", true);
    return false;
  }
  editor_.replaceAll(basic::format(listing), CodeEditor::Position{0, 0});
  formattedRevision_ = editor_.revision();
  syncedText_ = editor_.text();
  syncedHash_ = hash;
  state_.programHash = hash;
  error_.reset();
  message("Read " + std::to_string(countLines(editor_.lines())) + " lines from memory");
  return true;
}

// The editor's program, tokenised into memory at $0801.
bool BasicWindow::write() {
  if (!state_.powered) {
    message("The machine is off", true);
    return false;
  }
  if (!state_.atPrompt) {
    message("The machine must be at the ] prompt to write a program", true);
    return false;
  }
  if (editor_.empty()) {
    message("There is no program to write", true);
    return false;
  }
  formatNow(true);
  const std::string source = editor_.text();
  int lines = -1;
  uint32_t hash = 0;
  emulation_.withMachine([&](host::MachineHost &host) {
    Emulator *e = host.emulator();
    if (!e) return;
    lines = a2e::loadBasicProgram(
        source.c_str(), [e](uint16_t a) { return e->readMemory(a); },
        [e](uint16_t a, uint8_t v) { e->writeMemory(a, v); });
    hash = programHash(*e);
  });
  if (lines < 0) {
    message("The program is too large: it would run past $C000", true);
    return false;
  }
  if (lines == 0) {
    message("There are no numbered lines to write", true);
    return false;
  }
  syncedText_ = source;
  syncedHash_ = hash;
  state_.programHash = hash;
  message("Wrote " + std::to_string(lines) + " lines into memory");
  return true;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

int BasicWindow::textLineFor(int basicLine) const {
  const auto &lines = editor_.lines();
  for (size_t i = 0; i < lines.size(); i++) {
    if (basic::lineNumber(lines[i]) == basicLine) return static_cast<int>(i);
  }
  return -1;
}

// Format, keeping the caret on the program line it was on.
void BasicWindow::formatNow(bool keepCaretLine) {
  const std::string formatted = basic::format(editor_.text());
  CodeEditor::Position caret = editor_.caret();
  const auto &lines = editor_.lines();
  const std::optional<int> number =
      caret.line < static_cast<int>(lines.size()) ? basic::lineNumber(lines[static_cast<size_t>(caret.line)]) : std::nullopt;
  if (formatted != editor_.text()) {
    editor_.replaceAll(formatted, caret);
    if (keepCaretLine && number) {
      if (const int l = textLineFor(*number); l >= 0) caret.line = l;
      editor_.setCaret(caret);
    }
  }
  formattedRevision_ = editor_.revision();
  // A runtime error stays only while its line still reads as it did.
  if (error_) {
    const int l = textLineFor(error_->line);
    if (l < 0 || editor_.lines()[static_cast<size_t>(l)] != error_->code) {
      // The same code under another number (a renumber) keeps it.
      int moved = -1;
      for (size_t i = 0; i < editor_.lines().size(); i++) {
        const std::string &line = editor_.lines()[i];
        const auto n = basic::lineNumber(line);
        const auto code = [](const std::string &t) {
          const size_t c = t.find_first_not_of(" 0123456789");
          return c == std::string::npos ? std::string() : t.substr(c);
        };
        if (n && !error_->code.empty() && code(line) == code(error_->code)) moved = *n;
      }
      if (moved >= 0) {
        error_->line = moved;
        error_->code = editor_.lines()[static_cast<size_t>(textLineFor(moved))];
      } else {
        error_.reset();
      }
    }
  }
}

// Renumber, and the breakpoints with it, conditions and all (the browser
// lost them).
void BasicWindow::renumberNow() {
  const basic::Renumbered r = basic::renumber(editor_.text());
  editor_.replaceAll(r.source, CodeEditor::Position{0, 0});
  for (Breakpoint &b : breakpoints_) {
    if (b.isRule()) continue;
    const auto it = r.mapping.find(b.line);
    if (it != r.mapping.end()) b.line = it->second;
  }
  if (error_) {
    const auto it = r.mapping.find(error_->line);
    if (it != r.mapping.end()) {
      error_->line = it->second;
      if (const int l = textLineFor(it->second); l >= 0) error_->code = editor_.lines()[static_cast<size_t>(l)];
    }
  }
  formattedRevision_ = editor_.revision();
  applyBreakpoints();
  message("Renumbered " + std::to_string(r.mapping.size()) + " lines from 10 by 10");
}

void BasicWindow::updateCompletions() {
  const CodeEditor::Position c = editor_.caret();
  const std::string &line = editor_.lines()[static_cast<size_t>(c.line)];
  const std::string before = line.substr(0, static_cast<size_t>(c.column));
  completions_ = basic::complete(editor_.text(), before);
  completionOpen_ = !completions_.empty();
  completionSelected_ = 0;
  completionLine_ = c.line;
  completionTo_ = c.column;
  completionFrom_ = c.column - static_cast<int>(basic::wordBefore(before).size());
}

void BasicWindow::acceptCompletion() {
  if (!completionOpen_ || completions_.empty()) return;
  const basic::Completion &c = completions_[static_cast<size_t>(std::clamp(completionSelected_, 0, static_cast<int>(completions_.size()) - 1))];
  editor_.replaceOnLine(completionFrom_, completionTo_, c.text);
  completionOpen_ = false;
}

void BasicWindow::onTyped(const std::string &typed) {
  const char last = typed.back();
  if (std::isalnum(static_cast<unsigned char>(last)) || last == '$') updateCompletions();
  else completionOpen_ = false;
  // Typing on the error's line is fixing it.
  if (error_ && editor_.caret().line == textLineFor(error_->line)) error_.reset();
}

// The keys the window takes before the editor: the completion list's, and
// Return, which numbers the next line.
bool BasicWindow::onKey(ImGuiKey key, bool shift) {
  if (completionOpen_) {
    const int n = static_cast<int>(completions_.size());
    switch (key) {
    case ImGuiKey_UpArrow: completionSelected_ = (completionSelected_ + n - 1) % n; return true;
    case ImGuiKey_DownArrow: completionSelected_ = (completionSelected_ + 1) % n; return true;
    case ImGuiKey_Tab:
    case ImGuiKey_Enter:
    case ImGuiKey_KeypadEnter: acceptCompletion(); return true;
    case ImGuiKey_Escape: completionOpen_ = false; return true;
    default: break;
    }
  }
  if ((key == ImGuiKey_Enter || key == ImGuiKey_KeypadEnter) && !shift) {
    // The browser's Return: the rest of the line moves down onto a new line
    // ten after this one. Unlike the browser, a number already taken is not
    // used again: the new line goes half way to the next one instead.
    const CodeEditor::Position c = editor_.caret();
    std::vector<std::string> lines = editor_.lines();
    const std::string current = lines[static_cast<size_t>(c.line)];
    const std::string head = current.substr(0, static_cast<size_t>(c.column));
    const std::string tail = trimLeft(current.substr(static_cast<size_t>(c.column)));
    int number = basic::nextLineNumber(lines, c.line);
    std::set<int> taken;
    for (const std::string &l : lines) {
      if (const auto n = basic::lineNumber(l)) taken.insert(*n);
    }
    if (taken.count(number)) {
      const int from = number - 10;
      const auto next = taken.upper_bound(from);
      const int limit = next == taken.end() ? 64000 : *next;
      number = from + std::max(1, (limit - from) / 2);
      while (taken.count(number) && number < 63999) number++;
    }
    lines[static_cast<size_t>(c.line)] = head;
    lines.insert(lines.begin() + c.line + 1, std::to_string(number) + " " + tail);
    std::string joined;
    for (size_t i = 0; i < lines.size(); i++) joined += (i ? "\n" : "") + lines[i];
    editor_.replaceAll(basic::format(joined));
    const int at = textLineFor(number);
    if (at >= 0) {
      const std::string &line = editor_.lines()[static_cast<size_t>(at)];
      // After the number, its space and the indent: where the new text goes.
      size_t col = line.find_first_not_of(' ');
      col = line.find(' ', col);
      col = col == std::string::npos ? line.size() : line.find_first_not_of(' ', col);
      if (col == std::string::npos) col = line.size();
      const int column = tail.empty() ? static_cast<int>(line.size()) : static_cast<int>(col);
      editor_.setCaret({at, column});
    }
    formattedRevision_ = editor_.revision();
    lastCaretLine_ = editor_.caret().line;
    return true;
  }
  return false;
}

const BasicWindow::Breakpoint *BasicWindow::breakpointAt(int line, int statement) const {
  for (const Breakpoint &b : breakpoints_) {
    if (!b.isRule() && b.line == line && b.statement == statement) return &b;
  }
  return nullptr;
}

bool BasicWindow::lineHasBreakpoint(int line) const {
  for (const Breakpoint &b : breakpoints_) {
    if (!b.isRule() && b.line == line) return true;
  }
  return false;
}

// A gutter click: any breakpoint on the line goes, or a whole-line one
// arrives. An Option-click on a statement does the same for that statement.
void BasicWindow::toggleBreakpoint(int line, int statement) {
  if (statement < 0 && lineHasBreakpoint(line)) {
    breakpoints_.erase(std::remove_if(breakpoints_.begin(), breakpoints_.end(),
                                      [line](const Breakpoint &b) { return !b.isRule() && b.line == line; }),
                       breakpoints_.end());
  } else if (const Breakpoint *b = breakpointAt(line, statement)) {
    breakpoints_.erase(breakpoints_.begin() + (b - breakpoints_.data()));
  } else {
    Breakpoint added;
    added.line = line;
    added.statement = statement;
    breakpoints_.push_back(added);
    std::stable_sort(breakpoints_.begin(), breakpoints_.end(), [](const Breakpoint &x, const Breakpoint &y) {
      return x.line != y.line ? x.line < y.line : x.statement < y.statement;
    });
  }
  pulse_ = -2;
  applyBreakpoints();
}

void BasicWindow::editValue(uint16_t at, BasicVarType type, const std::string &input, int stringLength) {
  std::string value = input;
  emulation_.withMachine([&](host::MachineHost &host) {
    Emulator *e = host.emulator();
    if (!e) return;
    if (type == BasicVarType::Integer) {
      const long v = std::strtol(value.c_str(), nullptr, 10);
      if (v < -32768 || v > 32767) return;
      e->writeMemory(at, static_cast<uint8_t>((v >> 8) & 0xFF));
      e->writeMemory(static_cast<uint16_t>(at + 1), static_cast<uint8_t>(v & 0xFF));
    } else if (type == BasicVarType::Real) {
      uint8_t bytes[APPLESOFT_FLOAT_SIZE];
      ApplesoftVars::encodeFloat(std::strtod(value.c_str(), nullptr), bytes);
      for (int i = 0; i < APPLESOFT_FLOAT_SIZE; i++) e->writeMemory(static_cast<uint16_t>(at + i), bytes[i]);
    } else {
      // A string is changed where it is: its descriptor's length and the
      // bytes it points at, never longer than it was, and in plain ASCII as
      // Applesoft stores it (the browser set the top bit, which no
      // comparison then matched).
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      if (static_cast<int>(value.size()) > stringLength) value.resize(static_cast<size_t>(stringLength));
      const uint16_t ptr = static_cast<uint16_t>(e->peekMemory(static_cast<uint16_t>(at + 1)) |
                                                 (e->peekMemory(static_cast<uint16_t>(at + 2)) << 8));
      e->writeMemory(at, static_cast<uint8_t>(value.size()));
      for (size_t i = 0; i < value.size(); i++) {
        e->writeMemory(static_cast<uint16_t>(ptr + i), static_cast<uint8_t>(value[i] & 0x7F));
      }
    }
  });
  variablesAt_ = -10.0;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void BasicWindow::drawToolbar() {
  const Palette &p = palette();
  const bool on = state_.powered;
  const bool paused = state_.running && state_.paused;
  ImGui::PushID("toolbar");
  // Run means typing RUN at Applesoft's prompt, so it is offered only there,
  // or to carry on a program stopped in this window.
  const bool canRun = on && (state_.atPrompt || paused);
  if (toolButton("##run", Icon::Run, paused ? "Continue" : "Run",
                 paused  ? "Carry on from here  (\xE2\x8C\x98R)"
                 : canRun ? "Write the program if it has changed, then RUN it  (\xE2\x8C\x98R)"
                          : "Waiting for Applesoft's ] prompt",
                 canRun, p.green, true)) {
    run();
  }
  ImGui::SameLine(0, 6);
  if (toolButton("##pause", Icon::Pause, "Pause", "Stop at the next statement", on && state_.running && !paused)) pause();
  ImGui::SameLine(0, 2);
  if (toolButton("##stop", Icon::Stop, "Stop", "Stop the program, as Control-C does  (\xE2\x8C\x98.)", on && state_.running,
                 p.red)) {
    stop();
  }
  ImGui::SameLine(0, 10);
  if (toolButton("##step", Icon::Step, "Step", "Run one statement and stop", on && state_.running)) step(false);
  ImGui::SameLine(0, 2);
  if (toolButton("##stepline", Icon::StepLine, "Step Line", "Run to the next line and stop", on && state_.running)) {
    step(true);
  }
  ImGui::SameLine(0, 14);

  // What it is doing: idle, running with a slow breath, paused, or stopped
  // on an error, which stays until it runs again.
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float height = ImGui::GetFrameHeight() + 6;
  std::string label = "Idle";
  ImU32 colour = secondary();
  if (!on) label = "Off";
  else if (paused) {
    label = stopReason_.empty() || stopReason_ == "Pausing" ? "Paused" : stopReason_;
    if (currentLine_) label += " at " + std::to_string(*currentLine_);
    colour = stoppedAtBreak_ ? p.red : p.yellow;
  } else if (state_.running) {
    label = "Running";
    colour = p.green;
  } else if (error_) {
    label = "?" + error_->message + " in " + std::to_string(error_->line);
    colour = p.red;
  }
  const float w = ImGui::CalcTextSize(label.c_str()).x + 34;
  draw->AddRectFilled(at, ImVec2(at.x + w, at.y + height), withAlpha(colour, 0.16f), height * 0.5f);
  draw->AddRect(at, ImVec2(at.x + w, at.y + height), withAlpha(colour, 0.55f), height * 0.5f);
  const ImVec2 dot(at.x + 14, at.y + height * 0.5f);
  if (state_.running && !paused) {
    const float breath = 0.6f + 0.4f * (0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f));
    draw->AddCircleFilled(dot, 6.5f, withAlpha(colour, 0.25f * breath));
  }
  draw->AddCircleFilled(dot, 4.0f, colour);
  draw->AddText(ImVec2(at.x + 24, at.y + (height - ImGui::GetTextLineHeight()) * 0.5f), text(), label.c_str());
  ImGui::Dummy(ImVec2(w, height));

  // The view switches, on the right.
  const float switches = ui::SwitchWidth("Trace") + ui::SwitchWidth("Heat") + 16;
  ImGui::SameLine(0, 12);
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - switches));
  // Both switches centred on the row of taller buttons.
  const float switchY = ImGui::GetCursorPosY() + 3;
  ImGui::SetCursorPosY(switchY);
  if (ui::Switch("Trace", &trace_)) ImGui::MarkIniSettingsDirty();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Follow the line running");
  ImGui::SameLine(0, 16);
  ImGui::SetCursorPosY(switchY);
  if (ui::Switch("Heat", &heat_)) {
    heatLevels_.clear();
    ImGui::MarkIniSettingsDirty();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shade each line by how often it runs");

  // The second row: the program, its file, and memory.
  ImGui::Dummy(ImVec2(0, 2));
  if (ui::Button("New")) {
    if (editor_.empty()) editor_.setText("");
    else confirmNew_ = true;
  }
  ImGui::SameLine(0, 4);
  if (ui::Button("Open\xE2\x80\xA6") && platform_.openFile) {
    platform_.openFile("Open a BASIC program", {"bas", "txt"}, [this](const std::string &path) {
      if (path.empty()) return;
      if (auto bytes = readFile(path)) {
        editor_.setText(std::string(bytes->begin(), bytes->end()));
        filePath_ = path;
        syncedText_.reset();
        error_.reset();
        formattedRevision_ = editor_.revision();
        ImGui::MarkIniSettingsDirty();
      } else {
        message("Could not read the file", true);
      }
    });
  }
  auto saveTo = [this](const std::string &path) {
    const std::string body = editor_.text() + "\n";
    if (writeFile(path, reinterpret_cast<const uint8_t *>(body.data()), body.size())) {
      filePath_ = path;
      message("Saved");
      ImGui::MarkIniSettingsDirty();
    } else {
      message("Could not write the file", true);
    }
  };
  auto saveAs = [this, saveTo] {
    if (!platform_.saveFile) return;
    std::string name = filePath_.empty() ? "program.bas" : filePath_.substr(filePath_.find_last_of('/') + 1);
    platform_.saveFile("Save the BASIC program", name, {"bas"}, [saveTo](const std::string &path) {
      if (!path.empty()) saveTo(path);
    });
  };
  ImGui::SameLine(0, 4);
  ImGui::BeginDisabled(editor_.empty());
  if (ui::Button("Save")) {
    if (filePath_.empty()) saveAs();
    else saveTo(filePath_);
  }
  ImGui::SameLine(0, 4);
  if (ui::Button("Save As\xE2\x80\xA6")) saveAs();
  ImGui::EndDisabled();

  // Read and Write go through Applesoft's program memory, which is only the
  // program's while Applesoft sits at its prompt.
  const bool atPrompt = state_.powered && state_.atPrompt;
  const char *notAtPrompt = "Waiting for Applesoft's ] prompt";
  ImGui::SameLine(0, 16);
  ImGui::BeginDisabled(!atPrompt);
  if (ui::Button("Read")) read();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", atPrompt ? "Take the program out of memory into the editor" : notAtPrompt);
  }
  ImGui::SameLine(0, 4);
  if (ui::Button("Write")) write();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", atPrompt ? "Put the editor's program into memory at $0801" : notAtPrompt);
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0, 16);
  if (ui::Button("Format")) formatNow(true);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Order, align, indent and capitalise the lines");
  ImGui::SameLine(0, 4);
  if (ui::Button("Renum")) renumberNow();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Renumber from 10 by 10, with every GOTO, GOSUB and THEN");

  // Whether the editor and memory agree.
  ImGui::SameLine(0, 16);
  const ImVec2 chipAt = ImGui::GetCursorScreenPos();
  const char *sync = "Not in memory";
  ImU32 syncColour = secondary();
  const char *tip = "The editor's program has not been read from or written to memory";
  if (editor_.empty()) {
    sync = "Empty";
    tip = "Nothing to write";
  } else if (syncedText_ && *syncedText_ != editor_.text()) {
    sync = "Edited";
    syncColour = p.orange;
    tip = "The editor has changes memory does not: Run writes them first";
  } else if (syncedText_ && state_.programHash != 0 && state_.programHash != syncedHash_) {
    sync = "Memory changed";
    syncColour = p.blue;
    tip = "The program in memory has changed since it was read or written: Read takes it";
  } else if (syncedText_) {
    sync = "In memory";
    syncColour = p.green;
    tip = "The editor and memory hold the same program";
  }
  const float chipHeight = ImGui::GetFrameHeight();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 ts = ImGui::CalcTextSize(sync);
  ImGui::PopFont();
  const ImVec2 chipEnd(chipAt.x + ts.x + 26, chipAt.y + chipHeight);
  ImGui::Dummy(ImVec2(chipEnd.x - chipAt.x, chipHeight));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  draw->AddRectFilled(chipAt, chipEnd, withAlpha(syncColour, 0.14f), chipHeight * 0.5f);
  draw->AddCircleFilled(ImVec2(chipAt.x + 10, chipAt.y + chipHeight * 0.5f), 3.0f, syncColour);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(ImVec2(chipAt.x + 18, chipAt.y + (chipHeight - ts.y) * 0.5f), syncColour, sync);
  ImGui::PopFont();
  if (!filePath_.empty()) {
    ImGui::SameLine(0, 10);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", filePath_.substr(filePath_.find_last_of('/') + 1).c_str());
  }
  ImGui::PopID();
}

void BasicWindow::drawEditor(ImVec2 size) {
  const Palette &p = palette();
  const ImGuiIO &io = ImGui::GetIO();
  const auto &lines = editor_.lines();
  const std::optional<CodeEditor::Position> hover = editor_.hovered();
  const int currentText = currentLine_ ? textLineFor(*currentLine_) : -1;
  const int errorText = error_ ? textLineFor(error_->line) : -1;

  CodeEditor::Hooks hooks;
  // Colours, with the rest of a multi-statement line dimmed while one of its
  // statements is the one stopped on.
  hooks.colour = [&](int line, const std::string &t) {
    std::vector<CodeEditor::Run> runs;
    const std::vector<basic::Statement> st = line == currentText && currentStatement_ >= 0 ? basic::statements(t)
                                                                                         : std::vector<basic::Statement>{};
    for (const basic::Span &s : basic::highlight(t)) {
      ImU32 c = colourFor(s.kind);
      if (!st.empty() && currentStatement_ < static_cast<int>(st.size())) {
        const basic::Statement &cur = st[static_cast<size_t>(currentStatement_)];
        if (s.kind != basic::Kind::LineNumber && (s.start + s.length <= cur.start || s.start >= cur.end + 1)) {
          c = withAlpha(c, 0.5f);
        }
      }
      runs.push_back({s.start, s.length, c});
    }
    return runs;
  };

  // The gutter: breakpoints, the line running, the error, and the heat.
  hooks.gutter = [&](ImDrawList *draw, int line, ImVec2 a, ImVec2 b) {
    const std::string &t = lines[static_cast<size_t>(line)];
    const std::optional<int> number = basic::lineNumber(t);
    if (!number) return;
    const float cy = (a.y + b.y) * 0.5f;
    if (heat_) {
      if (const auto it = heatLevels_.find(*number); it != heatLevels_.end() && it->second > 0.005f) {
        draw->AddRectFilled(a, b, heatColour(it->second, 0.10f + 0.25f * it->second));
        draw->AddRectFilled(ImVec2(a.x, a.y + 1), ImVec2(a.x + 4, b.y - 1), heatColour(it->second, 0.95f), 2.0f);
      }
    }
    const float bx = a.x + 16;
    if (line == errorText) {
      draw->AddCircleFilled(ImVec2(bx, cy), 6.5f, p.red);
      draw->AddRectFilled(ImVec2(bx - 1, cy - 4), ImVec2(bx + 1, cy + 1), IM_COL32_WHITE);
      draw->AddRectFilled(ImVec2(bx - 1, cy + 2.5f), ImVec2(bx + 1, cy + 4), IM_COL32_WHITE);
    } else {
      // A whole-line breakpoint is a full disc, statements only a smaller
      // one; a disabled one is a ring, and one with a condition wears an
      // orange ring.
      bool whole = false, any = false, enabled = false, conditional = false;
      for (const Breakpoint &bp : breakpoints_) {
        if (bp.isRule() || bp.line != *number) continue;
        any = true;
        whole = whole || bp.statement < 0;
        enabled = enabled || bp.enabled;
        conditional = conditional || !bp.condition.empty();
      }
      if (any) {
        const float r = whole ? 5.5f : 4.0f;
        if (enabled) draw->AddCircleFilled(ImVec2(bx, cy), r, p.red);
        else draw->AddCircle(ImVec2(bx, cy), r, withAlpha(p.red, 0.8f), 0, 1.5f);
        if (conditional) draw->AddCircle(ImVec2(bx, cy), r + 2.5f, p.orange, 0, 1.5f);
      } else if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MousePos.y >= a.y && io.MousePos.y < b.y &&
                 io.MousePos.x >= a.x && io.MousePos.x < b.x) {
        draw->AddCircle(ImVec2(bx, cy), 5.0f, withAlpha(p.red, 0.45f), 0, 1.2f);
      }
    }
    if (line == currentText) {
      const ImU32 c = stoppedAtBreak_ ? p.red : state_.paused ? p.yellow : p.green;
      const float x = a.x + 30;
      draw->AddTriangleFilled(ImVec2(x, cy - 5), ImVec2(x, cy + 5), ImVec2(x + 8, cy), c);
      draw->AddRectFilled(ImVec2(x - 4, cy - 2), ImVec2(x + 1, cy + 2), c);
    }
  };

  // Behind the text: the line running or stopped on, the error, the
  // statement under the pointer; in front: statement breakpoints and the
  // error's message.
  hooks.decorate = [&](ImDrawList *draw, int line, ImVec2 a, ImVec2 b, float textX, float charWidth, bool front) {
    const std::string &t = lines[static_cast<size_t>(line)];
    if (!basic::lineNumber(t)) return;
    const std::vector<basic::Statement> st = basic::statements(t);
    auto span = [&](int s, float inset) {
      const basic::Statement &x = st[static_cast<size_t>(s)];
      int from = x.start, to = x.end;
      while (from < to && t[static_cast<size_t>(from)] == ' ') from++;
      return std::pair<float, float>(textX + from * charWidth - inset, textX + to * charWidth + inset);
    };
    if (!front) {
      if (line == currentText) {
        const ImU32 c = stoppedAtBreak_ ? p.red : state_.paused ? p.yellow : accent();
        draw->AddRectFilled(a, b, withAlpha(c, 0.13f));
        draw->AddRectFilled(a, ImVec2(a.x + 3, b.y), withAlpha(c, 0.9f));
        if (currentStatement_ >= 0 && currentStatement_ < static_cast<int>(st.size()) && st.size() > 1) {
          const auto [x0, x1] = span(currentStatement_, 3);
          draw->AddRectFilled(ImVec2(x0, a.y + 1), ImVec2(x1, b.y - 1), withAlpha(c, 0.22f), 4.0f);
        }
      }
      if (line == errorText) {
        draw->AddRectFilled(a, b, withAlpha(p.red, 0.10f));
        if (error_->statement >= 0 && error_->statement < static_cast<int>(st.size()) && st.size() > 1) {
          const auto [x0, x1] = span(error_->statement, 3);
          draw->AddRectFilled(ImVec2(x0, a.y + 1), ImVec2(x1, b.y - 1), withAlpha(p.red, 0.20f), 4.0f);
        }
      }
      if (hover && hover->line == line) {
        const int s = basic::statementAt(t, hover->column);
        if (s >= 0) {
          const auto [x0, x1] = span(s, 3);
          draw->AddRectFilled(ImVec2(x0, a.y + 1), ImVec2(x1, b.y - 1), withAlpha(p.blue, io.KeyAlt ? 0.20f : 0.07f), 4.0f);
        }
      }
      return;
    }
    // Statement breakpoints: dashed under their statement, solid while the
    // program is stopped on one.
    const std::optional<int> number = basic::lineNumber(t);
    for (const Breakpoint &bp : breakpoints_) {
      if (bp.isRule() || bp.line != *number || bp.statement < 0 || bp.statement >= static_cast<int>(st.size())) continue;
      const auto [x0, x1] = span(bp.statement, 0);
      const float y = b.y - 2;
      const ImU32 c = bp.enabled ? p.red : withAlpha(p.red, 0.4f);
      const bool solid = line == currentText && currentStatement_ == bp.statement && stoppedAtBreak_;
      if (solid) draw->AddLine(ImVec2(x0, y), ImVec2(x1, y), c, 2.0f);
      else {
        for (float x = x0; x < x1; x += 6) draw->AddLine(ImVec2(x, y), ImVec2(std::min(x + 3.5f, x1), y), c, 2.0f);
      }
    }
    if (line == errorText) {
      const std::string m = "?" + error_->message;
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
      const ImVec2 ms = ImGui::CalcTextSize(m.c_str());
      const float x = std::max(textX + (t.size() + 3) * charWidth, a.x + 40);
      const ImVec2 pa(x, (a.y + b.y - ms.y) * 0.5f - 2), pb(x + ms.x + 14, (a.y + b.y + ms.y) * 0.5f + 2);
      draw->AddRectFilled(pa, pb, p.red, (pb.y - pa.y) * 0.5f);
      draw->AddText(ImVec2(pa.x + 7, pa.y + 2), ui::textOn(p.red), m.c_str());
      ImGui::PopFont();
    }
  };

  hooks.gutterClicked = [&](int line) {
    if (const auto n = basic::lineNumber(lines[static_cast<size_t>(line)])) toggleBreakpoint(*n, -1);
  };
  hooks.optionClicked = [&](CodeEditor::Position at) {
    const std::string &t = lines[static_cast<size_t>(at.line)];
    const auto n = basic::lineNumber(t);
    const int s = basic::statementAt(t, at.column);
    if (n && s >= 0) toggleBreakpoint(*n, s);
  };
  hooks.key = [this](ImGuiKey key, bool shift) { return onKey(key, shift); };
  hooks.typed = [this](const std::string &typed) { onTyped(typed); };
  // A paste is formatted at once, as the browser does, once the editor has
  // finished with this frame's keys.
  hooks.pasted = [this] { formatPending_ = true; };

  ImGui::PushStyleColor(ImGuiCol_ChildBg, well());
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
  const char *placeholder = "Type your Applesoft program here, or Read the one in memory.\n\n"
                            "10 REM EXAMPLE PROGRAM\n20 HOME\n30 FOR I = 1 TO 10\n40 PRINT \"HELLO WORLD \";I\n50 NEXT I\n60 END";
  editor_.draw("##basiceditor", size, GUTTER, hooks, placeholder);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Format, as the browser does, after a paste, when the caret leaves a line
  // it changed and when the editor is left.
  const bool focused = editor_.focused();
  const int caretLine = editor_.caret().line;
  if (formatPending_ || (editor_.revision() != formattedRevision_ &&
                         ((focused && lastCaretLine_ >= 0 && caretLine != lastCaretLine_) ||
                          (editorWasFocused_ && !focused)))) {
    formatPending_ = false;
    formatNow(true);
  }
  if (completionOpen_ && (caretLine != completionLine_ || (!focused && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)))) {
    completionOpen_ = false;
  }
  lastCaretLine_ = focused ? editor_.caret().line : -1;
  editorWasFocused_ = focused;
}

// The completion list, under the word being typed: each entry with the
// typed part picked out, its kind on the right, and the chosen one's syntax
// and meaning beneath.
void BasicWindow::drawCompletions() {
  if (!completionOpen_ || completions_.empty()) return;
  const Palette &p = palette();
  const ImVec2 at = editor_.caretScreenPos();
  ImGui::SetNextWindowPos(ImVec2(at.x - 6, at.y + editor_.lineHeight() + 2));
  ImGui::SetNextWindowSizeConstraints(ImVec2(280, 0), ImVec2(320, 400));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoDocking;
  if (ImGui::Begin("##basiccompletions", nullptr, flags)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    const float row = ImGui::GetTextLineHeight() + 6;
    const int typed = completionTo_ - completionFrom_;
    for (size_t i = 0; i < completions_.size(); i++) {
      const basic::Completion &c = completions_[i];
      const ImVec2 r0 = ImGui::GetCursorScreenPos();
      const float width = 300;
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::InvisibleButton("##item", ImVec2(width, row))) {
        completionSelected_ = static_cast<int>(i);
        acceptCompletion();
        editor_.focus();
      }
      if (ImGui::IsItemHovered()) completionSelected_ = static_cast<int>(i);
      ImGui::PopID();
      const bool chosen = static_cast<int>(i) == completionSelected_;
      if (chosen) draw->AddRectFilled(r0, ImVec2(r0.x + width, r0.y + row), accent(0.22f), 5.0f);
      using T = basic::Completion::Type;
      const ImU32 kind = c.type == T::Variable ? p.orange : c.type == T::LineNumber ? p.blue : c.type == T::Function ? p.purple
                                                                                                                     : text();
      const float ty = r0.y + 3;
      const int n = std::min(typed, static_cast<int>(c.text.size()));
      draw->AddText(ImVec2(r0.x + 8, ty), c.type == T::Keyword ? p.green : kind, c.text.c_str(), c.text.c_str() + n);
      const float w = ImGui::CalcTextSize(c.text.c_str(), c.text.c_str() + n).x;
      draw->AddText(ImVec2(r0.x + 8 + w, ty), c.type == T::Keyword ? text() : kind, c.text.c_str() + n);
      ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
      const ImVec2 cs = ImGui::CalcTextSize(c.category.c_str());
      draw->AddText(ImVec2(r0.x + width - cs.x - 8, r0.y + (row - cs.y) * 0.5f), secondary(), c.category.c_str());
      ImGui::PopFont();
    }
    ImGui::PopFont();
    const basic::Completion &c = completions_[static_cast<size_t>(std::clamp(completionSelected_, 0, static_cast<int>(completions_.size()) - 1))];
    ImGui::Separator();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(p.blue), "%s", c.syntax.c_str());
    ImGui::PopFont();
    if (!c.description.empty()) ImGui::TextDisabled("%s", c.description.c_str());
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void BasicWindow::drawVariables(float width, float height) {
  const Palette &p = palette();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  card(draw, start, ImVec2(start.x + width, start.y + height));
  char head[48];
  std::snprintf(head, sizeof head, "VARIABLES  %zu", variables_.size() + arrays_.size());
  caption(draw, ImVec2(start.x + PAD, start.y + 10), head);
  if (state_.paused && state_.running) {
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
    const char *hint = "CLICK A VALUE TO CHANGE IT";
    draw->AddText(ImVec2(start.x + width - PAD - ImGui::CalcTextSize(hint).x, start.y + 10), withAlpha(accent(), 0.8f), hint);
    ImGui::PopFont();
  }
  ImGui::SetCursorScreenPos(ImVec2(start.x + 6, start.y + 28));
  ImGui::BeginChild("##vars", ImVec2(width - 12, height - 34), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  const bool editable = state_.paused && state_.running;
  const double now = ImGui::GetTime();
  if (variables_.empty() && arrays_.empty()) ImGui::TextDisabled(" No variables");

  auto typeColour = [&](BasicVarType t) {
    return t == BasicVarType::String ? p.green : t == BasicVarType::Integer ? p.blue : text(0.9f);
  };
  auto valueText = [](BasicVarType t, double real, int32_t integer, const std::string &s) {
    return t == BasicVarType::String ? "\"" + s + "\"" : t == BasicVarType::Integer ? std::to_string(integer) : basic::formatReal(real);
  };
  // A value, editable while the program is stopped: a click turns it into a
  // field, Return writes it and Escape leaves it.
  auto value = [&](const std::string &key, BasicVarType t, const std::string &shown, uint16_t at, int strLen, float x0,
                   float x1, ImU32 colour) {
    const float y = ImGui::GetCursorScreenPos().y;
    if (editingKey_ == key) {
      ImGui::SetCursorScreenPos(ImVec2(x0, y - 2));
      ImGui::SetNextItemWidth(x1 - x0);
      // ImGui applies a focus request a frame or two after it is made, so
      // the field keeps asking until it has the keyboard, and only then
      // does losing it close the field.
      if (focusEdit_ > 0) {
        ImGui::SetKeyboardFocusHere();
        focusEdit_--;
      }
      ImGui::PushFont(ui::monoFont(), 0.0f);
      const bool enter = ImGui::InputText("##edit", editText_, sizeof editText_,
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      ImGui::PopFont();
      if (ImGui::IsItemActive()) focusEdit_ = 0;
      if (enter) {
        editValue(at, t, editText_, strLen);
        editingKey_.clear();
      } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsItemActive() && !ImGui::IsItemFocused() && focusEdit_ == 0)) {
        editingKey_.clear();
      }
      return;
    }
    ImGui::PushFont(ui::monoFont(), 0.0f);
    const ImVec2 vs = ImGui::CalcTextSize(shown.c_str());
    ImGui::PopFont();
    const ImVec2 a(x0, y), b(x1, y + ImGui::GetTextLineHeight() + 4);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(key.c_str());
    const bool clicked = ImGui::InvisibleButton("##v", ImVec2(std::max(1.0f, x1 - x0), b.y - a.y));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    // The scrolling list's own draw list, which is clipped to the list: the
    // card's is not, and a highlight drawn on it showed through outside.
    ImDrawList *d = ImGui::GetWindowDrawList();
    if (const auto it = changedAt_.find(key); it != changedAt_.end() && now - it->second < FLASH) {
      d->AddRectFilled(a, b, withAlpha(p.yellow, 0.35f * static_cast<float>(1.0 - (now - it->second) / FLASH)), 4.0f);
    }
    if (editable && hovered) d->AddRect(a, b, accent(0.6f), 4.0f);
    ImGui::PushFont(ui::monoFont(), 0.0f);
    d->PushClipRect(a, b, true);
    d->AddText(ImVec2(std::max(x0 + 2, x1 - vs.x - 4), y + 2), colour, shown.c_str());
    d->PopClipRect();
    ImGui::PopFont();
    if (hovered && vs.x > x1 - x0 - 6) ImGui::SetTooltip("%s", shown.c_str());
    if (editable && clicked) {
      editingKey_ = key;
      std::snprintf(editText_, sizeof editText_, "%s", t == BasicVarType::String ? shown.substr(1, shown.size() - 2).c_str() : shown.c_str());
      focusEdit_ = 4;
    }
  };

  const float inner = ImGui::GetContentRegionAvail().x;
  const float x0 = ImGui::GetCursorScreenPos().x;
  for (const BasicVariableInfo &v : variables_) {
    const float y = ImGui::GetCursorScreenPos().y;
    ImDrawList *d = ImGui::GetWindowDrawList();
    // The name, with its type as a small badge.
    const char *badge = v.type == BasicVarType::String ? "$" : v.type == BasicVarType::Integer ? "%" : "R";
    const ImU32 tc = typeColour(v.type);
    d->AddRectFilled(ImVec2(x0 + 4, y + 3), ImVec2(x0 + 18, y + ImGui::GetTextLineHeight() + 1), withAlpha(tc, 0.18f), 4.0f);
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
    d->AddText(ImVec2(x0 + 8, y + 4), tc, badge);
    ImGui::PopFont();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    d->AddText(ImVec2(x0 + 24, y + 2), text(), v.name.c_str());
    ImGui::PopFont();
    value(v.name, v.type, valueText(v.type, v.realValue, v.intValue, v.stringValue), static_cast<uint16_t>(v.address + 2),
          v.type == BasicVarType::String ? static_cast<int>(v.stringValue.size()) : 0, x0 + inner * 0.38f, x0 + inner - 2,
          tc);
    ImGui::SetCursorScreenPos(ImVec2(x0, y + ImGui::GetTextLineHeight() + 6));
    ImGui::Dummy(ImVec2(0, 0));
  }

  // Arrays: a heading to open each, then its elements as a table. Applesoft
  // stores the sizes last subscript first and the elements first subscript
  // fastest, so `sizes` puts them back in the order the DIM wrote them.
  for (const BasicArrayInfo &arr : arrays_) {
    const float y = ImGui::GetCursorScreenPos().y;
    ImDrawList *d = ImGui::GetWindowDrawList();
    const bool expanded = expandedArrays_.count(arr.name) != 0;
    const std::vector<uint32_t> sizes(arr.dimensions.rbegin(), arr.dimensions.rend());
    std::string dims;
    for (size_t i = 0; i < sizes.size(); i++) {
      dims += (i ? "," : "") + std::to_string(sizes[i] - 1);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushID(arr.name.c_str());
    if (ImGui::InvisibleButton("##arr", ImVec2(inner, ImGui::GetTextLineHeight() + 4))) {
      if (expanded) expandedArrays_.erase(arr.name);
      else expandedArrays_.insert(arr.name);
    }
    ImGui::PopID();
    const float cx = x0 + 10, cy = y + (ImGui::GetTextLineHeight() + 4) * 0.5f;
    if (expanded) d->AddTriangleFilled(ImVec2(cx - 4, cy - 2), ImVec2(cx + 4, cy - 2), ImVec2(cx, cy + 3), secondary());
    else d->AddTriangleFilled(ImVec2(cx - 2, cy - 4), ImVec2(cx - 2, cy + 4), ImVec2(cx + 3, cy), secondary());
    ImGui::PushFont(ui::monoFont(), 0.0f);
    const std::string title = arr.name + "(" + dims + ")";
    d->AddText(ImVec2(x0 + 20, y + 2), typeColour(arr.type), title.c_str());
    char count[24];
    std::snprintf(count, sizeof count, "%u el", arr.elementCount);
    const ImVec2 cs = ImGui::CalcTextSize(count);
    d->AddText(ImVec2(x0 + inner - cs.x - 4, y + 2), secondary(), count);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(x0, y + ImGui::GetTextLineHeight() + 6));
    if (!expanded) continue;

    const int size = ApplesoftVars::elementSize(arr.type);
    const uint16_t dataAt = static_cast<uint16_t>(arr.address + 5 + arr.numDims * 2);
    auto element = [&](uint32_t i) {
      if (arr.type == BasicVarType::String) return i < arr.stringValues.size() ? "\"" + arr.stringValues[i] + "\"" : std::string();
      if (arr.type == BasicVarType::Integer) return i < arr.intValues.size() ? std::to_string(arr.intValues[i]) : std::string();
      return i < arr.realValues.size() ? basic::formatReal(arr.realValues[i]) : std::string();
    };
    if (arr.elementCount == 0 || sizes.empty()) continue;

    // The table's shape. Rows are the first subscript and columns the
    // second; a third and beyond make one table per combination of them,
    // captioned with it. A single subscript is a column of values, wrapped
    // into as many index/value pairs as the width takes.
    const uint32_t rows = sizes[0];
    const uint32_t cols = sizes.size() > 1 ? sizes[1] : 1;
    const uint32_t perSlice = rows * cols;
    const uint32_t slices = std::max<uint32_t>(1, arr.elementCount / std::max<uint32_t>(1, perSlice));
    const uint32_t shownSlices = std::min<uint32_t>(slices, 16);
    const bool oneDim = sizes.size() == 1;

    // Every column as wide as the widest value, so a table reads down.
    ImGui::PushFont(ui::monoFont(), 0.0f);
    float valueWidth = ImGui::CalcTextSize("0").x * 3;
    for (uint32_t i = 0; i < std::min<uint32_t>(arr.elementCount, 2000); i++) {
      valueWidth = std::max(valueWidth, ImGui::CalcTextSize(element(i).c_str()).x);
    }
    const float indexWidth = ImGui::CalcTextSize(std::to_string(std::max(rows, cols) - 1).c_str()).x;
    ImGui::PopFont();
    valueWidth = std::min(valueWidth + 12, 180.0f);
    const float headWidth = indexWidth + 14;
    const float rowHeight = ImGui::GetTextLineHeight() + 6;
    const float tableWidth = inner - 22;
    // A single subscript wraps into side by side index/value pairs.
    const uint32_t wrap =
        oneDim ? std::clamp<uint32_t>(static_cast<uint32_t>(tableWidth / (headWidth + valueWidth)), 1, 8) : 1;
    const uint32_t tableRows = oneDim ? (rows + wrap - 1) / wrap : rows;

    const ImU32 tc = typeColour(arr.type);
    auto header = [&](const std::string &label, bool right) {
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
      ImGui::GetWindowDrawList()->AddText(ImVec2(right ? at.x + w - ls.x - 4 : at.x + (w - ls.x) * 0.5f, at.y + 3),
                                          secondary(), label.c_str());
      ImGui::PopFont();
      ImGui::Dummy(ImVec2(w, rowHeight - 4));
    };
    auto cell = [&](uint32_t i) {
      const float cx0 = ImGui::GetCursorScreenPos().x;
      const float cx1 = cx0 + ImGui::GetContentRegionAvail().x;
      ImGui::SetCursorScreenPos(ImVec2(cx0, ImGui::GetCursorScreenPos().y + 1));
      const std::string shownValue = element(i);
      value(arr.name + "#" + std::to_string(i), arr.type, shownValue, static_cast<uint16_t>(dataAt + i * size),
            arr.type == BasicVarType::String ? static_cast<int>(shownValue.size()) - 2 : 0, cx0, cx1, tc);
    };

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 1));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, withAlpha(tc, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, withAlpha(text(), 0.08f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, withAlpha(text(), 0.16f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, withAlpha(text(), 0.035f));
    for (uint32_t s = 0; s < shownSlices; s++) {
      // A slice's caption: the subscripts past the second, as (*,*,k).
      if (sizes.size() > 2) {
        std::string label = "(*,*";
        uint32_t rest = s;
        for (size_t k = 2; k < sizes.size(); k++) {
          label += "," + std::to_string(rest % sizes[k]);
          rest /= sizes[k];
        }
        label += ")";
        ImGui::SetCursorScreenPos(ImVec2(x0 + 22, ImGui::GetCursorScreenPos().y + 2));
        ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(secondary()), "%s", label.c_str());
        ImGui::PopFont();
      }
      const uint32_t base = s * perSlice;
      const int columns = static_cast<int>(oneDim ? wrap * 2 : std::min<uint32_t>(cols, 63) + 1);
      // Tall arrays scroll inside their table, with the headers held still.
      const float height = rowHeight * static_cast<float>(std::min<uint32_t>(tableRows, 12) + (oneDim ? 0 : 1)) + 4;
      ImGui::SetCursorScreenPos(ImVec2(x0 + 22, ImGui::GetCursorScreenPos().y + 2));
      ImGui::PushID(static_cast<int>(s));
      const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
      if (ImGui::BeginTable(arr.name.c_str(), columns, flags, ImVec2(tableWidth, height))) {
        for (int c = 0; c < columns; c++) {
          const bool head = oneDim ? (c % 2) == 0 : c == 0;
          ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, head ? headWidth : valueWidth);
        }
        if (!oneDim) {
          ImGui::TableSetupScrollFreeze(1, 1);
          ImGui::TableNextRow(ImGuiTableRowFlags_Headers, rowHeight);
          for (int c = 0; c < columns; c++) {
            ImGui::TableSetColumnIndex(c);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, withAlpha(tc, 0.10f));
            header(c == 0 ? "" : std::to_string(c - 1), false);
          }
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(tableRows), rowHeight);
        while (clipper.Step()) {
          for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            if (oneDim) {
              for (uint32_t w = 0; w < wrap; w++) {
                const uint32_t i = static_cast<uint32_t>(r) * wrap + w;
                if (i >= rows) break;
                ImGui::TableSetColumnIndex(static_cast<int>(w * 2));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, withAlpha(tc, 0.10f));
                header(std::to_string(i), true);
                ImGui::TableSetColumnIndex(static_cast<int>(w * 2 + 1));
                cell(i);
              }
              continue;
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, withAlpha(tc, 0.10f));
            header(std::to_string(r), true);
            for (int c = 1; c < columns; c++) {
              ImGui::TableSetColumnIndex(c);
              cell(base + static_cast<uint32_t>(r) + static_cast<uint32_t>(c - 1) * rows);
            }
          }
        }
        ImGui::EndTable();
      }
      ImGui::PopID();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    if (slices > shownSlices || cols > 63) {
      ImGui::SetCursorScreenPos(ImVec2(x0 + 22, ImGui::GetCursorScreenPos().y + 2));
      if (cols > 63) ImGui::TextDisabled("Columns past 62 not shown");
      else ImGui::TextDisabled("%u more tables not shown", slices - shownSlices);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + 6));
    ImGui::Dummy(ImVec2(0, 0));
  }
  ImGui::Dummy(ImVec2(0, 4));
  ImGui::EndChild();
  // An item where the card ends, which is what moves the layout on.
  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
  ImGui::Dummy(ImVec2(width, 0));
}

void BasicWindow::drawBreakpoints(float width, float height) {
  const Palette &p = palette();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  card(draw, start, ImVec2(start.x + width, start.y + height));
  char head[48];
  std::snprintf(head, sizeof head, "BREAKPOINTS  %zu", breakpoints_.size());
  caption(draw, ImVec2(start.x + PAD, start.y + 10), head);

  // The form: a line (or line:statement), or a rule.
  ImGui::SetCursorScreenPos(ImVec2(start.x + PAD, start.y + 28));
  ImGui::SetNextItemWidth(width - PAD * 2 - 120);
  if (newLineBad_) ImGui::PushStyleColor(ImGuiCol_FrameBg, withAlpha(p.red, 0.18f));
  const bool enter = ImGui::InputTextWithHint("##newline", "Line, or line:statement", newLine_, sizeof newLine_,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  if (newLineBad_) ImGui::PopStyleColor();
  if (ImGui::IsItemEdited()) newLineBad_ = false;
  ImGui::SameLine(0, 4);
  const bool add = ui::Button("+", ImVec2(28, 0));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a breakpoint on the line");
  if (enter || add) {
    int line = -1, stmt = -1;
    const int n = std::sscanf(newLine_, "%d:%d", &line, &stmt);
    if (n >= 1 && line >= 0 && line <= 63999 && (n == 1 || stmt >= 0)) {
      if (!breakpointAt(line, n == 2 ? stmt : -1)) toggleBreakpoint(line, n == 2 ? stmt : -1);
      newLine_[0] = 0;
    } else {
      newLineBad_ = true;
    }
  }
  ImGui::SameLine(0, 4);
  if (ui::Button("if\xE2\x80\xA6", ImVec2(44, 0))) {
    ruleTarget_ = -1;
    rules_.open("A condition rule: stop wherever this becomes true", "", true);
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a rule that stops the program wherever it becomes true");
  ImGui::SameLine(0, 6);
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Click a line's gutter to toggle a breakpoint on it.\n"
                      "Option-click a statement to toggle one on that statement.\n"
                      "A condition (if\xE2\x80\xA6) makes it stop only when the condition holds:\n"
                      "BV(73,0)==50, PEEK($00)==$42, A==3");
  }

  ImGui::SetCursorScreenPos(ImVec2(start.x + 6, ImGui::GetCursorScreenPos().y + 4));
  ImGui::BeginChild("##bplist", ImVec2(width - 12, start.y + height - ImGui::GetCursorScreenPos().y - 6), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  if (breakpoints_.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - 30);
    ImGui::TextDisabled(" Click a line's number in the gutter to stop there, or add one above.");
    ImGui::PopTextWrapPos();
  }
  int removeAt = -1;
  const double now = ImGui::GetTime();
  for (size_t i = 0; i < breakpoints_.size(); i++) {
    Breakpoint &b = breakpoints_[i];
    ImGui::PushID(static_cast<int>(i));
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    const float rowH = ImGui::GetFrameHeight() + 4;
    ImDrawList *d = ImGui::GetWindowDrawList();
    // Flashing while it is the one stopped on.
    if (pulse_ == static_cast<int>(i)) {
      const float t = static_cast<float>(std::fmod(now - pulseAt_, PULSE) / PULSE);
      d->AddRectFilled(r0, ImVec2(r0.x + width - 14, r0.y + rowH), withAlpha(p.red, 0.10f + 0.18f * (1 - t)), 6.0f);
    } else if (b.isRule()) {
      d->AddRectFilled(r0, ImVec2(r0.x + width - 14, r0.y + rowH), withAlpha(p.orange, 0.07f), 6.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(r0.x + 4, r0.y + 2));
    if (ui::Checkbox("##on", &b.enabled)) applyBreakpoints();
    ImGui::SameLine(0, 6);
    ImGui::AlignTextToFramePadding();
    std::string label;
    if (b.isRule()) {
      const auto tree = fromExpression(b.condition);
      // The whole tree as it reads, nested groups bracketed, as the browser's
      // toDisplayLabel; a condition the builder cannot read is shown as typed.
      label = tree ? describe(*tree) : b.condition;
      if (label.empty()) label = "Rule";
    } else {
      label = "Line " + std::to_string(b.line) + (b.statement >= 0 ? " : " + std::to_string(b.statement) : "");
    }
    const float labelX = ImGui::GetCursorScreenPos().x;
    if (b.isRule()) {
      pill(d, ImVec2(labelX, r0.y + 5), "if", true, p.orange);
      ImGui::SetCursorScreenPos(ImVec2(labelX + 26, ImGui::GetCursorScreenPos().y));
    }
    ImGui::PushFont(ui::monoFont(), 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, b.enabled ? text() : text(0.45f));
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (ImGui::IsItemHovered() && !b.condition.empty()) ImGui::SetTooltip("%s", b.condition.c_str());
    if (!b.isRule() && !b.condition.empty()) {
      ImGui::SameLine(0, 6);
      const ImVec2 ca = ImGui::GetCursorScreenPos();
      pill(d, ImVec2(ca.x, r0.y + 5), "if", true, p.orange);
      ImGui::Dummy(ImVec2(22, 1));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", b.condition.c_str());
    }
    if (b.hits) {
      ImGui::SameLine(0, 8);
      ImGui::TextDisabled("\xC3\x97%u", b.hits);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stopped here %u times", b.hits);
    }
    // At the right: the condition and the way out.
    ImGui::SameLine(width - 92);
    if (ui::Button("if\xE2\x80\xA6", ImVec2(44, 0))) {
      ruleTarget_ = static_cast<int>(i);
      rules_.open(b.isRule() ? "Condition rule" : "Condition for " + label, b.condition, true);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit the condition");
    ImGui::SameLine(0, 6);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
    if (removeButton("##remove")) removeAt = static_cast<int>(i);
    ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rowH + 2));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
  }
  if (removeAt >= 0) {
    breakpoints_.erase(breakpoints_.begin() + removeAt);
    pulse_ = -2;
    applyBreakpoints();
  }
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
  ImGui::Dummy(ImVec2(width, 0));

  // The rule builder, writing into the breakpoint it was opened for or a new
  // rule.
  const std::optional<std::string> applied = rules_.draw(nullptr, false);
  if (applied && ruleTarget_ > -2) {
    if (ruleTarget_ == -1) {
      if (!applied->empty()) {
        Breakpoint r;
        r.line = -1;
        int id = 0;
        for (const Breakpoint &x : breakpoints_) {
          if (x.isRule()) id = std::max(id, x.statement + 1);
        }
        r.statement = id;
        r.condition = *applied;
        breakpoints_.insert(breakpoints_.begin(), r);
      }
    } else if (ruleTarget_ < static_cast<int>(breakpoints_.size())) {
      breakpoints_[static_cast<size_t>(ruleTarget_)].condition = *applied;
    }
    ruleTarget_ = -2;
    applyBreakpoints();
  }
}

void BasicWindow::drawSidebar(float width, float height) {
  ImGui::BeginGroup();
  const float bpHeight = std::clamp(breakpointsHeight_, 120.0f, std::max(120.0f, height - 140.0f));
  const float varsHeight = height - bpHeight - 10;
  drawVariables(width, varsHeight);
  // The splitter between the two cards.
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##hsplit", ImVec2(width, 10));
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x + width * 0.5f - 18, at.y + 3.5f),
                                              ImVec2(at.x + width * 0.5f + 18, at.y + 6.5f), text(0.25f), 1.5f);
  }
  if (ImGui::IsItemActive()) {
    breakpointsHeight_ = std::clamp(breakpointsHeight_ - ImGui::GetIO().MouseDelta.y, 120.0f, height - 140.0f);
    ImGui::MarkIniSettingsDirty();
  }
  drawBreakpoints(width, bpHeight);
  ImGui::EndGroup();
}

void BasicWindow::drawStatusBar() {
  const CodeEditor::Position c = editor_.caret();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.9f);
  if (state_.running && state_.paused && (state_.curlin >> 8) != 0xFF) {
    ImGui::TextDisabled("LINE %u", state_.curlin);
    if (state_.statements > 1) {
      ImGui::SameLine(0, 6);
      ImGui::TextDisabled("[%d/%d]", state_.statement + 1, state_.statements);
    }
    ImGui::SameLine(0, 14);
    ImGui::TextDisabled("PTR $%04X", state_.txtptr);
  } else {
    ImGui::TextDisabled("LINE ---   PTR $----");
  }
  ImGui::SameLine(0, 18);
  ImGui::TextDisabled("Ln %d, Col %d", c.line + 1, c.column + 1);
  ImGui::SameLine(0, 18);
  const int lineCount = countLines(editor_.lines());
  const size_t charCount = countChars(editor_.lines());
  ImGui::TextDisabled("%d line%s", lineCount, lineCount == 1 ? "" : "s");
  ImGui::SameLine(0, 12);
  ImGui::TextDisabled("%zu char%s", charCount, charCount == 1 ? "" : "s");
  ImGui::PopFont();
  const double age = ImGui::GetTime() - messageAt_;
  if (!message_.empty() && age < 4.0) {
    const float w = ImGui::CalcTextSize(message_.c_str()).x;
    ImGui::SameLine(0, 18);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
    const ImU32 c2 = messageProblem_ ? palette().red : ui::accentText(std::min(1.0f, static_cast<float>(4.0 - age)));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(c2), "%s", message_.c_str());
  }
}

void BasicWindow::drawNewConfirm() {
  if (confirmNew_) {
    ImGui::OpenPopup("##basicnew");
    confirmNew_ = false;
  }
  if (!ImGui::BeginPopupModal("##basicnew", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextUnformatted("Clear the editor and start a new program?");
  ImGui::TextDisabled("The program in memory is not touched.");
  ImGui::Spacing();
  if (ui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  ImGui::SameLine(0, 6);
  if (ui::Button("New", ImVec2(90, 0), ui::ButtonKind::Primary)) {
    editor_.setText("");
    filePath_.clear();
    syncedText_.reset();
    error_.reset();
    formattedRevision_ = editor_.revision();
    ImGui::MarkIniSettingsDirty();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void BasicWindow::draw(bool *open) {
  if (!open || !*open || !available_) return;
  ui::BeforeWindow("Applesoft BASIC");
  ImGui::SetNextWindowSize(ImVec2(1040, 720), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(EDITOR_MIN + SIDEBAR_MIN + 60, 460), ImVec2(FLT_MAX, FLT_MAX));
  if (!ui::BeginWindow("Applesoft BASIC", open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::End();
    return;
  }

  // The window's own keys: Run, Stop, and a breakpoint on the caret's line.
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_R, ImGuiInputFlags_RouteFocused)) run();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Period, ImGuiInputFlags_RouteFocused)) stop();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Backslash, ImGuiInputFlags_RouteFocused)) {
    if (const auto n = basic::lineNumber(editor_.lines()[static_cast<size_t>(editor_.caret().line)])) toggleBreakpoint(*n, -1);
  }

  drawToolbar();
  ImGui::Dummy(ImVec2(0, 4));

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  // The status line's own height, and the gap above it.
  const float statusHeight = ImGui::GetTextLineHeightWithSpacing() + 8;
  const float height = avail.y - statusHeight;
  const float sidebar = std::clamp(sidebarWidth_, SIDEBAR_MIN, std::max(SIDEBAR_MIN, avail.x - EDITOR_MIN - 10));
  const float editorWidth = avail.x - sidebar - 10;
  drawEditor(ImVec2(editorWidth, height));
  ImGui::SameLine(0, 0);
  // The splitter between the editor and the sidebar.
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##vsplit", ImVec2(10, height));
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x + 3.5f, at.y + height * 0.5f - 18),
                                              ImVec2(at.x + 6.5f, at.y + height * 0.5f + 18), text(0.25f), 1.5f);
  }
  if (ImGui::IsItemActive()) {
    sidebarWidth_ = std::clamp(sidebarWidth_ - ImGui::GetIO().MouseDelta.x, SIDEBAR_MIN, avail.x - EDITOR_MIN - 10);
    ImGui::MarkIniSettingsDirty();
  }
  ImGui::SameLine(0, 0);
  drawSidebar(sidebar, height);
  ImGui::Dummy(ImVec2(0, 2));
  drawStatusBar();

  drawNewConfirm();
  ImGui::End();
  drawCompletions();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void BasicWindow::writeSettings(std::string &out) const {
  char line[96];
  std::snprintf(line, sizeof line, "Trace=%d\nHeat=%d\nSidebar=%.0f\nBreakpointsHeight=%.0f\n", trace_ ? 1 : 0, heat_ ? 1 : 0,
                sidebarWidth_, breakpointsHeight_);
  out += line;
  if (!filePath_.empty()) out += "File=" + filePath_ + "\n";
  for (const Breakpoint &b : breakpoints_) {
    std::snprintf(line, sizeof line, "Breakpoint=%d\t%d\t%d\t", b.line, b.statement, b.enabled ? 1 : 0);
    out += line + b.condition + "\n";
  }
  // The program last, a line of the ini to a line of it.
  for (const std::string &l : haveText_ ? editor_.lines() : pendingText_) out += "Text=" + l + "\n";
}

void BasicWindow::readSetting(const char *line) {
  int value = 0;
  float number = 0;
  if (std::sscanf(line, "Trace=%d", &value) == 1) trace_ = value;
  else if (std::sscanf(line, "Heat=%d", &value) == 1) heat_ = value;
  else if (std::sscanf(line, "Sidebar=%f", &number) == 1) sidebarWidth_ = number;
  else if (std::sscanf(line, "BreakpointsHeight=%f", &number) == 1) breakpointsHeight_ = number;
  else if (!std::strncmp(line, "File=", 5)) filePath_ = line + 5;
  else if (!std::strncmp(line, "Text=", 5)) pendingText_.emplace_back(line + 5);
  else if (!std::strncmp(line, "Breakpoint=", 11)) {
    Breakpoint b;
    int enabled = 1, consumed = 0;
    if (std::sscanf(line + 11, "%d\t%d\t%d\t%n", &b.line, &b.statement, &enabled, &consumed) >= 3) {
      b.enabled = enabled;
      if (consumed > 0) b.condition = line + 11 + consumed;
      breakpoints_.push_back(b);
    }
  }
}

} // namespace a2e::native
