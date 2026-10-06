/*
 * develop.hpp - Building the user's program and running it here
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "develop/dev_project.hpp"
#include "app/machine_poll.hpp"
#include "app/platform.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

class Emulation;

// The user's own editor, Makefile and cl65, and ApplEm as where a build lands.
// A project file (dev_project.hpp) says what to build and how to start it;
// Build and Run runs the build in the user's login shell, off the UI thread,
// and on success starts the program; its errors are a list that opens each
// file in the app that edits it. Watching the output, a build made anywhere
// (the editor's own build task, make in a terminal) is run as soon as it is
// written.
//
// A program is started one of two ways. From memory: put at its load address
// and entered through the reset (MachineHost::startProgram), in a moment and
// with no DOS, for a program that runs on the bare machine. From disk: a
// ProDOS volume made for it (dev::makeBootVolume) goes in the SmartPort, or the
// first floppy drive on a machine with no SmartPort, and the machine starts
// from it, for a program that calls ProDOS, as everything cl65 builds in C
// does. "auto" chooses by what the build made: AppleSingle is cl65's ProDOS
// program, a raw binary a bare one.
class Develop {
public:
  struct Hooks {
    // The machine the project names, switched to without asking: running the
    // project is asking.
    std::function<bool(const std::string &machineKey)> useMachine;
    std::function<std::string()> machineKey;
    // The debugger's symbols, from the build's .dbg.
    std::function<int(const std::string &dbg)> importSymbols;
    // A volume into the SmartPort's first device, or the first floppy drive.
    std::function<bool()> hasSmartPort;
    std::function<void(const std::string &path)> insertHardDrive;
    std::function<void(const std::string &path)> insertFloppy;
  };

  Develop(Emulation &emulation, Platform &platform, Hooks hooks);
  ~Develop();

  void openProject(const std::string &path);
  void closeProject();
  void chooseProject();
  bool hasProject() const { return project_.has_value(); }
  const std::string &projectPath() const { return projectPath_; }

  // Build if the project says how, then start what was built.
  void buildAndRun();
  // Start the last build again, without building.
  void runAgain();
  bool building() const { return build_.valid(); }

  // Run whatever the build writes, as soon as it has written it.
  bool watch = false;

  // Every frame: a build finishing, a watched file changing, a start waiting
  // for the machine to come up.
  void update(double now);
  void draw(bool *open);

  // Show the window: something happened the user should see.
  bool wantsWindow() {
    const bool wants = wantsWindow_;
    wantsWindow_ = false;
    return wants;
  }

private:
  struct BuildResult {
    int status = -1;
    std::string output;
  };

  void run();
  void startFromMemory(const dev::Program &program);
  void startFromDisk(const dev::Program &program);
  void report(const std::string &status, bool problem);
  std::optional<std::filesystem::file_time_type> modified(const std::string &path) const;

  Emulation &emulation_;
  Platform &platform_;
  Hooks hooks_;
  std::string projectPath_;
  std::optional<dev::Project> project_;

  std::future<BuildResult> build_;
  double buildStarted_ = 0;
  std::string output_;
  std::vector<dev::Issue> issues_;
  std::string status_;
  bool problem_ = false;
  bool wantsWindow_ = false;

  // The output and its .dbg as last run, and a change seen but not yet run:
  // a linker writes in pieces, so a file is run once it has stopped changing.
  std::optional<std::filesystem::file_time_type> ranOutput_;
  std::optional<std::filesystem::file_time_type> seenOutput_;
  double seenAt_ = -1;
  double lastPoll_ = 0;

  // A start waiting for a machine just switched on to finish starting itself.
  std::optional<dev::Program> pendingStart_;
  double startAt_ = 0;
  double now_ = 0;
};

} // namespace a2e::native
