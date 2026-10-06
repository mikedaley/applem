/*
 * test_native_dev.cpp - A program built with the user's own tools, run here
 *
 * The project file, what a build made, the build's errors and the volume a
 * program is booted from, then both ways of starting a program on a real
 * machine: from memory, and from that volume through ProDOS.
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "../src/develop/dev_project.hpp"
#include "../src/drives/media_store.hpp"
#include "../../src/host/machine_host.hpp"
#include "emulator.hpp"
#include "filesystem/prodos.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace a2e;
using namespace a2e::native::dev;
using a2e::host::MachineHost;

namespace {

// 0803 LDX #0 / 0805 LDA $0815,X / 0808 BEQ $0812 / 080A ORA #$80 /
// 080C JSR COUT / 080F INX / 0810 BNE $0805 / 0812 RTS / ... / 0815 text
std::vector<uint8_t> printer(const std::string &text) {
  std::vector<uint8_t> code = {0xA2, 0x00, 0xBD, 0x15, 0x08, 0xF0, 0x08, 0x09, 0x80, 0x20, 0xED,
                               0xFD, 0xE8, 0xD0, 0xF3, 0x60, 0xEA, 0xEA};
  for (char c : text) code.push_back(static_cast<uint8_t>(c));
  code.push_back(0);
  return code;
}

void runSeconds(MachineHost &host, double seconds) {
  std::vector<float> buffer(800 * 2);
  for (int i = 0; i < static_cast<int>(seconds * 60.0); i++) host.generateStereoAudioSamples(buffer.data(), 800);
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}

// cl65's AppleSingle: a BIN at `aux`, as `cl65 -t apple2` writes one.
std::vector<uint8_t> appleSingle(const std::vector<uint8_t> &data, uint16_t type, uint16_t aux) {
  std::vector<uint8_t> f;
  put32(f, 0x00051600);
  put32(f, 0x00020000);
  f.resize(24, 0);
  f.push_back(0);
  f.push_back(2); // two entries
  const uint32_t infoAt = 26 + 24;
  const uint32_t dataAt = infoAt + 8;
  put32(f, 11);
  put32(f, infoAt);
  put32(f, 8);
  put32(f, 1);
  put32(f, dataAt);
  put32(f, static_cast<uint32_t>(data.size()));
  f.insert(f.end(), {0x00, 0xC3, static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(type)});
  put32(f, aux);
  f.insert(f.end(), data.begin(), data.end());
  return f;
}

const char *DBG = "version\tmajor=2,minor=0\n"
                  "seg\tid=0,name=\"CODE\",start=0x000803,size=0x0021,addrsize=absolute,type=rw,oname=\"hello\",ooffs=0\n"
                  "seg\tid=2,name=\"BSS\",start=0x000824,size=0x0000,addrsize=absolute,type=rw\n"
                  "seg\tid=4,name=\"ZEROPAGE\",start=0x000000,size=0x0002,addrsize=zeropage,type=rw\n";

} // namespace

TEST_CASE("A project file says what to build and how to start it", "[dev]") {
  std::string error;
  const auto p = parseProject(R"({
    "build": "make all", "output": "build/game", "symbols": "build/game.dbg",
    "machine": "apple2e", "start": "memory", "load": "$0803", "entry": "0x0810", "name": "my game!"
  })", "/work/game/game.applem", error);
  REQUIRE(p);
  REQUIRE(p->directory == "/work/game");
  REQUIRE(p->name == "game");
  REQUIRE(p->build == "make all");
  REQUIRE(p->output == "/work/game/build/game");
  REQUIRE(p->symbols == "/work/game/build/game.dbg");
  REQUIRE(p->start == Project::Start::Memory);
  REQUIRE(p->load == std::optional<uint16_t>(0x0803));
  REQUIRE(p->entry == std::optional<uint16_t>(0x0810));
  REQUIRE(p->fileName == "MYGAME");

  // Only the output is needed; its name names the file.
  const auto small = parseProject(R"({"output": "hello"})", "/a/b.applem", error);
  REQUIRE(small);
  REQUIRE(small->start == Project::Start::Auto);
  REQUIRE(small->fileName == "HELLO");

  // And each mistake is said.
  REQUIRE_FALSE(parseProject(R"({"build": "make"})", "/a/b.applem", error));
  REQUIRE(error.find("output") != std::string::npos);
  REQUIRE_FALSE(parseProject(R"({"output": "x", "start": "soon"})", "/a/b.applem", error));
  REQUIRE_FALSE(parseProject(R"({"output": "x", "load": "$12345"})", "/a/b.applem", error));
  REQUIRE_FALSE(parseProject(R"({"output": "x" "load": "1"})", "/a/b.applem", error));
}

TEST_CASE("Addresses and ProDOS names are read the way they are written", "[dev]") {
  REQUIRE(parseAddress("$0803") == std::optional<uint16_t>(0x0803));
  REQUIRE(parseAddress("0x2000") == std::optional<uint16_t>(0x2000));
  REQUIRE(parseAddress("2051") == std::optional<uint16_t>(2051));
  REQUIRE_FALSE(parseAddress("$10000"));
  REQUIRE_FALSE(parseAddress("$08G3"));
  REQUIRE(prodosName("hello-world.bin") == "HELLOWORLD.BIN");
  REQUIRE(prodosName("1st") == "ST");
  REQUIRE(prodosName("a.very.long.name.indeed") == "A.VERY.LONG.NAM");
}

TEST_CASE("What a build made is read as a program", "[dev]") {
  std::string error;
  Project project = *parseProject(R"({"output": "hello"})", "/p/x.applem", error);
  const std::vector<uint8_t> code = printer("HI");

  SECTION("AppleSingle carries its type and load address") {
    const auto p = readProgram(appleSingle(code, 0x06, 0x0803), project, "", error);
    REQUIRE(p);
    REQUIRE(p->appleSingle);
    REQUIRE(p->wantsProDOS());
    REQUIRE(p->fileType == 0x06);
    REQUIRE(p->load == 0x0803);
    REQUIRE(p->bytes == code);
    // A SYSTEM file loads at $2000 whatever its aux type.
    REQUIRE(readProgram(appleSingle(code, 0xFF, 0), project, "", error)->load == 0x2000);
  }
  SECTION("A raw binary is placed by the .dbg's segments") {
    REQUIRE(loadAddressFromDbg(DBG) == std::optional<uint16_t>(0x0803));
    const auto p = readProgram(code, project, DBG, error);
    REQUIRE(p);
    REQUIRE_FALSE(p->wantsProDOS());
    REQUIRE(p->load == 0x0803);
    REQUIRE(p->entry == 0x0803);
  }
  SECTION("or by the project, or by a DOS 3.3 header") {
    project.load = 0x4000;
    REQUIRE(readProgram(code, project, "", error)->load == 0x4000);
    project.load.reset();
    std::vector<uint8_t> dos = {0x00, 0x60, static_cast<uint8_t>(code.size()), 0x00};
    dos.insert(dos.end(), code.begin(), code.end());
    const auto p = readProgram(dos, project, "", error);
    REQUIRE(p);
    REQUIRE(p->load == 0x6000);
    REQUIRE(p->bytes == code);
  }
  SECTION("and nothing to say where it goes is said") {
    REQUIRE_FALSE(readProgram(code, project, "", error));
    REQUIRE(error.find("load") != std::string::npos);
  }
  SECTION("nor anything that runs into the I/O space") {
    project.load = 0xBFF0;
    REQUIRE_FALSE(readProgram(code, project, "", error));
  }
}

TEST_CASE("The build's errors name their files and lines", "[dev]") {
  const std::string output = "cl65 -t apple2 -o build/game src/main.s\n"
                             "src/main.s:12: Error: Unexpected trailing garbage characters\n"
                             "src/main.s(14): Error: ':' expected\n"
                             "src/notes.txt(3) and something that is not ours\n"
                             "src/util.s:3: Warning: Symbol 'tmp' is defined but never used\n"
                             "src/c.c:7:5: error: use of undeclared identifier 'x'\n"
                             "ld65: Error: Missing memory area assignment for segment 'FOO'\n"
                             "make: *** [build/game] Error 1\n";
  const std::vector<Issue> issues = parseIssues(output, "/proj");
  REQUIRE(issues.size() == 4);
  REQUIRE(issues[0].file == "/proj/src/main.s");
  REQUIRE(issues[0].line == 12);
  REQUIRE_FALSE(issues[0].warning);
  REQUIRE(issues[0].message == "Unexpected trailing garbage characters");
  // cc65 2.18's own form, the line in brackets.
  REQUIRE(issues[1].file == "/proj/src/main.s");
  REQUIRE(issues[1].line == 14);
  REQUIRE(issues[1].message == "':' expected");
  REQUIRE(issues[2].warning);
  REQUIRE(issues[3].line == 7);
  REQUIRE(issues[3].column == 5);
}

TEST_CASE("A program starts from memory without any DOS", "[dev][machine]") {
  MachineHost host;
  host.build();
  host.reset();
  runSeconds(host, 1.0);
  std::vector<uint8_t> code = printer("STARTED FROM MEMORY");
  code[15] = 0x4C; // end in a loop rather than returning to nothing
  code[16] = 0x12;
  code[17] = 0x08;
  REQUIRE(host.startProgram(code.data(), code.size(), 0x0803, 0x0803));
  runSeconds(host, 0.5);
  REQUIRE(host.screenText().find("STARTED FROM MEMORY") != std::string::npos);
}

TEST_CASE("A volume made for a program boots straight into it", "[dev][machine]") {
  const auto system = native::readFile("public/disks/ProDOS 2.4.3.po");
  if (!system) {
    WARN("public/disks/ProDOS 2.4.3.po not found; skipping");
    return;
  }
  std::string error;
  Program program;
  program.bytes = printer("STARTED FROM PRODOS");
  program.load = program.entry = program.auxType = 0x0803;

  SECTION("a BIN, through BASIC.SYSTEM and a STARTUP that runs it") {
    const auto volume = makeBootVolume(*system, program, "HELLO", 280, error);
    INFO(error);
    REQUIRE(volume);
    // The volume holds what it says, in the order that boots it.
    std::vector<ProDOSCatalogEntry> entries(16);
    const int count = ProDOS::readDirectory(volume->data(), volume->size(), 2, "", entries.data(), 16);
    REQUIRE(count == 4);
    REQUIRE(std::string(entries[0].filename) == "PRODOS");
    REQUIRE(std::string(entries[1].filename) == "BASIC.SYSTEM");
    REQUIRE(std::string(entries[2].filename) == "STARTUP");
    REQUIRE(std::string(entries[3].filename) == "HELLO");
    REQUIRE(entries[3].auxType == 0x0803);

    // And a //e booting it runs the program, the way a person would see it.
    MachineHost host;
    host.build();
    REQUIRE(host.setSlotCard(7, "empty")); // so the scan reaches the floppy
    REQUIRE(host.insertDisk(0, volume->data(), volume->size(), "applem.po"));
    host.reset();
    runSeconds(host, 12.0);
    INFO(host.screenText());
    REQUIRE(host.screenText().find("STARTED FROM PRODOS") != std::string::npos);
  }
  SECTION("a SYSTEM program, run by ProDOS itself") {
    program.fileType = 0xFF;
    const auto volume = makeBootVolume(*system, program, "HELLO", 1600, error);
    REQUIRE(volume);
    REQUIRE(volume->size() == 1600 * 512u);
    std::vector<ProDOSCatalogEntry> entries(16);
    const int count = ProDOS::readDirectory(volume->data(), volume->size(), 2, "", entries.data(), 16);
    REQUIRE(count == 2);
    REQUIRE(std::string(entries[1].filename) == "HELLO.SYSTEM");
    REQUIRE(entries[1].fileType == 0xFF);
  }
}

