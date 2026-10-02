/*
 * machine_poll.hpp - When a view last reached the machine
 *
 * Emulation::poll() reads the machine without waiting for a refill, unless a
 * view has been turned away for too long. This is how long; one per view, so
 * each is owed its own turn. Kept apart from emulation.hpp so the views that
 * hold one need only forward-declare Emulation.
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <chrono>

namespace a2e::native {

struct MachinePoll {
  std::chrono::steady_clock::time_point last{};
};

} // namespace a2e::native
