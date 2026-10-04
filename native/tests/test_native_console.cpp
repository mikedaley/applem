/*
 * test_native_console.cpp - The debugging console, against a real machine
 *
 * The parser is pinned in test_native_debugger; this types whole commands
 * into a console over a //e that is built but powered off, so nothing runs
 * between them, and reads back what the console printed and what it did to
 * the machine and the shared breakpoint list.
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "../src/console_window.hpp"
#include "../src/cpu_debugger.hpp"
#include "../src/emulation.hpp"
#include "../src/platform.hpp"

#include "imgui.h"

#include <string>

using namespace a2e;
using namespace a2e::native;

namespace {

struct Fixture {
  Fixture() {
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    emulation.start(MachineId::AppleIIe, 0);
    const MachineProfile &profile = machineProfile(MachineId::AppleIIe);
    debugger.setMachine(profile);
    console.setMachine(profile);
  }
  ~Fixture() {
    emulation.stop();
    ImGui::DestroyContext();
  }

  // Everything the last command printed, a line each.
  std::string run(const std::string &line) {
    const size_t before = console.output().size();
    console.run(line);
    std::string out;
    for (size_t i = before + 1; i < console.output().size(); i++) out += console.output()[i].text + "\n";
    return out;
  }
  bool errored() const { return console.output().back().style == ConsoleWindow::Line::Style::Error; }

  Platform platform;
  Emulation emulation;
  Breakpoints breakpoints;
  CpuDebugger debugger{emulation, platform, breakpoints};
  ConsoleWindow console{emulation, debugger, breakpoints};
};

} // namespace

TEST_CASE("The monitor's syntax reads and writes the machine's memory", "[console]") {
  Fixture f;
  const std::string written = f.run("300: A9 42 8D 00 04");
  REQUIRE(written.find("0300- A9 42 8D 00 04") != std::string::npos);
  REQUIRE(f.run("300.304").find("0300- A9 42 8D 00 04") != std::string::npos);
  REQUIRE(f.run("? PEEK($301)") == "= $42  66\n");
  REQUIRE(f.run("w $302 EA EA").find("0302- EA EA") != std::string::npos);
  // A range starts its first line where it was asked to, then goes by eights.
  const std::string range = f.run("2FE.309");
  REQUIRE(range.find("02FE- ") == 0);
  REQUIRE(range.find("0300- A9 42 EA EA 04") != std::string::npos);
  REQUIRE(range.find("0308- ") != std::string::npos);
  REQUIRE(f.run("f $2000.$2007 $11").find("Filled 2000.2007") != std::string::npos);
  REQUIRE(f.run("m $2000").find("2000- 11 11 11 11 11 11 11 11") != std::string::npos);

  // The ROM refuses.
  f.run("w $F800 00");
  REQUIRE(f.errored());
}

TEST_CASE("The console lists code, with the machine's names", "[console]") {
  Fixture f;
  f.run("300: A9 42 20 ED FD 60");
  const std::string listing = f.run("l $300 3");
  REQUIRE(listing.find("0300") != std::string::npos);
  REQUIRE(listing.find("LDA #$42") != std::string::npos);
  REQUIRE(listing.find("JSR") != std::string::npos);
  REQUIRE(listing.find("COUT") != std::string::npos);
  REQUIRE(listing.find("RTS") != std::string::npos);
  REQUIRE(f.run("300L").find("LDA #$42") != std::string::npos);
}

TEST_CASE("Registers are shown and set, by number or by name", "[console]") {
  Fixture f;
  REQUIRE(f.run("r a=$42 x=3 pc=COUT").find("PC=FDED A=42 X=03") != std::string::npos);
  REQUIRE(f.run("? A + X") == "= $45  69\n");
  f.run("r pbr=1");
  REQUIRE(f.errored()); // a 6502 has none
}

TEST_CASE("Names and addresses are looked up both ways", "[console]") {
  Fixture f;
  REQUIRE(f.run("sym COUT").find("COUT = FDED") != std::string::npos);
  REQUIRE(f.run("sym $FDED").find("FDED is COUT") != std::string::npos);
  f.run("sym NOSUCHNAME");
  REQUIRE(f.errored());
}

TEST_CASE("Breakpoints go into the list every window shares", "[console][breakpoints]") {
  Fixture f;
  f.run("bp COUT if A == $C1");
  f.run("bp w $0400-$07FF");
  f.run("bp sw page2 on");
  f.run("bp beam col 10");
  const auto &all = f.breakpoints.all();
  REQUIRE(all.size() == 4);
  REQUIRE(all[0].kind == Breakpoint::Kind::Exec);
  REQUIRE(all[0].start == 0xFDED);
  REQUIRE(all[0].condition == "A == $C1");
  REQUIRE(all[1].end == 0x07FF);
  REQUIRE(all[2].key == "page2");
  REQUIRE(all[2].equals);
  // A visible column becomes the scanner's position, blanking included.
  REQUIRE(all[3].hPos == 10 + machineProfile(MachineId::AppleIIe).timing.hblankCycles);
  REQUIRE(all[3].scanline == -1);

  f.run("bp COUT");
  REQUIRE(f.errored()); // already there

  const std::string list = f.run("bl");
  REQUIRE(list.find("exec  FDED COUT  if A == $C1") != std::string::npos);
  REQUIRE(list.find("switch PAGE2 on") != std::string::npos);
  REQUIRE(list.find("beam  column 10, every line") != std::string::npos);

  f.run("bx 2");
  REQUIRE_FALSE(all[1].enabled);
  f.run("be all");
  REQUIRE(all[1].enabled);
  f.run("bd 1");
  REQUIRE(all.size() == 3);
  f.run("bd 9");
  REQUIRE(f.errored());
  f.run("bd all");
  REQUIRE(all.empty());
}

TEST_CASE("Find searches memory for bytes and for text", "[console]") {
  Fixture f;
  f.run("300: A9 77 8D 55 C0");
  REQUIRE(f.run("find A9 77 8D").find("0300") != std::string::npos);
  REQUIRE(f.run("find 8D ?? C0").find("0302") != std::string::npos);
  // Text matches with its high bit set, as the screen holds it.
  f.run("400: C8 C5 CC CC CF");
  REQUIRE(f.run("find \"HELLO\"").find("0400") != std::string::npos);
}

TEST_CASE("Help lists every command, and one on its own", "[console]") {
  Fixture f;
  const std::string all = f.run("help");
  for (const ConsoleCommandHelp &h : consoleCommands()) {
    INFO(h.name);
    REQUIRE(all.find(h.summary) != std::string::npos);
  }
  const std::string bp = f.run("help bp");
  REQUIRE(bp.find("soft switch") != std::string::npos);
  REQUIRE(bp.find("beam") != std::string::npos);
  f.run("frobnicate");
  REQUIRE(f.errored());
}
