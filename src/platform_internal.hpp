#pragma once

#include <cachescope/benchmark.hpp>

namespace cachescope::detail {

// Pins the *calling* thread to `preferred_cpu` (or the first allowed CPU when
// negative) and records the outcome in `info`. Returns false when the platform
// offers no strict pinning or the request was rejected.
bool apply_affinity(SystemInfo& info, int preferred_cpu);

// Pins the calling thread to exactly one logical CPU. Used by the multi-thread
// coherency probe, which must place its two workers itself.
bool pin_current_thread(int cpu);

// Best-effort current core frequency in MHz, 0 when the OS does not expose it.
double current_cpu_mhz(int cpu);

// Best-effort maximum (turbo) core frequency in MHz, 0 when unknown. Used only
// to tell the reader when the OS is reporting a static clock.
double maximum_cpu_mhz(int cpu);

// Logical CPUs the process is actually allowed to run on, in ascending order.
std::vector<int> allowed_cpus();

} // namespace cachescope::detail
