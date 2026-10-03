/*
 * disk_inspector_data.cpp - What is recorded on a floppy, for drawing
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "disk_inspector_data.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace a2e::native {

namespace {

uint16_t u16(const uint8_t *at) { return static_cast<uint16_t>(at[0] | (at[1] << 8)); }
uint32_t u32(const uint8_t *at) {
  return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
         (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

// The overview's per-track flags.
constexpr uint8_t FLAG_PRESENT = 1;
constexpr uint8_t FLAG_FLUX = 2;
constexpr uint8_t FLAG_THIRTEEN = 4;

} // namespace

std::optional<Overview> parseOverview(const std::vector<uint8_t> &bytes) {
  if (bytes.size() < 16 || std::memcmp(bytes.data(), "DINS", 4) != 0) return std::nullopt;
  if (u16(&bytes[4]) != inspect::OVERVIEW_VERSION) return std::nullopt;
  const int count = u16(&bytes[6]);
  const int buckets = u16(&bytes[8]);
  const size_t record = u16(&bytes[10]);
  if (record != static_cast<size_t>(12 + buckets * 3) || bytes.size() < 16 + count * record) {
    return std::nullopt;
  }

  Overview overview;
  overview.buckets = buckets;
  overview.tracks.resize(count);
  for (int qt = 0; qt < count; qt++) {
    const uint8_t *at = &bytes[16 + qt * record];
    OverviewTrack &t = overview.tracks[qt];
    t.present = at[0] & FLAG_PRESENT;
    t.flux = at[0] & FLAG_FLUX;
    t.thirteenSector = at[0] & FLAG_THIRTEEN;
    t.trackId = at[1] == 0xFF ? -1 : at[1];
    t.sectorsFound = at[2];
    t.sectorsGood = at[3];
    t.addressBad = at[4];
    t.dataBad = at[5];
    t.bitCount = u32(at + 8);
    t.kinds.assign(at + 12, at + 12 + buckets);
    t.sectors.assign(at + 12 + buckets, at + 12 + buckets * 2);
    t.times.assign(at + 12 + buckets * 2, at + 12 + buckets * 3);
  }
  return overview;
}

DiskSummary summariseDisk(const Overview &overview) {
  DiskSummary summary;
  std::set<int> seen;
  bool thirteen = false;
  bool sixteen = false;
  for (const OverviewTrack &t : overview.tracks) {
    if (!t.present || !seen.insert(t.trackId).second) continue;
    summary.tracks++;
    summary.sectors += t.sectorsFound;
    summary.good += t.sectorsGood;
    summary.bad += t.addressBad + t.dataBad;
    if (t.flux) summary.fluxTracks++;
    if (t.sectorsFound == 0) summary.nonStandardTracks++;
    if (t.thirteenSector) thirteen = true;
    else if (t.sectorsFound > 0) sixteen = true;
  }
  if (summary.tracks == 0) summary.format = "Empty";
  else if (summary.sectors == 0) summary.format = "Unknown format";
  else if (thirteen && sixteen) summary.format = "13 and 16 sector";
  else if (thirteen) summary.format = "13 sector";
  else summary.format = "16 sector";
  return summary;
}

TrackDetail readTrackDetail(DiskImage &image, int quarterTrack, inspect::Recording recording) {
  TrackDetail detail;
  detail.quarterTrack = quarterTrack;
  DiskImage::TrackView view;
  if (!image.inspectQuarterTrack(quarterTrack, view) || view.bit_count == 0) return detail;
  detail.present = true;
  detail.flux = view.flux;
  detail.trackId = view.track_id;
  detail.analysis = inspect::analyzeTrack(view.bits.data(), view.bit_count, recording);
  detail.bits = std::move(view.bits);
  detail.cellTime = std::move(view.cell_time);
  return detail;
}

Ring makeRing(TrackDetail track) {
  Ring ring;
  const uint32_t cells = track.analysis.bit_count;
  ring.cellKinds.assign(cells, inspect::NONE);
  for (const inspect::Nibble &n : track.analysis.nibbles) {
    for (uint32_t c = 0; c < n.cells; c++) ring.cellKinds[(n.start_bit + c) % cells] = n.kind;
  }
  ring.track = std::move(track);
  return ring;
}

std::string trackLabel(int quarterTrack) {
  static const char *const parts[] = {"", ".25", ".5", ".75"};
  return std::to_string(quarterTrack / 4) + parts[quarterTrack % 4];
}

std::string kindName(uint8_t kind) {
  static const char *const names[] = {"Unread",        "Sync",          "Address prologue",
                                      "Address field", "Address epilogue", "Data prologue",
                                      "Data field",    "Data epilogue", "Unknown",
                                      "Noise"};
  const int k = kind & inspect::KIND_MASK;
  std::string name = k < inspect::KIND_COUNT ? names[k] : "Unknown";
  if (kind & inspect::BAD) name += " (bad checksum)";
  return name;
}

int nibbleAtCell(const std::vector<inspect::Nibble> &nibbles, uint32_t cell) {
  if (nibbles.empty()) return -1;
  // The last nibble starting at or before the cell.
  auto after = std::upper_bound(nibbles.begin(), nibbles.end(), cell,
                                [](uint32_t c, const inspect::Nibble &n) { return c < n.start_bit; });
  if (after == nibbles.begin()) return static_cast<int>(nibbles.size()) - 1;
  return static_cast<int>(after - nibbles.begin()) - 1;
}

} // namespace a2e::native
