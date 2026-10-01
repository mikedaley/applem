/*
 * disk_platter.cpp - A floppy's recording, painted as the disk it is on
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "disk_platter.hpp"

#include <algorithm>
#include <cmath>

namespace a2e::native {

namespace platter {

int quarterTrackAt(float radius) {
  if (radius > BAND_OUTER || radius < BAND_INNER) return -1;
  return std::min(QUARTER_TRACKS - 1, static_cast<int>((BAND_OUTER - radius) / RING_WIDTH));
}

float radiusOf(int quarterTrack) { return BAND_OUTER - (quarterTrack + 0.5f) * RING_WIDTH; }

} // namespace platter

namespace {

constexpr uint32_t BLUE = 0x009ddc;
constexpr uint32_t GREEN = 0x61bb46;
constexpr uint32_t YELLOW = 0xfdb827;
constexpr uint32_t ORANGE = 0xf5821f;
constexpr uint32_t RED = 0xe03a3e;
constexpr uint32_t PURPLE = 0x963d97;
constexpr uint32_t MUTED = 0x8b949e;
// How far off nominal a cell must be to take the full colour.
constexpr double TIMING_RANGE = 0.05;

uint32_t mix(uint32_t a, uint32_t b, double t) {
  t = std::clamp(t, 0.0, 1.0);
  auto channel = [&](int shift) {
    const double x = (a >> shift) & 0xFF;
    const double y = (b >> shift) & 0xFF;
    return static_cast<uint32_t>(std::lround(x + (y - x) * t)) << shift;
  };
  return channel(16) | channel(8) | channel(0);
}

const uint32_t SYNC = mix(MEDIUM_COLOUR, MUTED, 0.28);
const uint32_t NEUTRAL = mix(MEDIUM_COLOUR, MUTED, 0.55);

} // namespace

uint32_t kindColour(uint8_t kind) {
  if (kind & inspect::BAD) return RED;
  switch (kind & inspect::KIND_MASK) {
  case inspect::SYNC: return SYNC;
  case inspect::ADDR_PROLOGUE:
  case inspect::ADDR_EPILOGUE: return YELLOW;
  case inspect::ADDR: return BLUE;
  case inspect::DATA_PROLOGUE:
  case inspect::DATA_EPILOGUE: return ORANGE;
  case inspect::DATA: return GREEN;
  case inspect::OTHER: return PURPLE;
  case inspect::INVALID: return MUTED;
  default: return MEDIUM_COLOUR;
  }
}

uint32_t timeColour(uint8_t time) {
  if (!time) return 0;
  const double d = std::clamp(cellDeviation(time) / TIMING_RANGE, -1.0, 1.0);
  return d < 0 ? mix(NEUTRAL, BLUE, -d) : mix(NEUTRAL, ORANGE, d);
}

void paintPlatter(std::vector<uint8_t> &rgba, int size, const Overview *overview, PlatterMode mode,
                  const PlatterView &view, const PlatterRings *rings) {
  using namespace platter;
  rgba.assign(static_cast<size_t>(size) * size * 4, 0);
  const double half = size / 2.0;
  const auto *tracks = overview && !overview->tracks.empty() ? &overview->tracks : nullptr;
  const int buckets = overview ? overview->buckets : 0;
  const bool timing = mode == PlatterMode::Timing;
  const double zoom = std::max(1.0, view.zoom);
  const double pixel = 1.0 / (half * zoom); // one pixel, as a fraction of the radius
  // The index hole sits at the disk's angle 0, wherever the view has turned it.
  const double holeAngle = -view.turn * 2 * M_PI;
  const double holeX = std::sin(holeAngle) * INDEX_HOLE_RADIUS;
  const double holeY = -std::cos(holeAngle) * INDEX_HOLE_RADIUS;
  constexpr double HOLE = 0.018;
  // Noise that belongs to the place on the disk, so it holds still as the
  // view moves.
  auto noise = [](uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (h & 0xFFFF) / 65535.0;
  };

  for (int y = 0; y < size; y++) {
    const double dy = (y + 0.5 - half) / half / zoom + view.cy;
    for (int x = 0; x < size; x++) {
      const double dx = (x + 0.5 - half) / half / zoom + view.cx;
      const double r = std::sqrt(dx * dx + dy * dy);
      // Smooth edges at the rim and the hub hole.
      const double coverage = std::clamp((DISK_EDGE - r) / pixel, 0.0, 1.0) *
                              std::clamp((r - HUB_HOLE) / pixel, 0.0, 1.0);
      if (coverage <= 0) continue;

      uint32_t rgb = MEDIUM_COLOUR;
      double shade = 1.0;
      if (r > BAND_OUTER || r < BAND_INNER) {
        rgb = r < HUB_RING_OUTER ? HUB_COLOUR : MEDIUM_COLOUR;
        if (r > BAND_OUTER) shade = 0.85;
      } else if (tracks) {
        const double position = (BAND_OUTER - r) / RING_WIDTH;
        int qt = std::min(QUARTER_TRACKS - 1, static_cast<int>(position));
        bool bleed = false;
        if (!(*tracks)[qt].present) {
          const int near = qt > 0 && (*tracks)[qt - 1].present                    ? qt - 1
                           : qt + 1 < QUARTER_TRACKS && (*tracks)[qt + 1].present ? qt + 1
                                                                                  : -1;
          if (near >= 0) {
            qt = near;
            bleed = true;
          }
        }
        const OverviewTrack &t = (*tracks)[qt];
        if (t.present && buckets > 0) {
          // Angle from twelve o'clock, clockwise, as a fraction of a turn.
          double a = std::atan2(dx, -dy) / (2 * M_PI) + view.turn;
          a -= std::floor(a);
          const Ring *ring = rings ? (*rings)[qt] : nullptr;
          uint8_t kind;
          uint8_t time = 0;
          uint32_t place;
          bool transition = false;
          if (ring && ring->track.present && !ring->cellKinds.empty()) {
            const uint32_t cells = static_cast<uint32_t>(ring->cellKinds.size());
            const uint32_t cell = std::min(cells - 1, static_cast<uint32_t>(a * cells));
            kind = ring->cellKinds[cell];
            if (cell < ring->track.cellTime.size()) time = ring->track.cellTime[cell];
            place = cell;
            // Once a cell is wide enough to see, the ones in it show.
            const double cellPixels = 2 * M_PI * r / cells * half * zoom;
            transition = cellPixels >= 2.5 && cellBit(ring->track.bits, cell);
          } else {
            const int b = std::min(buckets - 1, static_cast<int>(a * buckets));
            kind = t.kinds[b];
            time = t.times[b];
            place = static_cast<uint32_t>(b);
          }
          if (timing) {
            const uint32_t tc = t.flux ? timeColour(time) : 0;
            rgb = tc ? tc : mix(MEDIUM_COLOUR, kindColour(kind), 0.22);
          } else if ((kind & inspect::KIND_MASK) == inspect::INVALID && !(kind & inspect::BAD)) {
            rgb = mix(MEDIUM_COLOUR, MUTED, 0.25 + noise(qt, place) * 0.6);
          } else {
            rgb = kindColour(kind);
          }
          if (transition) rgb = mix(rgb, 0xffffff, 0.55);
          if (bleed && rgb != MEDIUM_COLOUR) rgb = mix(MEDIUM_COLOUR, rgb, 0.45);
        }
        // A groove between whole tracks, so the rings read as tracks.
        const double inTrack = std::fmod(position / 4.0, 1.0);
        const double groove = std::max(0.03, pixel / RING_WIDTH / 4.0);
        if (inTrack < groove || inTrack > 1.0 - groove * 0.5) shade = 0.62;
      }
      // The index hole.
      const double hole = std::hypot(dx - holeX, dy - holeY);
      if (hole < HOLE + pixel) shade *= 1.0 - 0.85 * std::clamp((HOLE - hole) / pixel + 0.5, 0.0, 1.0);

      uint8_t *p = &rgba[(static_cast<size_t>(y) * size + x) * 4];
      p[0] = static_cast<uint8_t>(((rgb >> 16) & 0xFF) * shade);
      p[1] = static_cast<uint8_t>(((rgb >> 8) & 0xFF) * shade);
      p[2] = static_cast<uint8_t>((rgb & 0xFF) * shade);
      p[3] = static_cast<uint8_t>(std::lround(255 * coverage));
    }
  }
}

} // namespace a2e::native
