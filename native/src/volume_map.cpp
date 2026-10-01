/*
 * volume_map.cpp - What a SmartPort image holds, for drawing
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "volume_map.hpp"

#include <algorithm>

namespace a2e::native {

namespace {

constexpr size_t BLOCK = 512;
constexpr size_t VOLUME_DIRECTORY = 2;
// The volume directory header, from the start of its block.
constexpr size_t STORAGE_AND_LENGTH = 0x04;
constexpr size_t NAME = 0x05;
constexpr size_t ENTRY_LENGTH = 0x23;
constexpr size_t ENTRIES_PER_BLOCK = 0x24;
constexpr size_t FILE_COUNT = 0x25;
constexpr size_t BITMAP_POINTER = 0x27;
constexpr size_t TOTAL_BLOCKS = 0x29;

uint16_t word(const uint8_t *at) { return static_cast<uint16_t>(at[0] | (at[1] << 8)); }

} // namespace

int cellForBlock(uint32_t block, uint32_t totalBlocks, int cells) {
  if (totalBlocks == 0 || cells <= 0) return 0;
  const uint64_t cell = static_cast<uint64_t>(std::min(block, totalBlocks - 1)) * cells / totalBlocks;
  return static_cast<int>(cell);
}

uint32_t cellFirstBlock(int cell, uint32_t totalBlocks, int cells) {
  if (cells <= 0) return 0;
  // The first block whose cell is `cell`: ceil(cell * total / cells).
  return static_cast<uint32_t>((static_cast<uint64_t>(cell) * totalBlocks + cells - 1) / cells);
}

VolumeSummary summariseVolume(const uint8_t *blocks, size_t size, int cells) {
  VolumeSummary summary;
  const uint32_t imageBlocks = static_cast<uint32_t>(size / BLOCK);
  summary.totalBlocks = imageBlocks;
  if (!blocks || imageBlocks <= VOLUME_DIRECTORY) return summary;

  const uint8_t *header = blocks + VOLUME_DIRECTORY * BLOCK;
  const uint8_t storage = header[STORAGE_AND_LENGTH] >> 4;
  const int nameLength = header[STORAGE_AND_LENGTH] & 0x0F;
  if (storage != 0x0F || nameLength == 0 || header[ENTRY_LENGTH] != 0x27 ||
      header[ENTRIES_PER_BLOCK] != 0x0D) {
    return summary;
  }
  const uint32_t total = std::min<uint32_t>(word(header + TOTAL_BLOCKS), imageBlocks);
  const uint32_t bitmap = word(header + BITMAP_POINTER);
  const size_t bitmapBytes = (total + 7) / 8;
  if (total == 0 || bitmap == 0 || (bitmap * BLOCK + bitmapBytes) > size) return summary;

  summary.prodos = true;
  summary.name = "/";
  for (int i = 0; i < nameLength; i++) summary.name += static_cast<char>(header[NAME + i] & 0x7F);
  summary.totalBlocks = total;
  summary.entries = word(header + FILE_COUNT);

  std::vector<uint32_t> usedInCell(std::max(cells, 0), 0);
  std::vector<uint32_t> blocksInCell(std::max(cells, 0), 0);
  const uint8_t *bits = blocks + bitmap * BLOCK;
  for (uint32_t block = 0; block < total; block++) {
    // A set bit is a free block, block 0 the top bit of the first byte.
    const bool free = bits[block / 8] & (0x80 >> (block % 8));
    if (free) summary.freeBlocks++;
    if (cells > 0) {
      const int cell = cellForBlock(block, total, cells);
      blocksInCell[cell]++;
      if (!free) usedInCell[cell]++;
    }
  }
  summary.used.assign(std::max(cells, 0), 0.0f);
  for (int cell = 0; cell < cells; cell++) {
    if (blocksInCell[cell]) summary.used[cell] = static_cast<float>(usedInCell[cell]) / blocksInCell[cell];
  }
  return summary;
}

} // namespace a2e::native
