/*
 * volume_map.hpp - What a SmartPort image holds, for drawing
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace a2e::native {

// A ProDOS volume read off its blocks: the volume directory's header in
// block 2 and the free-block bitmap it points at. A volume ProDOS would not
// recognise reads as not ProDOS, with only its size.
struct VolumeSummary {
  bool prodos = false;
  std::string name;         // "/HARD1"
  uint32_t totalBlocks = 0; // as the volume says, and never past the image
  uint32_t freeBlocks = 0;
  int entries = 0;          // in the volume directory
  // How full each part of the volume is, 0 to 1, in `cells` equal runs of
  // blocks from block 0. Empty when the volume is not ProDOS.
  std::vector<float> used;
};

// `blocks` is the image's block data in ProDOS order, without a 2IMG header.
VolumeSummary summariseVolume(const uint8_t *blocks, size_t size, int cells);

// The cell a block falls in, for `cells` equal runs of `totalBlocks`.
int cellForBlock(uint32_t block, uint32_t totalBlocks, int cells);

// The first block of a cell and one past its last.
uint32_t cellFirstBlock(int cell, uint32_t totalBlocks, int cells);

} // namespace a2e::native
