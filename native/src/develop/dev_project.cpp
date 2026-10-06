/*
 * dev_project.cpp - A program the user builds with their own tools, run here
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "develop/dev_project.hpp"

#include "basic/basic_tokenizer.hpp"
#include "filesystem/prodos.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace a2e::native::dev {

namespace {

std::string upper(std::string text) {
  for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

// A flat JSON object of strings, numbers and booleans, which is all a project
// file is. Values come back as their text.
bool parseFlatObject(const std::string &json, std::map<std::string, std::string> &out, std::string &error) {
  size_t i = 0;
  auto skip = [&] {
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) i++;
  };
  auto string = [&](std::string &value) {
    if (i >= json.size() || json[i] != '"') return false;
    i++;
    value.clear();
    while (i < json.size() && json[i] != '"') {
      char c = json[i++];
      if (c == '\\' && i < json.size()) {
        const char e = json[i++];
        c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
      }
      value += c;
    }
    if (i >= json.size()) return false;
    i++;
    return true;
  };
  skip();
  if (i >= json.size() || json[i] != '{') {
    error = "A project file is a JSON object, in braces.";
    return false;
  }
  i++;
  skip();
  if (i < json.size() && json[i] == '}') return true;
  while (i < json.size()) {
    std::string key;
    std::string value;
    skip();
    if (!string(key)) break;
    skip();
    if (i >= json.size() || json[i] != ':') break;
    i++;
    skip();
    if (i < json.size() && json[i] == '"') {
      if (!string(value)) break;
    } else {
      const size_t from = i;
      while (i < json.size() && (std::isalnum(static_cast<unsigned char>(json[i])) || json[i] == '-' || json[i] == '.')) i++;
      if (i == from) break;
      value = json.substr(from, i - from);
    }
    out[key] = value;
    skip();
    if (i < json.size() && json[i] == ',') {
      i++;
      continue;
    }
    if (i < json.size() && json[i] == '}') return true;
    break;
  }
  error = "The project file is not valid JSON (near character " + std::to_string(i + 1) + ").";
  return false;
}

std::string absolute(const std::string &directory, const std::string &path) {
  if (path.empty()) return path;
  fs::path p(path);
  if (!p.is_absolute()) p = fs::path(directory) / p;
  return p.lexically_normal().string();
}

uint32_t be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

} // namespace

std::optional<uint16_t> parseAddress(const std::string &text) {
  std::string t = text;
  int base = 10;
  if (!t.empty() && t[0] == '$') {
    t = t.substr(1);
    base = 16;
  } else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
    t = t.substr(2);
    base = 16;
  }
  if (t.empty() || t.size() > 6) return std::nullopt;
  unsigned long value = 0;
  for (char c : t) {
    const int digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                      : base == 16 && std::isxdigit(static_cast<unsigned char>(c))
                          ? std::toupper(static_cast<unsigned char>(c)) - 'A' + 10
                          : -1;
    if (digit < 0) return std::nullopt;
    value = value * static_cast<unsigned long>(base) + static_cast<unsigned long>(digit);
  }
  if (value > 0xFFFF) return std::nullopt;
  return static_cast<uint16_t>(value);
}

std::string prodosName(const std::string &text) {
  std::string name;
  for (char c : upper(text)) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.') name += c;
  }
  while (!name.empty() && !std::isalpha(static_cast<unsigned char>(name[0]))) name.erase(0, 1);
  if (name.empty()) name = "PROGRAM";
  return name.substr(0, 15);
}

std::optional<Project> parseProject(const std::string &json, const std::string &path, std::string &error) {
  std::map<std::string, std::string> v;
  if (!parseFlatObject(json, v, error)) return std::nullopt;
  Project p;
  p.path = path;
  p.directory = fs::path(path).parent_path().string();
  p.name = fs::path(path).stem().string();
  p.build = v["build"];
  if (v["output"].empty()) {
    error = "The project does not say what the build makes: give it an \"output\".";
    return std::nullopt;
  }
  p.output = absolute(p.directory, v["output"]);
  p.symbols = absolute(p.directory, v["symbols"]);
  p.machine = v["machine"];
  p.disk = absolute(p.directory, v["disk"]);
  const std::string start = v["start"];
  if (start.empty() || start == "auto") p.start = Project::Start::Auto;
  else if (start == "memory") p.start = Project::Start::Memory;
  else if (start == "disk") p.start = Project::Start::Disk;
  else {
    error = "\"start\" is auto, memory or disk, not \"" + start + "\".";
    return std::nullopt;
  }
  for (const char *key : {"load", "entry"}) {
    if (v[key].empty()) continue;
    const auto address = parseAddress(v[key]);
    if (!address) {
      error = std::string("\"") + key + "\" is not an address: \"" + v[key] + "\".";
      return std::nullopt;
    }
    (std::string(key) == "load" ? p.load : p.entry) = address;
  }
  p.fileName = prodosName(v["name"].empty() ? fs::path(p.output).stem().string() : v["name"]);
  return p;
}

// The linker's segments, each "seg id=..,name=..,start=0x..,size=0x..,..,oname=..".
// A segment with an output name was written to the file; the lowest start
// among them is where the file loads.
std::optional<uint16_t> loadAddressFromDbg(const std::string &dbg) {
  std::optional<uint16_t> lowest;
  std::istringstream in(dbg);
  std::string line;
  const std::regex start(R"(start=0x([0-9A-Fa-f]+))");
  const std::regex size(R"(size=0x([0-9A-Fa-f]+))");
  while (std::getline(in, line)) {
    if (line.rfind("seg", 0) != 0 || line.find("oname=") == std::string::npos) continue;
    std::smatch s, z;
    if (!std::regex_search(line, s, start) || !std::regex_search(line, z, size)) continue;
    if (std::stoul(z[1].str(), nullptr, 16) == 0) continue;
    const unsigned long at = std::stoul(s[1].str(), nullptr, 16);
    if (at > 0xFFFF) continue;
    if (!lowest || at < *lowest) lowest = static_cast<uint16_t>(at);
  }
  return lowest;
}

std::optional<Program> readProgram(const std::vector<uint8_t> &file, const Project &project, const std::string &dbg,
                                   std::string &error) {
  Program program;
  // AppleSingle: big-endian, magic $00051600, a count of entries at 24 and
  // the entries from 26, twelve bytes each (id, offset, length). Entry 1 is
  // the file's data and entry 11 its ProDOS type and aux type.
  if (file.size() >= 26 && be32(file.data()) == 0x00051600) {
    const int count = be16(&file[24]);
    bool data = false;
    for (int e = 0; e < count; e++) {
      const size_t at = 26 + static_cast<size_t>(e) * 12;
      if (at + 12 > file.size()) break;
      const uint32_t id = be32(&file[at]);
      const uint32_t offset = be32(&file[at + 4]);
      const uint32_t length = be32(&file[at + 8]);
      if (static_cast<uint64_t>(offset) + length > file.size()) continue;
      if (id == 1) {
        program.bytes.assign(file.begin() + offset, file.begin() + offset + length);
        data = true;
      } else if (id == 11 && length >= 8) {
        program.fileType = static_cast<uint8_t>(be16(&file[offset + 2]));
        program.auxType = static_cast<uint16_t>(be32(&file[offset + 4]));
      }
    }
    if (!data) {
      error = "The AppleSingle file has no data in it.";
      return std::nullopt;
    }
    program.appleSingle = true;
    // A BIN or SYSTEM file's aux type is its load address; a SYSTEM file
    // always loads at $2000.
    program.load = project.load.value_or(program.fileType == 0xFF ? 0x2000 : program.auxType);
  } else if (const auto from = project.load ? project.load : loadAddressFromDbg(dbg)) {
    program.bytes = file;
    program.load = *from;
  } else if (file.size() > 4 && static_cast<size_t>(file[2] | (file[3] << 8)) == file.size() - 4) {
    // A DOS 3.3 binary file: its load address and length, then the bytes.
    program.load = static_cast<uint16_t>(file[0] | (file[1] << 8));
    program.bytes.assign(file.begin() + 4, file.end());
  } else {
    error = "Nothing says where " + fs::path(project.output).filename().string() +
            " loads: give the project a \"load\" address, or its \"symbols\" (the .dbg).";
    return std::nullopt;
  }
  if (program.bytes.empty()) {
    error = fs::path(project.output).filename().string() + " is empty.";
    return std::nullopt;
  }
  if (program.load + program.bytes.size() > 0xC000) {
    char text[96];
    std::snprintf(text, sizeof text, "The program runs from $%04X past $BFFF, into the I/O space.", program.load);
    error = text;
    return std::nullopt;
  }
  program.entry = project.entry.value_or(program.load);
  if (!program.appleSingle) program.auxType = program.load;
  return program;
}

std::vector<Issue> parseIssues(const std::string &output, const std::string &directory) {
  std::vector<Issue> issues;
  // file(line): severity: message, as ca65, ld65 and cc65 2.18 write it, or
  // file:line[:column]: severity: message, as later ones and the C
  // compilers do. cc65's tools write "Error" and "Warning"; the C compilers
  // write them in lower case.
  const std::regex line(
      R"(^([^:(\s][^:(]*)(?::(\d+)(?::(\d+))?|\((\d+)\)):\s*([A-Za-z ]*?(?:[Ee]rror|[Ww]arning|[Nn]ote))?:?\s*(.*)$)");
  std::istringstream in(output);
  std::string text;
  while (std::getline(in, text)) {
    if (!text.empty() && text.back() == '\r') text.pop_back();
    std::smatch m;
    if (!std::regex_match(text, m, line)) continue;
    const std::string file = m[1].str();
    // A make target or a tool's own name is not a source file.
    if (file == "make" || file.rfind("make[", 0) == 0 || file == "ld65" || file == "ca65" || file == "cl65") continue;
    Issue issue;
    issue.file = absolute(directory, file);
    issue.line = std::stoi(m[2].matched ? m[2].str() : m[4].str());
    issue.column = m[3].matched ? std::stoi(m[3].str()) : 0;
    const std::string severity = upper(m[5].str());
    if (severity.empty() && !m[3].matched) continue; // "a:1: something" with no severity is not ours
    issue.warning = severity.find("WARNING") != std::string::npos || severity.find("NOTE") != std::string::npos;
    issue.message = m[6].str();
    issues.push_back(issue);
  }
  return issues;
}

namespace {

constexpr int BLOCK = 512;

// A blank ProDOS volume: the system disk's boot blocks, a four-block volume
// directory from block 2, and the free-block bitmap after it.
std::vector<uint8_t> blankVolume(const std::vector<uint8_t> &systemDisk, int blocks, const std::string &name) {
  std::vector<uint8_t> v(static_cast<size_t>(blocks) * BLOCK, 0);
  std::memcpy(v.data(), systemDisk.data(), 2 * BLOCK); // the boot loader, which loads PRODOS
  for (int b = 2; b <= 5; b++) {
    uint8_t *dir = &v[static_cast<size_t>(b) * BLOCK];
    dir[0] = static_cast<uint8_t>(b == 2 ? 0 : b - 1);
    dir[2] = static_cast<uint8_t>(b == 5 ? 0 : b + 1);
  }
  uint8_t *header = &v[2 * BLOCK + 4];
  header[0] = static_cast<uint8_t>(0xF0 | name.size()); // a volume directory header
  std::memcpy(header + 1, name.data(), name.size());
  header[0x22 - 4] = 0xC3; // access: read, write, rename, destroy
  header[0x23 - 4] = 0x27; // entry length
  header[0x24 - 4] = 0x0D; // entries per block
  const int bitmapBlocks = (blocks + 4095) / 4096;
  header[0x27 - 4] = 6; // the bitmap's first block
  header[0x29 - 4] = static_cast<uint8_t>(blocks & 0xFF);
  header[0x2A - 4] = static_cast<uint8_t>(blocks >> 8);
  // A set bit is a free block; the boot blocks, the directory and the
  // bitmap itself are in use.
  const int used = 6 + bitmapBlocks;
  for (int b = used; b < blocks; b++) v[6 * BLOCK + static_cast<size_t>(b / 8)] |= static_cast<uint8_t>(0x80 >> (b % 8));
  return v;
}

std::optional<std::vector<uint8_t>> fileFrom(const std::vector<uint8_t> &disk, const char *name, uint8_t &type,
                                             uint16_t &aux) {
  std::vector<ProDOSCatalogEntry> entries(64);
  const int count = ProDOS::readDirectory(disk.data(), disk.size(), 2, "", entries.data(), 64);
  for (int i = 0; i < count; i++) {
    if (std::strcmp(entries[static_cast<size_t>(i)].filename, name) != 0) continue;
    const ProDOSCatalogEntry &e = entries[static_cast<size_t>(i)];
    std::vector<uint8_t> data(e.eof);
    const int read = ProDOS::readFile(disk.data(), disk.size(), &e, data.data(), static_cast<int>(data.size()));
    if (read != static_cast<int>(e.eof)) return std::nullopt;
    type = e.fileType;
    aux = e.auxType;
    return data;
  }
  return std::nullopt;
}

} // namespace

std::optional<std::vector<uint8_t>> makeBootVolume(const std::vector<uint8_t> &systemDisk, const Program &program,
                                                   const std::string &fileName, int blocks, std::string &error) {
  if (!ProDOS::isProDOS(systemDisk.data(), systemDisk.size())) {
    error = "The system disk is not a ProDOS volume.";
    return std::nullopt;
  }
  if (systemDisk.size() < 2 * BLOCK || blocks < 280 || blocks > 65535) {
    error = "The volume's size is not one ProDOS can have.";
    return std::nullopt;
  }
  uint8_t type = 0;
  uint16_t aux = 0;
  const auto prodos = fileFrom(systemDisk, "PRODOS", type, aux);
  if (!prodos) {
    error = "The system disk has no PRODOS file.";
    return std::nullopt;
  }
  std::vector<uint8_t> volume = blankVolume(systemDisk, blocks, "APPLEM");
  auto add = [&](const std::string &name, uint8_t fileType, uint16_t auxType, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> bytes = data;
    const FsWriteStatus status = ProDOS::writeFile(volume.data(), volume.size(), name.c_str(), fileType, auxType,
                                                   bytes.data(), static_cast<uint32_t>(bytes.size()));
    if (status != FsWriteStatus::OK) error = "Could not write " + name + " to the boot volume.";
    return status == FsWriteStatus::OK;
  };
  if (!add("PRODOS", type, aux, *prodos)) return std::nullopt;

  if (program.fileType == 0xFF) {
    // A SYSTEM program is run by ProDOS itself, as the first SYSTEM file,
    // whose name has to end ".SYSTEM".
    std::string name = fileName;
    const std::string suffix = ".SYSTEM";
    if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
      name = name.substr(0, 15 - suffix.size()) + suffix;
    }
    return add(name, 0xFF, program.auxType ? program.auxType : 0x2000, program.bytes) ? std::optional(volume)
                                                                                      : std::nullopt;
  }

  // Anything else is run by BASIC.SYSTEM: named in its startup buffer, and in
  // a STARTUP program too for one with no buffer. "-" runs a BIN file as BRUN
  // does, or a SYSTEM file as ProDOS would.
  const auto basic = fileFrom(systemDisk, "BASIC.SYSTEM", type, aux);
  if (!basic) {
    error = "The system disk has no BASIC.SYSTEM to run a BIN program with.";
    return std::nullopt;
  }
  // The system program startup protocol (ProDOS 8 Technical Reference,
  // 5.1.5.1): a SYSTEM file with $EE $EE at bytes 3 and 4 takes the name of a
  // program to run in the buffer at byte 6, its length at byte 5. BASIC.SYSTEM
  // runs a name left there instead of STARTUP, and it has to: launched by
  // ProDOS 2.4 as the first SYSTEM file, it did not run STARTUP at all.
  std::vector<uint8_t> basicSystem = *basic;
  if (basicSystem.size() > 6 + fileName.size() && basicSystem[3] == 0xEE && basicSystem[4] == 0xEE &&
      basicSystem[5] > fileName.size()) {
    basicSystem[6] = static_cast<uint8_t>(fileName.size());
    std::memcpy(&basicSystem[7], fileName.data(), fileName.size());
  }
  if (!add("BASIC.SYSTEM", type, aux, basicSystem)) return std::nullopt;
  const std::string startup = "10 PRINT CHR$(4);\"-" + fileName + "\"\n";
  const BasicProgramImage image = tokenizeBasicProgram(startup.c_str());
  if (!add("STARTUP", 0xFC, 0x0801, image.bytes)) return std::nullopt;
  if (!add(fileName, program.fileType, program.load, program.bytes)) return std::nullopt;
  return volume;
}

} // namespace a2e::native::dev
