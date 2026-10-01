/*
 * emulation.hpp - The machine, run on its own thread and paced by audio
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "../../src/host/machine_host.hpp"
#include "audio_output.hpp"
#include "audio_ring.hpp"
#include "drive_sounds.hpp"
#include "frame_queue.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <dispatch/dispatch.h>
#include <mutex>
#include <thread>

namespace a2e::native {

// Owns the machine and the thread that runs it.
//
// The timing is the browser build's (CLAUDE.md, Audio-Driven Timing): the
// audio device asks for samples, the machine runs for exactly the time those
// samples represent, and a frame is published whenever a whole frame's worth
// has been generated. A refill is one video frame, 800 samples, and the
// device's callback asks for one whenever fewer than two are waiting.
//
// When there is no audio device, a free-running clock stands in, as the
// browser's does before a user gesture: it runs the machine for the real time
// that has passed, capped at 100ms a tick so a stalled thread loses time
// rather than racing to catch up.
//
// Everything else that touches the machine goes through withMachine(), which
// holds the same lock the thread holds for each refill, and announces itself
// first so the thread lets it in before the next one. A refill is one frame
// of the machine, so the UI waits at most that long for a debug view or a
// key. Without the announcement a machine that cannot keep up (8x on an M1)
// refills back to back, and since a mutex is not fair the thread took it
// straight back every time: the UI waited up to eleven seconds for it, and
// the screen sat on whatever picture it last had.
class Emulation {
public:
  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int SAMPLES_PER_FRAME = 800;

  Emulation();
  ~Emulation();

  Emulation(const Emulation &) = delete;
  Emulation &operator=(const Emulation &) = delete;

  // Build the machine, open the audio device and start the thread.
  void start(MachineId machine, size_t iigsFastRam);
  void stop();

  template <typename F> decltype(auto) withMachine(F &&f) {
    waiting_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    waiting_.fetch_sub(1, std::memory_order_relaxed);
    return f(host_);
  }

  // Powered off, the machine does not run and nothing is drawn; powering on
  // is a cold start, as the power switch is.
  void setPowered(bool on);
  bool powered() const { return powered_.load(); }

  // Rebuild as a different machine. Media and memory do not survive.
  bool setMachine(MachineId id);
  bool setIIgsFastRam(size_t bytes);

  // The newest finished picture not yet shown, or nullptr. Valid until the
  // next call.
  const FrameQueue::Frame *takeFrame() { return frames_.take(); }

  // The output gain, applied on top of the speaker's own volume exactly as
  // the browser's gain node is, so the two builds sound the same.
  void setVolume(float volume);
  void setMuted(bool muted);

  bool audioRunning() const { return audio_.running(); }
  // The drives' own sounds, mixed in at the main volume.
  DriveSounds &driveSounds() { return driveSounds_; }
  // The machine's clock as measured against the wall, in MHz, across the
  // last ten seconds. A refill runs a whole video frame at once (about
  // 17,000 cycles), so a one-second window caught a refill more or fewer and
  // wandered by about 1.3%; ten seconds brings that to about 0.1%.
  double measuredMHz() const { return measuredMHz_.load(); }
  // Start the measurement again: the clock it is measuring has changed.
  void resetMeasurement() { measureReset_ = true; }

private:
  void run();
  void refill(float *scratch, bool toDevice);
  void applyGain();
  void measure();

  host::MachineHost host_;
  std::mutex mutex_;
  // Callers of withMachine() waiting for the lock, which a refill yields to.
  std::atomic<int> waiting_{0};
  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> powered_{false};

  AudioRing ring_{SAMPLE_RATE / 2};
  FrameQueue frames_;
  AudioOutput audio_;
  DriveSounds driveSounds_{SAMPLE_RATE};
  dispatch_semaphore_t wake_;

  std::atomic<float> gain_{0.5f};
  float volume_ = 0.5f;
  bool muted_ = false;

  std::atomic<double> measuredMHz_{0.0};
  // (time, cycles) once a second, oldest first, on the emulation thread only.
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> measureSamples_;
  std::atomic<bool> measureReset_{true};
};

} // namespace a2e::native
