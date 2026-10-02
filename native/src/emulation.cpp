/*
 * emulation.cpp - The machine, run on its own thread and paced by audio
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "emulation.hpp"

#include "audio/audio.hpp"
#include "cards/disk_controller.hpp"

#include <algorithm>
#include <pthread/qos.h>
#include <vector>

namespace a2e::native {

namespace {

// Refill when fewer than two frames are waiting: the browser's low-water
// mark (REFILL_FRAMES in audio-worklet.js).
constexpr size_t LOW_WATER = Emulation::SAMPLES_PER_FRAME * 2;
// The free-run clock's tick, and the most emulated time one tick may cover.
constexpr auto FREE_RUN_TICK = std::chrono::milliseconds(16);
constexpr double FREE_RUN_MAX_SECONDS = 0.1;

} // namespace

Emulation::Emulation() : wake_(dispatch_semaphore_create(0)) {}

Emulation::~Emulation() { stop(); }

void Emulation::start(MachineId machine, size_t iigsFastRam) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    host_.setIIgsFastRam(iigsFastRam);
    host_.setMachine(machine);
    applyGain();
  }

  // The device's callback: hand over what is waiting, and ask for more when
  // the ring runs low. Nothing here may block.
  audio_.start(SAMPLE_RATE, [this](float *stereo, size_t frames) {
    ring_.read(stereo, frames);
    const float gain = gain_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames * 2; i++) stereo[i] *= gain;
    driveSounds_.mix(stereo, frames, gain);
    equalizer_.process(stereo, frames);
    if (ring_.available() < LOW_WATER) dispatch_semaphore_signal(wake_);
  });

  quit_ = false;
  thread_ = std::thread([this] { run(); });
}

void Emulation::stop() {
  if (!thread_.joinable()) return;
  quit_ = true;
  dispatch_semaphore_signal(wake_);
  thread_.join();
  audio_.stop();
}

void Emulation::run() {
  // The thread the picture and the sound wait on, so it is scheduled as the
  // UI is. Left at the default class, macOS may put it on an efficiency
  // core: one 8x frame then took 8.6ms on an M5's (2.1ms on a performance
  // core) and more than the 16.7ms a frame lasts on an M1's, which ran the
  // machine at 95% of 8x and let the sound run dry between frames.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

  std::vector<float> scratch(SAMPLES_PER_FRAME * 2);
  auto last = std::chrono::steady_clock::now();
  double owed = 0.0; // free-run samples not yet generated

  while (!quit_) {
    const bool audio = audio_.running();
    const auto timeout = audio ? std::chrono::milliseconds(50) : FREE_RUN_TICK;
    dispatch_semaphore_wait(
        wake_, dispatch_time(DISPATCH_TIME_NOW,
                             std::chrono::nanoseconds(timeout).count()));
    if (quit_) break;

    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - last).count();
    last = now;

    if (!powered_) {
      // Nothing refills, so what was posted is applied here.
      std::lock_guard<std::mutex> lock(mutex_);
      applyPosted();
      owed = 0.0;
      continue;
    }

    if (audio) {
      while (!quit_ && powered_ && ring_.available() < LOW_WATER) {
        refill(scratch.data(), true);
      }
    } else {
      // No device: run for the time that has passed, and throw the sound away.
      owed += std::min(elapsed, FREE_RUN_MAX_SECONDS) * SAMPLE_RATE;
      while (!quit_ && owed >= SAMPLES_PER_FRAME) {
        refill(scratch.data(), false);
        owed -= SAMPLES_PER_FRAME;
      }
    }
    measure();
  }
}

// One video frame of samples: the machine runs for exactly that long, and if
// it finished a frame in doing so the picture is published.
void Emulation::refill(float *scratch, bool toDevice) {
  while (waiting_.load(std::memory_order_relaxed) > 0) std::this_thread::yield();
  std::lock_guard<std::mutex> lock(mutex_);
  applyPosted();
  host_.generateStereoAudioSamples(scratch, SAMPLES_PER_FRAME);
  // A paused machine is silent, as the browser's is: its sound sources would
  // otherwise hold whatever they were playing, a Mockingboard or an Ensoniq
  // note sounding for as long as the debugger sits on a breakpoint. The level
  // ramps across one buffer on the way in and out, so neither edge clicks.
  const float target = host_.isPaused() ? 0.0f : 1.0f;
  if (pauseLevel_ != target || target == 0.0f) {
    const float from = pauseLevel_;
    for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
      const float level = from + (target - from) * (i + 1) / SAMPLES_PER_FRAME;
      scratch[i * 2] *= level;
      scratch[i * 2 + 1] *= level;
    }
    pauseLevel_ = target;
  }
  if (toDevice) ring_.write(scratch, SAMPLES_PER_FRAME);

  if (host_.consumeFrameSamples() > 0) {
    const auto &display = host_.profile().display;
    const int width = display.pixelWidth;
    const int height = display.pixelHeight;
    if (host_.framebufferSize() == static_cast<size_t>(width) * height * 4) {
      frames_.push(host_.framebuffer(), width, height);
    }
  }
}

void Emulation::post(Posted f) {
  std::lock_guard<std::mutex> lock(postedMutex_);
  posted_.push_back(std::move(f));
}

void Emulation::applyPosted() {
  std::vector<Posted> due;
  {
    std::lock_guard<std::mutex> lock(postedMutex_);
    if (posted_.empty()) return;
    due.swap(posted_);
  }
  for (Posted &f : due) f(host_);
}

void Emulation::measure() {
  constexpr double SAMPLE_SECONDS = 1.0;
  constexpr double WINDOW_SECONDS = 10.0;
  const auto now = std::chrono::steady_clock::now();
  if (measureReset_.exchange(false)) {
    measureSamples_.clear();
    measuredMHz_ = 0.0;
  }
  if (!measureSamples_.empty() &&
      std::chrono::duration<double>(now - measureSamples_.back().first).count() < SAMPLE_SECONDS) {
    return;
  }
  uint64_t cycles = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cycles = host_.totalCycles();
  }
  // A clock that went backwards is a machine that was rebuilt or reloaded.
  if (!measureSamples_.empty() && cycles < measureSamples_.back().second) measureSamples_.clear();
  measureSamples_.emplace_back(now, cycles);
  while (measureSamples_.size() > 2 &&
         std::chrono::duration<double>(now - measureSamples_.front().first).count() > WINDOW_SECONDS) {
    measureSamples_.pop_front();
  }
  if (measureSamples_.size() < 2) return;
  const auto &[firstTime, firstCycles] = measureSamples_.front();
  const double seconds = std::chrono::duration<double>(now - firstTime).count();
  if (seconds > 0) measuredMHz_ = (cycles - firstCycles) / seconds / 1.0e6;
}

void Emulation::setPowered(bool on) {
  if (on == powered_) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    applyPosted(); // for the machine it was meant for
    if (on) {
      host_.reset();
    } else if (DiskController *disk = host_.diskController()) {
      disk->stopMotor();
    }
  }
  ring_.clear();
  measureReset_ = true;
  powered_ = on;
  dispatch_semaphore_signal(wake_);
}

bool Emulation::setMachine(MachineId id) {
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    applyPosted(); // for the machine it was meant for
    ok = host_.setMachine(id);
    applyGain();
  }
  measureReset_ = true;
  ring_.clear();
  return ok;
}

bool Emulation::setIIgsFastRam(size_t bytes) {
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    applyPosted(); // for the machine it was meant for
    ok = host_.setIIgsFastRam(bytes);
    applyGain();
  }
  ring_.clear();
  return ok;
}

void Emulation::setVolume(float volume) {
  std::lock_guard<std::mutex> lock(mutex_);
  volume_ = std::clamp(volume, 0.0f, 1.0f);
  applyGain();
}

void Emulation::setMuted(bool muted) {
  std::lock_guard<std::mutex> lock(mutex_);
  muted_ = muted;
  applyGain();
}

// Called with the lock held. A rebuilt machine has a new speaker, so this
// runs after every rebuild as well as on every change.
void Emulation::applyGain() {
  if (Audio *speaker = host_.speaker()) speaker->setVolume(volume_);
  gain_ = muted_ ? 0.0f : volume_;
}

} // namespace a2e::native
