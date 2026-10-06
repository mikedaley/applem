/*
 * dev_project.hpp - A program the user builds with their own tools, run here
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native::dev {

// The user writes their program with their own editor and toolchain (ca65 and
// ld65, usually through cl65 and a Makefile), and ApplEm is where a build
// lands and runs. This is the part of that with no ImGui in it: the project
// file, reading what the build made, the build's errors, and the ProDOS
// volume a program is started from. All of it is unit-tested
// (test_native_dev).

// A project file, `<name>.applem`: a flat JSON object beside the Makefile.
//
//   {
//     "build":   "make",                 shell command, run in the project's folder
//     "output":  "build/game",           what the build makes
//     "symbols": "build/game.dbg",       ca65's debug file, for the debugger
//     "machine": "apple2e",              apple2plus, apple2e, apple2c, apple2gs
//     "start":   "auto",                 auto, memory or disk
//     "load":    "$0803",                where a raw binary goes, if no .dbg says
//     "entry":   "$0803",                where it starts, if not where it loads
//     "disk":    "system.po",            the ProDOS disk to take PRODOS and BASIC.SYSTEM from
//     "name":    "GAME"                  its name on that volume
//   }
//
// Only "output" is needed. Paths are relative to the project file.
struct Project {
  enum class Start { Auto, Memory, Disk };

  std::string path;      // the .applem file
  std::string directory; // its folder: the build runs here and paths resolve here
  std::string name;      // the project file's name without its extension
  std::string build;
  std::string output;  // absolute
  std::string symbols; // absolute, or empty
  std::string machine; // a machine profile key, or empty to keep the machine
  Start start = Start::Auto;
  std::optional<uint16_t> load;
  std::optional<uint16_t> entry;
  std::string disk;     // a ProDOS system disk, absolute, or empty for the app's own
  std::string fileName; // as ProDOS will name it
};

// The project, or why not.
std::optional<Project> parseProject(const std::string &json, const std::string &path, std::string &error);

// An address as a project file writes one: $0803, 0x0803 or 2051.
std::optional<uint16_t> parseAddress(const std::string &text);

// A name ProDOS takes: a letter, then letters, digits and full stops, 15 at
// most, in capitals.
std::string prodosName(const std::string &text);

// What a build made, ready to start.
struct Program {
  std::vector<uint8_t> bytes;
  uint16_t load = 0;
  uint16_t entry = 0;
  uint8_t fileType = 0x06; // ProDOS's BIN
  uint16_t auxType = 0;
  bool appleSingle = false;
  // How it is started when the project says "auto": a program cl65 wrote
  // as AppleSingle was built for ProDOS, and calls it; a raw binary is
  // started on the bare machine.
  bool wantsProDOS() const { return appleSingle; }
};

// The build's output read as a program. In order of what it says about
// itself: AppleSingle (cl65's default for the Apple II, carrying the ProDOS
// type and load address), a raw binary placed by the project's "load" or the
// .dbg's segments, then a DOS 3.3 binary's four-byte header.
std::optional<Program> readProgram(const std::vector<uint8_t> &file, const Project &project, const std::string &dbg,
                                   std::string &error);

// The lowest address the linker put anything at, from a ca65 .dbg: where a
// raw binary was linked to load.
std::optional<uint16_t> loadAddressFromDbg(const std::string &dbg);

// A line of the build's output that names a place in a source file.
struct Issue {
  std::string file; // absolute
  int line = 0;
  int column = 0;
  bool warning = false;
  std::string message;
};

// ca65's "file.s:12: Error: ...", ld65's "file.cfg:4: Error: ...", and the
// compilers' "file.c:12:5: error: ...". Lines naming no file are not issues.
std::vector<Issue> parseIssues(const std::string &output, const std::string &directory);

// A ProDOS volume that starts the program when booted: the system disk's
// boot blocks and PRODOS, then the program itself if it is a SYSTEM file
// (ProDOS runs the first one), or else BASIC.SYSTEM and a STARTUP program
// that runs it. Built fresh each time, because a system disk's own first
// SYSTEM file is usually a menu (ProDOS 2.4's Bitsy Bye). `blocks` is 280 for
// a 5.25" disk; anything bigger is a SmartPort volume.
std::optional<std::vector<uint8_t>> makeBootVolume(const std::vector<uint8_t> &systemDisk, const Program &program,
                                                   const std::string &fileName, int blocks, std::string &error);

} // namespace a2e::native::dev
