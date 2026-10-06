/*
 * develop.cpp - Building the user's program and running it here
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "develop/develop.hpp"

#include "app/emulation.hpp"
#include "drives/media_store.hpp"
#include "ui/ui_controls.hpp"
#include "ui/ui_theme.hpp"

#include "machine/machine_profile.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace a2e::native {

namespace {

// How long a machine just switched on is left to start itself before a
// program is put in it: long enough for the firmware to set the screen and
// the keyboard up, and on a IIgs to finish its own start.
constexpr double SETTLE_SECONDS = 0.5;
constexpr double SETTLE_SECONDS_IIGS = 3.0;
// How often the watched output is looked at, and how long it has to have
// stopped changing before it is run.
constexpr double POLL_SECONDS = 0.4;
constexpr double SETTLED_SECONDS = 0.4;

std::string shellQuoted(const std::string &text) {
  std::string out = "'";
  for (char c : text) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

std::string readText(const std::string &path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

} // namespace

Develop::Develop(Emulation &emulation, Platform &platform, Hooks hooks)
    : emulation_(emulation), platform_(platform), hooks_(std::move(hooks)) {}

Develop::~Develop() {
  if (build_.valid()) build_.wait();
}

void Develop::report(const std::string &status, bool problem) {
  status_ = status;
  problem_ = problem;
  if (problem) {
    state_ = State::Failed;
    wantsWindow_ = true;
  }
}

void Develop::openProject(const std::string &path) {
  projectPath_ = path;
  project_.reset();
  issues_.clear();
  output_.clear();
  ranOutput_.reset();
  seenOutput_.reset();
  std::string error;
  const std::string json = readText(path);
  if (json.empty()) {
    report("Could not read " + path + ".", true);
    return;
  }
  project_ = dev::parseProject(json, path, error);
  if (!project_) {
    report(error, true);
    return;
  }
  // What is there already counts as run, so watching starts from the next
  // build rather than running the old one the moment the project opens.
  ranOutput_ = modified(project_->output);
  lastRun_.reset();
  state_ = State::Idle;
  report("", false);
}

void Develop::closeProject() {
  project_.reset();
  lastRun_.reset();
  state_ = State::Idle;
  projectPath_.clear();
  issues_.clear();
  output_.clear();
  report("", false);
}

void Develop::chooseProject() {
  if (!platform_.openFile) return;
  platform_.openFile("Open an ApplEm project", {"applem"}, [this](const std::string &path) {
    if (!path.empty()) openProject(path);
  });
}

std::optional<fs::file_time_type> Develop::modified(const std::string &path) const {
  std::error_code error;
  const auto time = fs::last_write_time(path, error);
  if (error) return std::nullopt;
  return time;
}

// The build runs in the user's login shell, so it finds what their terminal
// finds (cl65 from Homebrew, say), with Homebrew's folders added in case
// their profile sets PATH somewhere a login shell does not read.
void Develop::buildAndRun() {
  if (!project_ || building()) return;
  if (project_->build.empty()) {
    run();
    return;
  }
  issues_.clear();
  output_.clear();
  report("Building…", false);
  state_ = State::Building;
  buildStarted_ = now_;
  const char *shell = std::getenv("SHELL");
  const std::string script = "export PATH=\"/opt/homebrew/bin:/usr/local/bin:$PATH\"; cd " +
                             shellQuoted(project_->directory) + " && " + project_->build;
  const std::string command =
      shellQuoted(shell && *shell ? shell : "/bin/zsh") + " -l -c " + shellQuoted(script) + " 2>&1";
  build_ = std::async(std::launch::async, [command] {
    BuildResult result;
    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe) return result;
    std::array<char, 4096> chunk{};
    size_t read = 0;
    while ((read = fread(chunk.data(), 1, chunk.size(), pipe)) > 0) result.output.append(chunk.data(), read);
    const int status = pclose(pipe);
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
  });
}

void Develop::runAgain() {
  if (project_ && !building()) run();
}

// What the build made, read and started on the machine the project names.
void Develop::run() {
  if (!project_) return;
  const auto file = readFile(project_->output);
  if (!file || file->empty()) {
    report("There is nothing at " + project_->output + " to run. Has it been built?", true);
    return;
  }
  ranOutput_ = modified(project_->output);
  const std::string dbg = project_->symbols.empty() ? std::string() : readText(project_->symbols);
  std::string error;
  const auto program = dev::readProgram(*file, *project_, dbg, error);
  if (!program) {
    report(error, true);
    return;
  }
  if (!project_->machine.empty() && hooks_.machineKey && hooks_.machineKey() != project_->machine) {
    if (!findMachineProfile(project_->machine.c_str())) {
      report("The project names a machine ApplEm does not have: \"" + project_->machine + "\".", true);
      return;
    }
    if (!hooks_.useMachine || !hooks_.useMachine(project_->machine)) {
      report("Could not switch to the " + project_->machine + ".", true);
      return;
    }
  }
  const int symbols = !dbg.empty() && hooks_.importSymbols ? hooks_.importSymbols(dbg) : -1;

  const bool disk = project_->start == dev::Project::Start::Disk ||
                    (project_->start == dev::Project::Start::Auto && program->wantsProDOS());
  LastRun run;
  run.bytes = program->bytes.size();
  run.load = program->load;
  run.entry = program->entry;
  run.fromDisk = disk;
  run.smartPort = disk && hooks_.hasSmartPort && hooks_.hasSmartPort();
  run.symbols = symbols;
  run.at = now_;
  lastRun_ = run;
  if (disk) {
    startFromDisk(*program);
    return;
  }
  // A machine that is off is switched on and left to start itself first: the
  // reset that enters the program needs the firmware to have run once.
  if (!emulation_.powered()) {
    emulation_.setPowered(true);
    pendingStart_ = program;
    const bool iigs = hooks_.machineKey && hooks_.machineKey() == "apple2gs";
    startAt_ = now_ + (iigs ? SETTLE_SECONDS_IIGS : SETTLE_SECONDS);
    report("Starting the machine…", false);
    return;
  }
  startFromMemory(*program);
}

void Develop::startFromMemory(const dev::Program &program) {
  const bool ok = emulation_.withMachine([&](host::MachineHost &host) {
    return host.startProgram(program.bytes.data(), program.bytes.size(), program.load, program.entry);
  });
  char text[160];
  if (ok) {
    std::snprintf(text, sizeof text, "Running %s: %zu bytes at $%04X, from $%04X.", project_->name.c_str(),
                  program.bytes.size(), program.load, program.entry);
    state_ = State::Ran;
  } else {
    std::snprintf(text, sizeof text, "%s does not fit below $C000.", project_->name.c_str());
  }
  report(text, !ok);
}

// A ProDOS volume made for the program, in the project's own hidden folder so
// what the machine writes to it stays with the project and is never the
// user's system disk.
void Develop::startFromDisk(const dev::Program &program) {
  const std::string systemPath =
      project_->disk.empty() ? platform_.resourceDirectory + "/disks/ProDOS 2.4.3.po" : project_->disk;
  const auto system = readFile(systemPath);
  if (!system) {
    report("Could not read the ProDOS system disk " + systemPath + ".", true);
    return;
  }
  const bool smartPort = hooks_.hasSmartPort && hooks_.hasSmartPort();
  std::string error;
  const auto volume = dev::makeBootVolume(*system, program, project_->fileName, smartPort ? 1600 : 280, error);
  if (!volume) {
    report(error, true);
    return;
  }
  const fs::path folder = fs::path(project_->directory) / ".applem";
  std::error_code ignored;
  fs::create_directories(folder, ignored);
  const std::string path = (folder / (smartPort ? "boot-hd.po" : "boot.po")).string();
  if (!writeFile(path, volume->data(), volume->size())) {
    report("Could not write the boot volume to " + path + ".", true);
    return;
  }
  if (smartPort) {
    if (hooks_.insertHardDrive) hooks_.insertHardDrive(path);
  } else if (hooks_.insertFloppy) {
    hooks_.insertFloppy(path);
  }
  if (!emulation_.powered()) emulation_.setPowered(true);
  emulation_.withMachine([](host::MachineHost &host) {
    host.setPaused(false);
    host.reset();
  });
  report("Starting " + project_->fileName + " from ProDOS, on " + std::string(smartPort ? "the SmartPort" : "drive 1") +
             ".",
         false);
  state_ = State::Ran;
}

void Develop::update(double now) {
  now_ = now;
  if (build_.valid() && build_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const BuildResult result = build_.get();
    output_ = result.output;
    lastBuildSeconds_ = now - buildStarted_;
    issues_ = project_ ? dev::parseIssues(output_, project_->directory) : std::vector<dev::Issue>{};
    if (result.status == 0) {
      run();
    } else {
      int errors = 0;
      for (const dev::Issue &issue : issues_) errors += issue.warning ? 0 : 1;
      report(errors > 0 ? "The build failed with " + std::to_string(errors) + (errors == 1 ? " error." : " errors.")
                        : "The build failed (exit status " + std::to_string(result.status) + ").",
             true);
    }
  }

  if (pendingStart_ && now >= startAt_) {
    const dev::Program program = std::move(*pendingStart_);
    pendingStart_.reset();
    startFromMemory(program);
  }

  // A build made anywhere: run once the output has stopped changing.
  if (!watch || !project_ || building() || now - lastPoll_ < POLL_SECONDS) return;
  lastPoll_ = now;
  const auto stamp = modified(project_->output);
  if (!stamp || stamp == ranOutput_) {
    seenOutput_.reset();
    return;
  }
  if (stamp != seenOutput_) {
    seenOutput_ = stamp;
    seenAt_ = now;
    return;
  }
  if (now - seenAt_ >= SETTLED_SECONDS) {
    seenOutput_.reset();
    issues_.clear();
    output_.clear();
    run();
  }
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

namespace {

constexpr float ROUNDING = 10.0f;
constexpr float PAD = 14.0f;
// The build output's well: the Disk Inspector's dark pane, so the two
// windows that show raw machine data look alike.
constexpr ImU32 WELL = IM_COL32(0x14, 0x10, 0x0a, 255);
constexpr ImU32 WELL_TEXT = IM_COL32(0xe0, 0xdc, 0xd0, 255);
constexpr ImU32 WELL_DIM = IM_COL32(0xe0, 0xdc, 0xd0, 110);
// The logo's colours as they read on the well, which is dark in either
// appearance: the theme's own are darkened for a light window.
constexpr ImU32 WELL_RED = IM_COL32(0xf0, 0x6e, 0x70, 255);
constexpr ImU32 WELL_ORANGE = IM_COL32(0xf7, 0xa8, 0x5c, 255);
constexpr ImU32 WELL_GREEN = IM_COL32(0x8c, 0xd4, 0x72, 255);
constexpr ImU32 WELL_BLUE = IM_COL32(0x5c, 0xc0, 0xf2, 255);

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }
ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

void card(ImDrawList *draw, ImVec2 a, ImVec2 b, bool hovered = false) {
  const bool dark = ui::isDark();
  draw->AddRectFilled(a, b, dark ? IM_COL32(255, 255, 255, hovered ? 20 : 10) : IM_COL32(0, 0, 0, hovered ? 14 : 7),
                      ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), ROUNDING);
}

// A capsule with a word in it. Returns its width.
float chip(ImDrawList *draw, ImVec2 at, const std::string &label, ImU32 colour) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 size = ImGui::CalcTextSize(label.c_str());
  const float h = size.y + 6;
  draw->AddRectFilled(at, ImVec2(at.x + size.x + 16, at.y + h), withAlpha(colour, 0.16f), h * 0.5f);
  draw->AddText(ImVec2(at.x + 8, at.y + 3), colour, label.c_str());
  ImGui::PopFont();
  return size.x + 16;
}

void caption(ImDrawList *draw, ImVec2 at, const char *label) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(at, secondary(), label);
  ImGui::PopFont();
}

// The middle of a long path given up for an ellipsis, so its start and its
// project folder both show.
std::string fitPath(const std::string &path, float width) {
  if (ImGui::CalcTextSize(path.c_str()).x <= width) return path;
  std::string head = path.substr(0, path.size() / 2);
  std::string tail = path.substr(path.size() / 2);
  while (!head.empty() && !tail.empty() && ImGui::CalcTextSize((head + "…" + tail).c_str()).x > width) {
    if (head.size() > tail.size()) head.pop_back();
    else tail.erase(0, 1);
  }
  return head + "…" + tail;
}

std::string ago(double seconds) {
  if (seconds < 5) return "just now";
  if (seconds < 60) return std::to_string(static_cast<int>(seconds)) + "s ago";
  if (seconds < 3600) return std::to_string(static_cast<int>(seconds / 60)) + "m ago";
  return std::to_string(static_cast<int>(seconds / 3600)) + "h ago";
}

} // namespace

void Develop::draw(bool *open) {
  if (!open || !*open) return;
  ImGui::SetNextWindowSize(ImVec2(660, 560), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(520, 300), ImVec2(FLT_MAX, FLT_MAX));
  ui::BeforeWindow("Build");
  if (!ui::BeginWindow("Build", open)) {
    ImGui::End();
    return;
  }
  const float width = ImGui::GetContentRegionAvail().x;
  if (!project_) {
    drawEmpty();
  } else {
    drawHeader(width);
    ImGui::Dummy(ImVec2(0, 4));
    drawActions(width);
    ImGui::Dummy(ImVec2(0, 6));
    drawSummary(width);
    drawIssues(width);
    drawOutput();
  }
  ImGui::End();
}

// No project: what one is, a sample to start from, and the way to open one.
void Develop::drawEmpty() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 at = ImGui::GetCursorScreenPos();

  // A pair of braces in a disc of the accent colour: a project file.
  const ImVec2 icon(at.x + width * 0.5f, at.y + 40);
  draw->AddCircleFilled(icon, 30, withAlpha(accent(), 0.14f), 48);
  draw->AddText(nullptr, 30.0f, ImVec2(icon.x - 15, icon.y - 17), accent(), "{ }");
  ImGui::Dummy(ImVec2(width, 84));

  auto centred = [&](const char *line, ImU32 colour, float scale) {
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * scale);
    const float w = ImGui::CalcTextSize(line).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - w) * 0.5f));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colour), "%s", line);
    ImGui::PopFont();
  };
  centred("Build and run your own program", text(), 1.3f);
  centred("Your editor, your Makefile and cl65; ApplEm runs what they make.", secondary(), 1.0f);
  ImGui::Dummy(ImVec2(0, 10));

  // The sample project file, in the output's dark well, coloured as JSON.
  const float wellWidth = std::min(width, 440.0f);
  const float line = ImGui::GetTextLineHeight() + 2;
  const char *keys[] = {"build", "output", "symbols", "machine"};
  const char *values[] = {"make", "build/game", "build/game.dbg", "apple2e"};
  const float wellHeight = line * 6 + PAD * 2;
  const ImVec2 w0(ImGui::GetCursorScreenPos().x + (width - wellWidth) * 0.5f, ImGui::GetCursorScreenPos().y);
  draw->AddRectFilled(w0, ImVec2(w0.x + wellWidth, w0.y + wellHeight), WELL, ROUNDING);
  ImGui::PushFont(ui::monoFont(), 0.0f);
  float y = w0.y + PAD;
  const float x = w0.x + PAD;
  draw->AddText(ImVec2(x, y), WELL_DIM, "{");
  for (int i = 0; i < 4; i++) {
    y += line;
    const std::string key = std::string("  \"") + keys[i] + "\":";
    draw->AddText(ImVec2(x, y), WELL_BLUE, key.c_str());
    const float vx = x + ImGui::CalcTextSize("  \"symbols\": ").x;
    const std::string value = std::string("\"") + values[i] + "\"" + (i < 3 ? "," : "");
    draw->AddText(ImVec2(vx, y), WELL_GREEN, value.c_str());
  }
  draw->AddText(ImVec2(x, y + line), WELL_DIM, "}");
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(width, wellHeight + 6));
  centred("Save it as game.applem beside your Makefile.", secondary(), ui::SMALL_TEXT);
  ImGui::Dummy(ImVec2(0, 8));

  const char *label = "Open Project…";
  const float bw = ImGui::CalcTextSize(label).x + 40;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - bw) * 0.5f);
  if (ui::Button(label, ImVec2(bw, 0), ui::ButtonKind::Primary)) chooseProject();
  if (!status_.empty()) {
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(problem_ ? p.red : secondary()), "%s", status_.c_str());
    ImGui::PopTextWrapPos();
  }
}

// The project: its monogram, its name and folder, and how it runs.
void Develop::drawHeader(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float height = 66;
  const ImVec2 b(a.x + width, a.y + height);
  card(draw, a, b);

  // The monogram: the project's first letter on the accent, as a document
  // icon would carry it.
  const ImVec2 m0(a.x + PAD, a.y + 13);
  const ImVec2 m1(m0.x + 40, m0.y + 40);
  draw->AddRectFilled(m0, m1, accent(), 9.0f);
  const std::string letter(1, static_cast<char>(std::toupper(static_cast<unsigned char>(
                                 project_->name.empty() ? 'A' : project_->name[0]))));
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.6f);
  const ImVec2 ls = ImGui::CalcTextSize(letter.c_str());
  draw->AddText(ImVec2((m0.x + m1.x - ls.x) * 0.5f, (m0.y + m1.y - ls.y) * 0.5f), ui::textOn(accent()),
                letter.c_str());
  ImGui::PopFont();

  // The chips on the right: the machine, and how it is started.
  float right = b.x - PAD;
  std::vector<std::pair<std::string, ImU32>> chips;
  if (!project_->machine.empty()) {
    const MachineProfile *machine = findMachineProfile(project_->machine.c_str());
    chips.push_back({machine ? machine->name : project_->machine, p.blue});
  }
  const char *start = project_->start == dev::Project::Start::Memory ? "From memory"
                      : project_->start == dev::Project::Start::Disk ? "From ProDOS"
                                                                     : "Auto start";
  chips.push_back({start, p.purple});
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  for (auto it = chips.rbegin(); it != chips.rend(); ++it) right -= ImGui::CalcTextSize(it->first.c_str()).x + 16 + 6;
  ImGui::PopFont();
  float cx = right + 6;
  for (const auto &[label, colour] : chips) cx += chip(draw, ImVec2(cx, a.y + 14), label, colour) + 6;

  // The name, and the folder under it.
  const float tx = m1.x + 12;
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.3f);
  draw->AddText(ImVec2(tx, a.y + 12), text(), project_->name.c_str());
  ImGui::PopFont();
  std::string folder = project_->directory;
  if (const char *home = std::getenv("HOME"); home && *home && folder.rfind(home, 0) == 0) {
    folder = "~" + folder.substr(std::strlen(home));
  }
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  folder = fitPath(folder, std::max(60.0f, right - tx - 10));
  draw->AddText(ImVec2(tx, a.y + 38), secondary(), folder.c_str());
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(width, height));
}

// Build and Run, Run Again and the watch, and a pill saying what is going on.
void Develop::drawActions(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  ImGui::BeginDisabled(building());
  if (ui::Button(project_->build.empty() ? "Run   ⌘B" : "Build and Run   ⌘B", ImVec2(0, 0),
                 ui::ButtonKind::Primary)) {
    buildAndRun();
  }
  ImGui::SameLine();
  if (ui::Button("Run Again")) runAgain();
  ImGui::EndDisabled();
  ImGui::SameLine(0, 16);
  ui::Switch("Watch", &watch);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Run %s whenever a build writes it, whatever built it",
                      fs::path(project_->output).filename().string().c_str());
  }

  // The pill, right-aligned on the same row.
  std::string label;
  ImU32 colour = secondary();
  switch (state_) {
  case State::Idle: label = watch ? "Watching" : "Ready"; colour = watch ? p.blue : secondary(); break;
  case State::Building: {
    char t[32];
    std::snprintf(t, sizeof t, "Building  %.0fs", now_ - buildStarted_);
    label = t;
    colour = p.yellow;
    break;
  }
  case State::Ran: label = watch ? "Running, watching" : "Running"; colour = p.green; break;
  case State::Failed: label = "Failed"; colour = p.red; break;
  }
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
  ImGui::PopFont();
  const float h = ImGui::GetFrameHeight();
  const float pw = ts.x + 34;
  const ImVec2 row = ImGui::GetItemRectMin();
  const ImVec2 p0(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - pw, row.y);
  if (p0.x > ImGui::GetItemRectMax().x + 12) {
    draw->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + h), withAlpha(colour, 0.14f), h * 0.5f);
    const ImVec2 dot(p0.x + 13, p0.y + h * 0.5f);
    if (state_ == State::Building) {
      // A turning arc, the system's spinner as near as a draw list gets.
      const float angle = static_cast<float>(now_) * 6.0f;
      draw->PathArcTo(dot, 5.0f, angle, angle + 4.4f, 16);
      draw->PathStroke(colour, 0, 2.0f);
    } else {
      draw->AddCircleFilled(dot, 7.0f, withAlpha(colour, 0.25f), 16);
      draw->AddCircleFilled(dot, 4.0f, colour, 16);
    }
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
    draw->AddText(ImVec2(p0.x + 24, p0.y + (h - ts.y) * 0.5f), colour, label.c_str());
    ImGui::PopFont();
  }
  (void)width;

  // What happened, in a line under the buttons.
  if (!status_.empty()) {
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(problem_ ? p.red : secondary()), "%s", status_.c_str());
    ImGui::PopTextWrapPos();
  }
}

// The last run in figures, a tile each.
void Develop::drawSummary(float width) {
  if (!lastRun_) return;
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const LastRun &r = *lastRun_;
  char size[32], load[16], entry[16], symbols[16], built[48];
  std::snprintf(size, sizeof size, r.bytes >= 1024 ? "%.1fK" : "%.0f", r.bytes >= 1024 ? r.bytes / 1024.0 : r.bytes * 1.0);
  std::snprintf(load, sizeof load, "$%04X", r.load);
  std::snprintf(entry, sizeof entry, "$%04X", r.entry);
  std::snprintf(symbols, sizeof symbols, "%d", std::max(0, r.symbols));
  if (lastBuildSeconds_ >= 0) std::snprintf(built, sizeof built, "%.1fs", lastBuildSeconds_);
  else std::snprintf(built, sizeof built, "—");
  struct Tile {
    const char *caption;
    std::string value;
    ImU32 colour;
  };
  const Tile tiles[] = {
      {r.bytes == 1 ? "BYTE" : "BYTES", size, text()},
      {"LOAD", load, p.orange},
      {"ENTRY", entry, p.orange},
      {"STARTED", r.fromDisk ? (r.smartPort ? "SmartPort" : "Drive 1") : "Memory", p.purple},
      {"SYMBOLS", r.symbols < 0 ? "—" : symbols, p.blue},
      {"BUILD", built, text()},
  };
  const int count = static_cast<int>(sizeof tiles / sizeof tiles[0]);
  const float gap = 8;
  const float tw = (width - gap * (count - 1)) / count;
  const float th = 54;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  for (int i = 0; i < count; i++) {
    const ImVec2 a(at.x + i * (tw + gap), at.y);
    const ImVec2 b(a.x + tw, a.y + th);
    card(draw, a, b);
    caption(draw, ImVec2(a.x + 10, a.y + 8), tiles[i].caption);
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.15f);
    draw->AddText(ImVec2(a.x + 10, a.y + 25), tiles[i].colour, tiles[i].value.c_str());
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(width, th));
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(secondary()), "Last run %s", ago(now_ - r.at).c_str());
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, 4));
}

// The build's errors and warnings, a card each: a click opens the file in the
// app that edits it.
void Develop::drawIssues(float width) {
  if (issues_.empty()) return;
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  int errors = 0;
  for (const dev::Issue &issue : issues_) errors += issue.warning ? 0 : 1;
  const int warnings = static_cast<int>(issues_.size()) - errors;
  {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    caption(draw, at, "ISSUES");
    float x = at.x + 56;
    if (errors) x += chip(draw, ImVec2(x, at.y - 3), std::to_string(errors) + (errors == 1 ? " error" : " errors"), p.red) + 6;
    if (warnings) chip(draw, ImVec2(x, at.y - 3), std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings"), p.orange);
    ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight() + 6));
  }
  const float rowHeight = ImGui::GetTextLineHeight() + 16;
  for (size_t i = 0; i < issues_.size(); i++) {
    const dev::Issue &issue = issues_[i];
    const ImU32 colour = issue.warning ? p.orange : p.red;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + width, a.y + rowHeight);
    ImGui::PushID(static_cast<int>(i));
    const bool clicked = ImGui::InvisibleButton("##issue", ImVec2(width, rowHeight));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    card(draw, a, b, hovered);
    draw->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + 4, b.y), colour, ROUNDING, ImDrawFlags_RoundCornersLeft);
    const float mid = a.y + rowHeight * 0.5f;
    draw->AddCircleFilled(ImVec2(a.x + 18, mid), 4.0f, colour, 12);
    const std::string where = fs::path(issue.file).filename().string() + ":" + std::to_string(issue.line);
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
    const ImVec2 ws = ImGui::CalcTextSize(where.c_str());
    draw->AddRectFilled(ImVec2(a.x + 30, mid - ws.y * 0.5f - 3), ImVec2(a.x + 30 + ws.x + 12, mid + ws.y * 0.5f + 3),
                        withAlpha(colour, 0.14f), 5.0f);
    draw->AddText(ImVec2(a.x + 36, mid - ws.y * 0.5f), colour, where.c_str());
    ImGui::PopFont();
    draw->PushClipRect(a, ImVec2(b.x - 30, b.y), true);
    draw->AddText(ImVec2(a.x + 30 + ws.x + 22, mid - ImGui::GetTextLineHeight() * 0.5f), text(), issue.message.c_str());
    draw->PopClipRect();
    if (hovered) {
      draw->AddText(ImVec2(b.x - 24, mid - ImGui::GetTextLineHeight() * 0.5f), secondary(), "↗");
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImGui::SetTooltip("Open %s (line %d)", issue.file.c_str(), issue.line);
    }
    if (clicked && platform_.openPath) platform_.openPath(issue.file);
    ImGui::Dummy(ImVec2(0, 2));
  }
  ImGui::Dummy(ImVec2(0, 4));
}

// The build's output as a terminal shows it, each line coloured by what it
// is: a command dimmed, an error or a warning in its colour.
void Develop::drawOutput() {
  if (output_.empty()) return;
  if (!ui::Disclosure("Build Output", issues_.empty())) return;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, WELL);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ROUNDING);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PAD, 10));
  // As tall as what it holds, up to the room left: a line of output in a
  // well that filled the window was mostly an empty black slab.
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.92f);
  const int lineCount = static_cast<int>(std::count(output_.begin(), output_.end(), '\n')) + 1;
  const float wanted = lineCount * ImGui::GetTextLineHeightWithSpacing() + 20 + ImGui::GetStyle().ScrollbarSize;
  ImGui::PopFont();
  const float height = std::min(wanted, std::max(ImGui::GetContentRegionAvail().y, 80.0f));
  ImGui::BeginChild("##output", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.92f);
  std::istringstream lines(output_);
  for (std::string line; std::getline(lines, line);) {
    ImU32 colour = WELL_TEXT;
    std::string lower = line;
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("error") != std::string::npos) colour = WELL_RED;
    else if (lower.find("warning") != std::string::npos) colour = WELL_ORANGE;
    else if (lower.rfind("cl65", 0) == 0 || lower.rfind("ca65", 0) == 0 || lower.rfind("ld65", 0) == 0 ||
             lower.rfind("cc65", 0) == 0 || lower.rfind("make", 0) == 0 || lower.rfind("mkdir", 0) == 0) {
      colour = WELL_DIM;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

} // namespace a2e::native
