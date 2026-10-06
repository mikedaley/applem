/*
 * state_store.hpp - Save states kept on disk: an autosave per machine, five slots
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

// What every machine's state begins with (src/js/state/state-header.js):
// twelve bytes, little-endian, the magic "A2ES" then a format version then
// the machine id. Everything after is laid out to the saving machine's
// shape, which is why a state restores only into the machine that wrote it.
struct StateHeader {
  uint32_t version = 0;
  uint32_t machineId = 0;
};
constexpr uint32_t STATE_MAGIC = 0x53324541;
std::optional<StateHeader> readStateHeader(const uint8_t *data, size_t size);

// The thumbnail the window shows, as the browser makes it: 140x96.
constexpr int THUMB_WIDTH = 140;
constexpr int THUMB_HEIGHT = 96;
// Box-filtered down from a frame of any size, so a IIgs's 736x448 and a
// //e's 560x384 both come out whole.
std::vector<uint8_t> makeThumbnail(const uint8_t *rgba, int width, int height);

struct StateRecord {
  std::string id;      // "autosave-<machine>" or "slot-N"
  std::string machine; // the key of the machine that wrote it
  int64_t savedAt = 0; // milliseconds since the epoch
  std::vector<uint8_t> thumbnail; // THUMB_WIDTH x THUMB_HEIGHT RGBA, or empty
};

// The browser's records (state-persistence.js), as files: each record is
// <id>.a2state with <id>.meta beside it (machine and time) and <id>.thumb.
// The five slots are shared by every machine and each says which machine
// filled it; the autosave is one per machine, because a state restores only
// into the machine that wrote it.
class StateStore {
public:
  static constexpr int SLOTS = 5;

  explicit StateStore(std::string directory);

  static std::string slotId(int slot) { return "slot-" + std::to_string(slot); }
  static std::string autosaveId(const std::string &machine) { return "autosave-" + machine; }
  // The autosave a session started with, kept aside before the session's own
  // first autosave replaces it: the one written at the last quit.
  static std::string lastSessionId(const std::string &machine) { return "last-session-" + machine; }

  bool save(const std::string &id, const std::string &machine, const std::vector<uint8_t> &state,
            const std::vector<uint8_t> &thumbnail);
  std::optional<StateRecord> info(const std::string &id) const;
  std::optional<std::vector<uint8_t>> load(const std::string &id) const;
  void clear(const std::string &id);
  // A record copied under another id, replacing what was there. False if
  // there was nothing to copy.
  bool copy(const std::string &from, const std::string &to);

private:
  std::string path(const std::string &id, const char *extension) const;
  std::string directory_;
};

} // namespace a2e::native
