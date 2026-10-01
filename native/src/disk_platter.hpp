/*
 * disk_platter.hpp - A floppy's recording, painted as the disk it is on
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "disk_inspector_data.hpp"

#include <cstdint>
#include <vector>

namespace a2e::native {

// The browser's Disk Inspector platter (disk-inspector-window.js): the disk
// as a fraction of its radius, with all 160 quarter tracks in the band
// between BAND_OUTER and BAND_INNER, track 0 outermost.
namespace platter {
constexpr float DISK_EDGE = 0.985f;
constexpr float BAND_OUTER = 0.95f;
constexpr float BAND_INNER = 0.36f;
constexpr float HUB_RING_OUTER = 0.25f;
constexpr float HUB_HOLE = 0.19f;
constexpr float INDEX_HOLE_RADIUS = 0.3f;
constexpr int QUARTER_TRACKS = inspect::QUARTER_TRACKS;
constexpr float RING_WIDTH = (BAND_OUTER - BAND_INNER) / QUARTER_TRACKS;

// The quarter track at a radius, or -1 outside the band.
int quarterTrackAt(float radius);
// The middle of a quarter track's ring.
float radiusOf(int quarterTrack);
} // namespace platter

enum class PlatterMode { Structure, Timing };

// What each kind of nibble is drawn as, from the rainbow's six colours: the
// address field blue and its marks yellow, the data field green and its
// marks orange, red for a failed checksum, purple for a well-formed nibble
// in no standard field, sync as a quiet tint of the medium and noise grey.
// RGB, packed 0xRRGGBB.
uint32_t kindColour(uint8_t kind);
// A flux cell: blue written fast, orange slow, neutral on time; 0 for none.
uint32_t timeColour(uint8_t time);
constexpr uint32_t MEDIUM_COLOUR = 0x1a1308;
constexpr uint32_t HUB_COLOUR = 0xd8d4c8;

// Paint the disk into `rgba` (size x size, rows from the top), angle 0 at
// twelve o'clock and running clockwise, with the index hole there. Outside
// the disk and inside the hub hole is transparent. Without an overview the
// disk is blank medium.
//
// Every pixel finds its quarter track from its radius and its place round
// the track from its angle. A quarter track with nothing recorded beside one
// that has data takes its neighbour's, faded: the head reads a track from
// the quarter track either side, and a disk recorded on half tracks would
// otherwise look nearly empty.
void paintPlatter(std::vector<uint8_t> &rgba, int size, const Overview *overview, PlatterMode mode);

} // namespace a2e::native
