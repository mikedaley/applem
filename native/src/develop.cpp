/*
 * develop.cpp - Building the user's program and running it here
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "develop.hpp"

#include "emulation.hpp"
#include "media_store.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "machine/machine_profile.hpp"

#include "imgui.h"

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
  if (problem) wantsWindow_ = true;
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
  report("Opened " + project_->name + ".", false);
}

void Develop::closeProject() {
  project_.reset();
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
  if (!dbg.empty() && hooks_.importSymbols) hooks_.importSymbols(dbg);

  const bool disk = project_->start == dev::Project::Start::Disk ||
                    (project_->start == dev::Project::Start::Auto && program->wantsProDOS());
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
}

void Develop::update(double now) {
  now_ = now;
  if (build_.valid() && build_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const BuildResult result = build_.get();
    output_ = result.output;
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

void Develop::draw(bool *open) {
  if (!open || !*open) return;
  ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
  ui::BeforeWindow("Build");
  if (!ui::BeginWindow("Build", open)) {
    ImGui::End();
    return;
  }
  const ui::Palette &p = ui::palette();
  const ImVec4 secondary = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

  if (!project_) {
    ImGui::TextWrapped("Open a project to build and run your program here. A project is a small file "
                       "beside your Makefile, ending .applem:");
    ImGui::Spacing();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    ImGui::TextUnformatted("{\n  \"build\":   \"make\",\n  \"output\":  \"build/game\",\n"
                           "  \"symbols\": \"build/game.dbg\",\n  \"machine\": \"apple2e\"\n}");
    ImGui::PopFont();
    ImGui::Spacing();
    if (ui::Button("Open Project…", ImVec2(0, 0), ui::ButtonKind::Primary)) chooseProject();
  } else {
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.2f);
    ImGui::TextUnformatted(project_->name.c_str());
    ImGui::PopFont();
    // The folder with the home folder as ~, as Finder and a terminal write it.
    std::string folder = project_->directory;
    if (const char *home = std::getenv("HOME"); home && *home && folder.rfind(home, 0) == 0) {
      folder = "~" + folder.substr(std::strlen(home));
    }
    ImGui::TextColored(secondary, "%s", folder.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(building());
    if (ui::Button(project_->build.empty() ? "Run" : "Build and Run", ImVec2(0, 0), ui::ButtonKind::Primary)) {
      buildAndRun();
    }
    ImGui::SameLine();
    if (ui::Button("Run Again")) runAgain();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::Button("Close Project")) closeProject();
    ImGui::SameLine(0, 18);
    ui::Switch("Watch for Changes", &watch);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Run %s as soon as a build writes it, whatever built it",
                        fs::path(project_->output).filename().string().c_str());
    }
  }

  if (!status_.empty()) {
    ImGui::Spacing();
    std::string status = status_;
    if (building()) {
      char elapsed[32];
      std::snprintf(elapsed, sizeof elapsed, "  %.0fs", now_ - buildStarted_);
      status += elapsed;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(problem_ ? p.red : ui::accentText()), "%s", status.c_str());
    ImGui::PopTextWrapPos();
  }

  // The build's errors and warnings: a click opens the file in the app that
  // edits it.
  if (!issues_.empty()) {
    ImGui::Spacing();
    ImGui::SeparatorText("Issues");
    for (size_t i = 0; i < issues_.size(); i++) {
      const dev::Issue &issue = issues_[i];
      ImGui::PushID(static_cast<int>(i));
      const std::string where = fs::path(issue.file).filename().string() + ":" + std::to_string(issue.line);
      const std::string label = where + "  " + issue.message;
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(issue.warning ? p.orange : p.red));
      if (ImGui::Selectable(label.c_str()) && platform_.openPath) platform_.openPath(issue.file);
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Open %s (line %d)", issue.file.c_str(), issue.line);
      }
      ImGui::PopID();
    }
  }

  if (!output_.empty()) {
    ImGui::Spacing();
    if (ui::Disclosure("Build Output", issues_.empty())) {
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.9f);
      ImGui::BeginChild("##output", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
      ImGui::TextUnformatted(output_.c_str());
      ImGui::EndChild();
      ImGui::PopFont();
    }
  }
  ImGui::End();
}

} // namespace a2e::native
