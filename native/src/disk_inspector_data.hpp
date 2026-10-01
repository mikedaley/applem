/*
 * disk_inspector_data.hpp - What is recorded on a floppy, for drawing
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "disk-image/disk_inspection.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

// The core's analyser reads a disk the way the drive's latch does and names
// every nibble (core/disk-image/disk_inspection.*). The whole disk comes
// through its overview buffer, the one the browser's Disk Inspector reads,
// so the two builds share one description of it; a single track is read
// straight from the analyser, since nothing here needs it as bytes.

// One quarter track of the overview: `buckets` equal arcs, each carrying the
// kind of most of its cells, the sector they belong to, and on a flux track
// how long its cells took.
struct OverviewTrack {
  bool present = false;
  bool flux = false;
  bool thirteenSector = false;
  int trackId = -1; // quarter tracks reading one stored track share it
  int sectorsFound = 0;
  int sectorsGood = 0;
  int addressBad = 0;
  int dataBad = 0;
  uint32_t bitCount = 0;
  std::vector<uint8_t> kinds;
  std::vector<uint8_t> sectors;
  std::vector<uint8_t> times;
};

struct Overview {
  int buckets = 0;
  std::vector<OverviewTrack> tracks; // 160, one per quarter track
};

// The buffer from inspect::buildOverview, or nothing if it is not one this
// reads (a tag or version it does not know, or short).
std::optional<Overview> parseOverview(const std::vector<uint8_t> &bytes);

// The whole disk at a glance. Quarter tracks that read one stored track are
// counted once, which is what a person means by "how many tracks".
struct DiskSummary {
  int tracks = 0;
  int sectors = 0;
  int good = 0;
  int bad = 0;
  int fluxTracks = 0;
  int nonStandardTracks = 0;
  std::string format = "Empty"; // "16 sector", "13 sector", "Non-standard"…
};
DiskSummary summariseDisk(const Overview &overview);

// One quarter track in full: every nibble round it and the sectors in the
// order they pass the head, with how long each cell took on a flux track.
struct TrackDetail {
  int quarterTrack = -1;
  bool present = false;
  bool flux = false;
  int trackId = -1;
  inspect::TrackAnalysis analysis;
  std::vector<uint8_t> cellTime; // per cell; empty unless flux
};
TrackDetail readTrackDetail(DiskImage &image, int quarterTrack);

// 17, 17.25, 17.5, 17.75.
std::string trackLabel(int quarterTrack);
// "Data field", "Address prologue (bad checksum)"…
std::string kindName(uint8_t kind);
// A nominal cell in the core's timing unit (quarters of 125ns), and how far
// a cell's time is from it: -0.05 is a cell written 5% fast.
constexpr double NOMINAL_CELL_TIME = (4.0 * 8 * 176) / 45;
inline double cellDeviation(uint8_t time) { return time / NOMINAL_CELL_TIME - 1.0; }

// The nibble holding a cell, by binary search over the nibbles' starts; the
// cells before the first nibble belong to the last one, round the end.
int nibbleAtCell(const std::vector<inspect::Nibble> &nibbles, uint32_t cell);

} // namespace a2e::native
