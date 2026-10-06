/*
 * state_store.cpp - Save states kept on disk: an autosave per machine, five slots
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "state_store.hpp"

#include "media_store.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace a2e::native {

namespace {

uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

} // namespace

std::optional<StateHeader> readStateHeader(const uint8_t *data, size_t size) {
  if (!data || size < 12 || le32(data) != STATE_MAGIC) return std::nullopt;
  return StateHeader{le32(data + 4), le32(data + 8)};
}

std::vector<uint8_t> makeThumbnail(const uint8_t *rgba, int width, int height) {
  std::vector<uint8_t> thumb(static_cast<size_t>(THUMB_WIDTH) * THUMB_HEIGHT * 4, 0);
  if (!rgba || width <= 0 || height <= 0) return thumb;
  for (int ty = 0; ty < THUMB_HEIGHT; ty++) {
    const int y0 = ty * height / THUMB_HEIGHT;
    const int y1 = std::max(y0 + 1, (ty + 1) * height / THUMB_HEIGHT);
    for (int tx = 0; tx < THUMB_WIDTH; tx++) {
      const int x0 = tx * width / THUMB_WIDTH;
      const int x1 = std::max(x0 + 1, (tx + 1) * width / THUMB_WIDTH);
      uint32_t sum[3] = {0, 0, 0};
      int count = 0;
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          const uint8_t *p = rgba + (static_cast<size_t>(y) * width + x) * 4;
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          count++;
        }
      }
      uint8_t *out = &thumb[(static_cast<size_t>(ty) * THUMB_WIDTH + tx) * 4];
      out[0] = static_cast<uint8_t>(sum[0] / count);
      out[1] = static_cast<uint8_t>(sum[1] / count);
      out[2] = static_cast<uint8_t>(sum[2] / count);
      out[3] = 255;
    }
  }
  return thumb;
}

StateStore::StateStore(std::string directory) : directory_(std::move(directory)) {
  std::error_code error;
  fs::create_directories(directory_, error);
}

std::string StateStore::path(const std::string &id, const char *extension) const {
  return (fs::path(directory_) / (id + extension)).string();
}

bool StateStore::save(const std::string &id, const std::string &machine,
                      const std::vector<uint8_t> &state, const std::vector<uint8_t> &thumbnail) {
  if (!writeFile(path(id, ".a2state"), state.data(), state.size())) return false;
  writeFile(path(id, ".thumb"), thumbnail.data(), thumbnail.size());
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::string meta = "machine=" + machine + "\nsavedAt=" + std::to_string(now) + "\n";
  return writeFile(path(id, ".meta"), reinterpret_cast<const uint8_t *>(meta.data()), meta.size());
}

std::optional<StateRecord> StateStore::info(const std::string &id) const {
  if (!fs::exists(path(id, ".a2state"))) return std::nullopt;
  StateRecord record;
  record.id = id;
  std::ifstream meta(path(id, ".meta"));
  std::string line;
  while (std::getline(meta, line)) {
    if (line.rfind("machine=", 0) == 0) record.machine = line.substr(8);
    else if (line.rfind("savedAt=", 0) == 0) {
      try {
        record.savedAt = std::stoll(line.substr(8));
      } catch (...) {
      }
    }
  }
  if (auto thumb = readFile(path(id, ".thumb"));
      thumb && thumb->size() == static_cast<size_t>(THUMB_WIDTH) * THUMB_HEIGHT * 4) {
    record.thumbnail = std::move(*thumb);
  }
  return record;
}

std::optional<std::vector<uint8_t>> StateStore::load(const std::string &id) const {
  return readFile(path(id, ".a2state"));
}

void StateStore::clear(const std::string &id) {
  std::error_code error;
  for (const char *extension : {".a2state", ".meta", ".thumb"}) fs::remove(path(id, extension), error);
}

bool StateStore::copy(const std::string &from, const std::string &to) {
  std::error_code error;
  if (!fs::exists(path(from, ".a2state"), error)) return false;
  clear(to);
  for (const char *extension : {".a2state", ".meta", ".thumb"}) {
    if (fs::exists(path(from, extension), error)) {
      fs::copy_file(path(from, extension), path(to, extension), fs::copy_options::overwrite_existing, error);
    }
  }
  return !error;
}

} // namespace a2e::native
