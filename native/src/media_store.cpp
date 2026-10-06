/*
 * media_store.cpp - Disk images the app keeps: in the drives, and recent
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "media_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace a2e::native {

uint32_t fingerprint(const uint8_t *data, size_t size) {
  uint32_t hash = 0x811c9dc5u;
  for (size_t i = 0; i < size; i++) {
    hash ^= data[i];
    hash *= 0x01000193u;
  }
  return hash;
}

std::optional<std::vector<uint8_t>> readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return bytes;
}

// Written beside the destination and renamed over it, so a failure never
// leaves half a file where an image was.
bool writeFile(const std::string &path, const uint8_t *data, size_t size) {
  const std::string temporary = path + ".tmp";
  std::error_code error;
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    out.close();
    // A full disk leaves a partial file, which must not be left behind.
    if (!out) {
      fs::remove(temporary, error);
      return false;
    }
  }
  fs::rename(temporary, path, error);
  if (error) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
  }
  return !error;
}

std::string baseName(const std::string &path) {
  return fs::path(path).filename().string();
}

MediaStore::MediaStore(std::string directory, std::string kind)
    : directory_(std::move(directory)), kind_(std::move(kind)) {
  std::error_code error;
  fs::create_directories(fs::path(directory_) / kind_, error);
}

std::string MediaStore::unitPath(int unit, const std::string &suffix) const {
  return (fs::path(directory_) / kind_ / ("unit-" + std::to_string(unit) + suffix)).string();
}

std::string MediaStore::indexPath(int unit) const { return unitPath(unit, "-recent.txt"); }

void MediaStore::saveInserted(int unit, const std::string &filename, const std::vector<uint8_t> &data,
                              const std::string &path) {
  std::error_code error;
  if (!path.empty()) {
    fs::remove(unitPath(unit, ".img"), error);
    std::ofstream(unitPath(unit, ".path"), std::ios::trunc) << path;
  } else {
    fs::remove(unitPath(unit, ".path"), error);
    if (!writeFile(unitPath(unit, ".img"), data.data(), data.size())) return;
  }
  std::ofstream(unitPath(unit, ".name"), std::ios::trunc) << filename;
}

std::optional<StoredImage> MediaStore::loadInserted(int unit) const {
  std::string name;
  std::ifstream names(unitPath(unit, ".name"));
  std::getline(names, name);
  if (name.empty()) return std::nullopt;
  std::string path;
  std::ifstream paths(unitPath(unit, ".path"));
  std::getline(paths, path);
  if (!path.empty()) {
    auto data = readFile(path);
    if (!data || data->empty()) return StoredImage{name, {}, path, true};
    return StoredImage{name, std::move(*data), path};
  }
  auto data = readFile(unitPath(unit, ".img"));
  if (!data || data->empty()) return std::nullopt;
  return StoredImage{name, std::move(*data)};
}

void MediaStore::clearInserted(int unit) {
  std::error_code error;
  fs::remove(unitPath(unit, ".img"), error);
  fs::remove(unitPath(unit, ".path"), error);
  fs::remove(unitPath(unit, ".name"), error);
}

// One entry per line: accessed time, stored file, filename and path, tab
// separated. The stored file is empty for an entry with a path; the path is
// missing from lines written before there were paths.
std::vector<RecentEntry> MediaStore::recent(int unit) const {
  std::vector<RecentEntry> entries;
  std::ifstream in(indexPath(unit));
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    RecentEntry entry;
    std::string accessed;
    if (!std::getline(fields, accessed, '\t') || !std::getline(fields, entry.file, '\t') ||
        !std::getline(fields, entry.filename, '\t')) {
      continue;
    }
    std::getline(fields, entry.path);
    try {
      entry.accessed = std::stoll(accessed);
    } catch (...) {
      continue;
    }
    // A path is kept even while its file is away, as a Mac's recent items
    // are; choosing it then says so.
    if (!entry.path.empty() || (!entry.file.empty() && fs::exists(fs::path(directory_) / kind_ / entry.file))) {
      entries.push_back(entry);
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const RecentEntry &a, const RecentEntry &b) { return a.accessed > b.accessed; });
  return entries;
}

void MediaStore::writeIndex(int unit, const std::vector<RecentEntry> &entries) const {
  std::ostringstream out;
  for (const RecentEntry &entry : entries) {
    out << entry.accessed << '\t' << entry.file << '\t' << entry.filename << '\t' << entry.path << '\n';
  }
  const std::string text = out.str();
  writeFile(indexPath(unit), reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

void MediaStore::addRecent(int unit, const std::string &filename, const std::vector<uint8_t> &data,
                           const std::string &path) {
  std::vector<RecentEntry> entries = recent(unit);
  // One entry per file: by its path when it has one, else by its name. The
  // newer replaces the older, and a copy it held goes with it.
  std::error_code error;
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const RecentEntry &e) {
                                 const bool same = path.empty() ? e.path.empty() && e.filename == filename
                                                                : e.path == path;
                                 if (same && !e.file.empty()) fs::remove(fs::path(directory_) / kind_ / e.file, error);
                                 return same;
                               }),
                entries.end());

  RecentEntry entry;
  entry.filename = filename;
  entry.path = path;
  entry.accessed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  if (path.empty()) {
    char hash[16];
    std::snprintf(hash, sizeof(hash), "%08x",
                  fingerprint(reinterpret_cast<const uint8_t *>(filename.data()), filename.size()));
    entry.file = "unit-" + std::to_string(unit) + "-recent-" + hash + ".img";
    if (!writeFile((fs::path(directory_) / kind_ / entry.file).string(), data.data(), data.size())) return;
  }
  entries.insert(entries.begin(), entry);

  // The oldest beyond the limit go, with any copy they held.
  while (entries.size() > MAX_RECENT) {
    if (!entries.back().file.empty()) fs::remove(fs::path(directory_) / kind_ / entries.back().file, error);
    entries.pop_back();
  }
  writeIndex(unit, entries);
}

std::optional<StoredImage> MediaStore::loadRecent(int unit, const RecentEntry &entry) const {
  (void)unit;
  if (!entry.path.empty()) {
    auto data = readFile(entry.path);
    if (!data || data->empty()) return std::nullopt;
    return StoredImage{entry.filename, std::move(*data), entry.path};
  }
  auto data = readFile((fs::path(directory_) / kind_ / entry.file).string());
  if (!data) return std::nullopt;
  return StoredImage{entry.filename, std::move(*data)};
}

void MediaStore::clearRecent(int unit) {
  for (const RecentEntry &entry : recent(unit)) {
    std::error_code error;
    if (!entry.file.empty()) fs::remove(fs::path(directory_) / kind_ / entry.file, error);
  }
  std::error_code error;
  fs::remove(indexPath(unit), error);
}

// library.json is a flat array of objects whose values are all strings. A
// full JSON parser would be more than this file needs.
std::vector<LibraryEntry> parseLibrary(const std::string &json) {
  std::vector<LibraryEntry> entries;
  const std::regex object(R"(\{([^{}]*)\})");
  const std::regex field(R"re("(\w+)"\s*:\s*"((?:[^"\\]|\\.)*)")re");
  for (auto it = std::sregex_iterator(json.begin(), json.end(), object);
       it != std::sregex_iterator(); ++it) {
    const std::string body = (*it)[1];
    LibraryEntry entry;
    for (auto f = std::sregex_iterator(body.begin(), body.end(), field);
         f != std::sregex_iterator(); ++f) {
      const std::string key = (*f)[1];
      const std::string value = (*f)[2];
      if (key == "id") entry.id = value;
      else if (key == "name") entry.name = value;
      else if (key == "file") entry.file = value;
      else if (key == "type") entry.type = value;
      else if (key == "description") entry.description = value;
    }
    if (!entry.name.empty() && !entry.file.empty() && !entry.type.empty()) entries.push_back(entry);
  }
  return entries;
}

} // namespace a2e::native
