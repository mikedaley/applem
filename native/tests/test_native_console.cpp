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

#include "../src/debugger/console_window.hpp"
#include "../src/debugger/cpu_debugger.hpp"
#include "../src/app/emulation.hpp"
#include "../src/app/platform.hpp"

#include "imgui.h"

#include <string>

using namespace a2e;
using namespace a2e::native;

namespace {

struct Fixture {
  explicit Fixture(MachineId machine = MachineId::AppleIIe) {
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    emulation.start(machine, iigs::FAST_RAM_SIZE_ROM01);
    const MachineProfile &profile = machineProfile(machine);
    debugger.setMachine(profile);
    console.setMachine(profile);
  }

  // The machine runs only when told, here: the emulation thread is powered
  // off. Then the debugger and the console look, as the App has them do once
  // a frame.
  void runFor(int cycles) {
    emulation.withMachine([&](host::MachineHost &host) { host.runCycles(cycles); });
  }
  void frame() {
    debugger.update();
    console.update();
  }
  host::CpuState cpu() {
    host::CpuState c;
    emulation.withMachine([&](host::MachineHost &host) { c = host.cpuState(); });
    return c;
  }
  std::string since(size_t from) const {
    std::string out;
    for (size_t i = from; i < console.output().size(); i++) out += console.output()[i].text + "\n";
    return out;
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

TEST_CASE("s steps as many instructions as it says", "[console]") {
  Fixture f;
  f.run("300: EA EA EA EA EA EA EA EA EA EA");
  f.run("r pc=300");
  f.run("s 5");
  REQUIRE((f.cpu().pc & 0xFFFF) == 0x0305); // five, not six
  f.run("s");
  REQUIRE((f.cpu().pc & 0xFFFF) == 0x0306);
}

TEST_CASE("n and finish say where they stopped, as s does", "[console]") {
  Fixture f;
  f.run("300: EA EA");
  f.run("r pc=300");
  // Over an instruction that is not a call is a step, said at once.
  REQUIRE(f.run("n").find("0301 >") != std::string::npos);
}

TEST_CASE("Numbers are hex in an expression as alone", "[console]") {
  Fixture f;
  f.run("w 300 10");
  f.run("w 301 10+1");
  REQUIRE(f.run("300.301").find("0300- 10 11") != std::string::npos);
  REQUIRE(f.run("? 10") == "= $10  16\n");
  REQUIRE(f.run("? #10") == "= $0A  10\n");
  f.run("? 1/0");
  REQUIRE(f.errored());
  REQUIRE(f.console.output().back().text.find("Division by zero") != std::string::npos);
}

TEST_CASE("A breakpoint keeps its number when another is deleted", "[console][breakpoints]") {
  Fixture f;
  f.run("bp 2000");
  f.run("bp 3000");
  f.run("bp 4000");
  f.run("bd 1");
  REQUIRE(f.run("bl").find("3    exec  4000") != std::string::npos);
  f.run("bd 3"); // still the one at 4000
  REQUIRE_FALSE(f.errored());
  REQUIRE(f.breakpoints.all().size() == 1);
  REQUIRE(f.breakpoints.all()[0].start == 0x3000);
  f.run("bd 4294967297");
  REQUIRE(f.errored());
}

TEST_CASE("A condition the evaluator cannot read is refused when it is set", "[console][breakpoints]") {
  Fixture f;
  f.run("bp 300 if A = $41");
  REQUIRE(f.errored());
  REQUIRE(f.breakpoints.all().empty());
  f.run("bp 300 if NOSUCH == 1");
  REQUIRE(f.errored());
}

TEST_CASE("A stop moments after Continue is judged", "[console][breakpoints]") {
  // 300: LDA #$C1, JSR $310, LDA #$C2, JSR $310, JMP $300; 310: RTS. The
  // breakpoint at $310 is conditional, so the first arrival must go back to
  // running and the second must stop, with neither seen running by a frame.
  Fixture f;
  f.run("300: A9 C1 20 10 03 A9 C2 20 10 03 4C 00 03");
  f.run("310: 60");
  f.run("r pc=300");
  f.run("pause");
  f.frame();
  REQUIRE(f.debugger.stopReason() == "Paused");

  f.run("bp 310 if A == $C2");
  f.run("g");
  const size_t mark = f.console.output().size();
  f.runFor(200);
  f.frame(); // the first arrival: A is $C1, so it is sent back
  REQUIRE((f.cpu().a & 0xFF) == 0xC1);
  REQUIRE(f.breakpoints.all()[0].hits == 0);
  REQUIRE_FALSE(f.emulation.withMachine([](host::MachineHost &host) { return host.isPaused(); }));
  f.runFor(200);
  f.frame();
  REQUIRE((f.cpu().pc & 0xFFFF) == 0x0310);
  REQUIRE((f.cpu().a & 0xFF) == 0xC2);
  REQUIRE(f.debugger.stopReason() == "Breakpoint at 0310");
  REQUIRE(f.breakpoints.all()[0].hits == 1);
  REQUIRE(f.since(mark).find("Stopped: Breakpoint at 0310") != std::string::npos);

  // Continue from it runs on past it, round the loop (past the $C1 arrival,
  // which is sent back again), to the next one.
  f.run("g");
  f.runFor(200);
  f.frame();
  f.runFor(200);
  f.frame();
  REQUIRE((f.cpu().a & 0xFF) == 0xC2);
  REQUIRE(f.breakpoints.all()[0].hits == 2);
}

TEST_CASE("Adding a breakpoint while stopped does not stop Continue in place", "[console][breakpoints]") {
  Fixture f;
  f.run("300: EA EA EA EA 4C 00 03");
  f.run("r pc=300");
  f.run("bp 302");
  f.run("g");
  f.runFor(50);
  f.frame();
  REQUIRE((f.cpu().pc & 0xFFFF) == 0x0302);
  f.run("bp 2000"); // an edit while stopped
  f.run("g");
  f.runFor(3); // one instruction's worth, and a little
  REQUIRE((f.cpu().pc & 0xFFFF) != 0x0302);
}

TEST_CASE("300G calls the routine and comes back, as the monitor's G does", "[console]") {
  // 400: JMP $400, the machine idling; 300: LDA #$42, RTS.
  Fixture f;
  f.run("400: 4C 00 04");
  f.run("300: A9 42 60");
  f.run("r pc=400 a=7");
  f.run("pause");
  f.frame();
  const host::CpuState before = f.cpu();

  const size_t mark = f.console.output().size();
  f.run("300G");
  f.runFor(100);
  f.frame();
  const std::string said = f.since(mark);
  REQUIRE(said.find("Returned from 0300") != std::string::npos);
  REQUIRE(said.find("A=42") != std::string::npos); // what the routine left
  // Back where it was, the stack as it was and the registers it had.
  const host::CpuState after = f.cpu();
  REQUIRE((after.pc & 0xFFFF) == 0x0400);
  REQUIRE(after.sp == before.sp);
  REQUIRE((after.a & 0xFF) == 0x07);
}

TEST_CASE("A IIgs's stops are judged and counted", "[console][breakpoints][iigs]") {
  // The scenario that showed "Paused" and no hits: a write watchpoint on
  // 00/0300, and SEP #$30, LDA #5, STA $0300 run from 00/1000; then an
  // execution breakpoint after it.
  Fixture f(MachineId::AppleIIgs);
  f.run("00/1000: E2 30 A9 05 8D 00 03 EA EA EA 80 FE");
  f.run("r pc=00/1000");
  f.run("pause");
  f.frame();
  f.run("bp w 00/0300");
  f.run("bp 00/1009");
  f.run("g");
  f.runFor(100);
  f.frame();
  REQUIRE(f.debugger.stopReason().find("Write 00/0300") == 0);
  REQUIRE(f.breakpoints.all()[0].hits == 1);
  f.run("g");
  f.runFor(100);
  f.frame();
  REQUIRE(f.debugger.stopReason() == "Breakpoint at 00/1009");
  REQUIRE(f.breakpoints.all()[1].hits == 1);
}
