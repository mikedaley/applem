/*
 * audio_ring.hpp - Samples from the emulation thread to the audio device
 *
 * One writer (the emulation thread) and one reader (Core Audio's render
 * callback, which runs on a real-time thread and so must never block or
 * allocate). Interleaved stereo floats; positions count frames, not floats.
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace a2e::native {

class AudioRing {
public:
  explicit AudioRing(size_t capacityFrames)
      : capacity_(capacityFrames), buffer_(capacityFrames * 2) {}

  // Frames waiting to be played.
  size_t available() const {
    return writePos_.load(std::memory_order_acquire) -
           readPos_.load(std::memory_order_acquire);
  }

  size_t space() const { return capacity_ - available(); }

  // Writer: as much of `frames` as fits. A full ring drops the rest, which is
  // what happens when nothing is reading.
  size_t write(const float *stereo, size_t frames) {
    const uint64_t write = writePos_.load(std::memory_order_relaxed);
    frames = std::min(frames, space());
    for (size_t i = 0; i < frames; i++) {
      const size_t at = ((write + i) % capacity_) * 2;
      buffer_[at] = stereo[i * 2];
      buffer_[at + 1] = stereo[i * 2 + 1];
    }
    writePos_.store(write + frames, std::memory_order_release);
    return frames;
  }

  // Reader: fills `frames`, with silence past what is waiting. Returns how
  // many real frames it had.
  size_t read(float *stereo, size_t frames) {
    if (clearRequested_.exchange(false, std::memory_order_acq_rel)) {
      readPos_.store(writePos_.load(std::memory_order_acquire),
                     std::memory_order_release);
    }
    const uint64_t read = readPos_.load(std::memory_order_relaxed);
    const size_t have = std::min(frames, available());
    for (size_t i = 0; i < have; i++) {
      const size_t at = ((read + i) % capacity_) * 2;
      stereo[i * 2] = buffer_[at];
      stereo[i * 2 + 1] = buffer_[at + 1];
    }
    std::fill(stereo + have * 2, stereo + frames * 2, 0.0f);
    readPos_.store(read + have, std::memory_order_release);
    return have;
  }

  // Discard everything waiting, so stale audio is not played when a machine
  // is switched or powered back on. The reader does it at its next read, so
  // each side stays the only writer of its own position.
  void clear() { clearRequested_.store(true, std::memory_order_release); }

private:
  const size_t capacity_;
  std::vector<float> buffer_;
  std::atomic<uint64_t> writePos_{0};
  std::atomic<uint64_t> readPos_{0};
  std::atomic<bool> clearRequested_{false};
};

} // namespace a2e::native
