/*
 * frame_queue.hpp - Finished pictures, from the emulation thread to the screen
 *
 * The rules are src/js/worker/frame-queue.js's, which the browser build
 * arrived at by measurement: frames are published when audio has run the
 * machine through one, and audio's clock is not the display's, so some
 * arrive in pairs a few milliseconds apart. A single "newest" slot drew the
 * first of a pair over before it was shown. A queue keeps both and shows them
 * on successive refreshes.
 *
 * Two counters run it. The producer is the only writer of `written_` and the
 * consumer the only writer of `shown_`, so there is no lock. Frame n lives in
 * slot n % SLOTS. The consumer owns the slot of the last frame it took,
 * because it keeps drawing from it, and the producer rule is what guarantees
 * the producer never writes there.
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace a2e::native {

class FrameQueue {
public:
  static constexpr int SLOTS = 4;
  // The furthest the consumer lets itself fall behind before jumping to the
  // newest frame, so a backlog costs a skipped frame rather than delay.
  static constexpr int MAX_BACKLOG = 2;

  struct Frame {
    std::vector<uint8_t> pixels; // RGBA
    int width = 0;
    int height = 0;
  };

  // Producer: the slot the next frame may be written into, or -1 if the
  // queue is full and this frame should be dropped. At most SLOTS - 2 frames
  // wait unshown: one slot for the frame being written and one for the frame
  // the consumer is holding.
  int slotToWrite() const {
    const uint32_t written = written_.load(std::memory_order_relaxed);
    const uint32_t shown = shown_.load(std::memory_order_acquire);
    if (static_cast<int32_t>(written - shown) >= SLOTS - 1) return -1;
    return static_cast<int>(written % SLOTS);
  }

  // Producer: copy a finished frame in and publish it. Returns false if it
  // was dropped because the consumer has stopped taking frames.
  bool push(const uint8_t *rgba, int width, int height) {
    const int slot = slotToWrite();
    if (slot < 0) return false;
    Frame &frame = frames_[slot];
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    frame.pixels.resize(bytes);
    std::memcpy(frame.pixels.data(), rgba, bytes);
    frame.width = width;
    frame.height = height;
    written_.fetch_add(1, std::memory_order_release);
    return true;
  }

  // Consumer: the frame to show at this refresh, or nullptr if nothing new
  // has arrived. Taking a frame hands the previously held slot back. The
  // oldest unshown frame is taken, so two that arrived together are shown a
  // refresh apart instead of one hiding the other.
  const Frame *take() {
    const uint32_t written = written_.load(std::memory_order_acquire);
    const uint32_t shown = shown_.load(std::memory_order_relaxed);
    const int32_t backlog = static_cast<int32_t>(written - shown);
    if (backlog <= 0) return nullptr;
    const uint32_t frame = backlog > MAX_BACKLOG ? written - 1 : shown;
    shown_.store(frame + 1, std::memory_order_release);
    return &frames_[frame % SLOTS];
  }

private:
  std::array<Frame, SLOTS> frames_;
  std::atomic<uint32_t> written_{0};
  std::atomic<uint32_t> shown_{0};
};

} // namespace a2e::native
