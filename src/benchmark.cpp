// CacheScope measurement engine.
//
// Design rules that the kernels below follow deliberately:
//
//  * Latency probes must be *dependent*: the address of access N+1 comes from
//    the value loaded by access N, so the processor cannot overlap misses.
//  * Throughput probes must NOT be dependent: they are allowed to vectorize,
//    prefetch and pipeline, because that is what real streaming code does.
//  * Every kernel must return a value that the caller consumes, and no kernel
//    may be provably redundant across repetitions, or the optimizer will delete
//    the work and report impossible numbers.
//  * Nothing here claims that a measurement isolates one cache level. The
//    experiments report where curves actually change; hardware-reported
//    geometry is carried separately.

#include <cachescope/benchmark.hpp>
#include "platform_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <new>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#  define CACHESCOPE_NOINLINE __declspec(noinline)
#else
#  define CACHESCOPE_NOINLINE __attribute__((noinline))
#endif

namespace cachescope {
namespace {

using Clock = std::chrono::steady_clock;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

// Every kernel result is folded in here so no kernel can be optimized away.
std::atomic<std::uint64_t> g_sink{0};

class AlignedBuffer {
public:
    explicit AlignedBuffer(std::size_t bytes, std::size_t alignment = 64)
        : bytes_(bytes),
          alignment_(std::max<std::size_t>(alignment, alignof(std::max_align_t))),
          data_(::operator new(round_up(bytes, alignment_), std::align_val_t(alignment_))) {}
    ~AlignedBuffer() {
        if (data_) ::operator delete(data_, std::align_val_t(alignment_));
    }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& other) noexcept
        : bytes_(other.bytes_), alignment_(other.alignment_), data_(other.data_) {
        other.data_ = nullptr;
    }
    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            if (data_) ::operator delete(data_, std::align_val_t(alignment_));
            bytes_ = other.bytes_;
            alignment_ = other.alignment_;
            data_ = other.data_;
            other.data_ = nullptr;
        }
        return *this;
    }
    template <typename T> T* as() noexcept { return static_cast<T*>(data_); }
    template <typename T> const T* as() const noexcept { return static_cast<const T*>(data_); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_; }

private:
    static std::size_t round_up(std::size_t value, std::size_t alignment) {
        return ((value + alignment - 1) / alignment) * alignment;
    }
    std::size_t bytes_{};
    std::size_t alignment_{};
    void* data_{};
};

struct Measurement {
    Statistics ns_per_access;
    Statistics gib_per_second;
    std::uint64_t operations = 0;
};

double quantile_sorted(const std::vector<double>& values, double q) {
    if (values.empty()) return 0.0;
    if (values.size() == 1) return values.front();
    const double position = q * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(position));
    const auto hi = static_cast<std::size_t>(std::ceil(position));
    const double t = position - static_cast<double>(lo);
    return values[lo] * (1.0 - t) + values[hi] * t;
}

std::uint64_t largest_cache(const SystemInfo& system, int level) {
    std::uint64_t best = 0;
    for (const auto& c : system.caches) {
        if (c.level == level && (c.type == CacheType::data || c.type == CacheType::unified)) {
            best = std::max(best, c.size_bytes);
        }
    }
    return best;
}

// The single cache entry that stands for a level. A hybrid CPU reports several
// entries per level with different sizes, line sizes and way counts, so size,
// line and associativity must all be taken from the *same* entry: pairing the
// largest size with some other entry's way count describes no real cache.
const CacheInfo* representative_cache(const SystemInfo& system, int level) {
    const CacheInfo* best = nullptr;
    for (const auto& c : system.caches) {
        if (c.level != level) continue;
        if (c.type != CacheType::data && c.type != CacheType::unified) continue;
        if (!best || c.size_bytes > best->size_bytes) best = &c;
    }
    return best;
}

std::uint64_t round_to_64(std::uint64_t value) {
    return std::max<std::uint64_t>(64, (value + 63ull) & ~63ull);
}

std::string utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    std::array<char, 32> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf.data();
}

// ---------------------------------------------------------------------------
// Kernels
// ---------------------------------------------------------------------------

// Dependent pointer chase, one node per cache line. `steps` may exceed the
// number of nodes: the chain is a single cycle, so it simply wraps.
CACHESCOPE_NOINLINE std::uint64_t pointer_chase(const std::byte* storage,
                                                std::size_t node_stride,
                                                std::size_t steps,
                                                std::uint32_t start) {
    std::uint32_t index = start;
    auto step = [&]() {
        index = *reinterpret_cast<const std::uint32_t*>(storage +
                                                        static_cast<std::size_t>(index) * node_stride);
    };
    std::size_t i = 0;
    for (; i + 8 <= steps; i += 8) {
        step(); step(); step(); step();
        step(); step(); step(); step();
    }
    for (; i < steps; ++i) step();
    return index;
}

CACHESCOPE_NOINLINE std::uint64_t sequential_read_kernel(std::uint64_t* data,
                                                         std::size_t words,
                                                         std::uint64_t seed) {
    // One seed-dependent sentinel mutation per pass prevents whole-pass common
    // subexpression elimination across calibrated repetitions. Its cost is
    // negligible next to the hundreds to millions of measured loads.
    const auto sentinel = static_cast<std::size_t>((seed * 11400714819323198485ull) % words);
    data[sentinel] ^= (seed | 1ull);
    std::uint64_t sum = seed;
    for (std::size_t i = 0; i < words; ++i) sum += data[i];
    return sum;
}

CACHESCOPE_NOINLINE std::uint64_t sequential_write_kernel(std::uint64_t* data,
                                                          std::size_t words,
                                                          std::uint64_t seed) {
    for (std::size_t i = 0; i < words; ++i) {
        data[i] = seed + static_cast<std::uint64_t>(i) * 0x9E3779B1ull;
    }
    return data[(seed * 1315423911ull) % words];
}

CACHESCOPE_NOINLINE std::uint64_t copy_kernel(std::byte* destination,
                                              const std::byte* source,
                                              std::size_t bytes,
                                              std::uint64_t seed) {
    std::memcpy(destination, source, bytes);
    return static_cast<std::uint64_t>(destination[(seed * 2654435761ull) % bytes]);
}

CACHESCOPE_NOINLINE std::uint64_t stride_kernel(const std::byte* data,
                                                std::size_t bytes,
                                                std::size_t stride,
                                                std::size_t offset) {
    std::uint64_t sum = 0;
    for (std::size_t i = offset; i < bytes; i += stride) {
        sum += static_cast<unsigned char>(data[i]);
    }
    return sum;
}

// Serialized line-pair probe. The address of the second load depends on the
// value returned by the first, so the two requests cannot be issued in
// parallel: when both land in the same cache line the pair is cheap, and when
// they straddle a line boundary the pair costs two serialized misses.
CACHESCOPE_NOINLINE std::uint64_t line_pair_chase(const std::byte* storage,
                                                  std::size_t region_stride,
                                                  std::size_t steps,
                                                  std::size_t delta,
                                                  std::uint32_t start) {
    std::uint32_t index = start;
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < steps; ++i) {
        const auto* base = storage + static_cast<std::size_t>(index) * region_stride;
        const std::uint32_t next = *reinterpret_cast<const std::uint32_t*>(base);
        // The +0/+1 jitter keeps the second address value-dependent without ever
        // crossing the boundary under test.
        const std::size_t dependent_delta = delta + static_cast<std::size_t>(next & 1u);
        const auto nearby = static_cast<std::uint32_t>(static_cast<unsigned char>(base[dependent_delta]));
        // Probe bytes are zero, so this preserves the cycle while still making
        // the next region address depend on the second load.
        index = next ^ (nearby & 1u);
        sum += nearby;
    }
    return static_cast<std::uint64_t>(index) ^ sum;
}

// ---------------------------------------------------------------------------
// Timing harness
// ---------------------------------------------------------------------------

template <class Kernel>
Measurement measure_kernel(Kernel&& kernel,
                           std::uint64_t operations_per_invocation,
                           std::uint64_t logical_bytes_per_invocation,
                           const BenchmarkConfig& config) {
    using namespace std::chrono;
    if (operations_per_invocation == 0) throw std::runtime_error("zero-operation benchmark kernel");

    std::uint64_t repeats = 1;
    double elapsed_ms = 0.0;

    // Calibrate a batch long enough to dominate timer-call overhead, then scale
    // it to the requested sample duration.
    for (int attempt = 0; attempt < 24; ++attempt) {
        std::uint64_t checksum = 0;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto begin = Clock::now();
        for (std::uint64_t r = 0; r < repeats; ++r) checksum ^= kernel(r);
        const auto end = Clock::now();
        std::atomic_signal_fence(std::memory_order_seq_cst);
        g_sink.fetch_xor(checksum, std::memory_order_relaxed);
        elapsed_ms = duration<double, std::milli>(end - begin).count();
        if (elapsed_ms >= std::max(0.20, config.target_sample_ms * 0.20)) break;
        if (repeats > (std::numeric_limits<std::uint64_t>::max() / 2)) break;
        repeats *= 2;
    }

    const double single_invocation_ms = elapsed_ms / static_cast<double>(std::max<std::uint64_t>(1, repeats));
    if (elapsed_ms > 0.0) {
        const double scale = config.target_sample_ms / elapsed_ms;
        const double desired = std::clamp(scale * static_cast<double>(repeats), 1.0,
                                          static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
        repeats = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(desired)));
    }

    // A single traversal of a very large working set can already take much
    // longer than the sample target. Taking the full sample count there costs
    // minutes for no extra signal, so trade a few samples for wall-clock time.
    int samples = config.samples;
    int warmups = config.warmup_samples;
    if (single_invocation_ms > config.target_sample_ms * 4.0) {
        samples = std::max(3, config.samples / 3);
        warmups = std::min(warmups, 1);
    }

    for (int w = 0; w < warmups; ++w) {
        std::uint64_t checksum = 0;
        for (std::uint64_t r = 0; r < repeats; ++r) {
            checksum ^= kernel(r + 0x10000ull * static_cast<std::uint64_t>(w + 1));
        }
        g_sink.fetch_xor(checksum, std::memory_order_relaxed);
    }

    std::vector<double> ns;
    std::vector<double> gib;
    ns.reserve(static_cast<std::size_t>(samples));
    gib.reserve(static_cast<std::size_t>(samples));

    const long double total_ops = static_cast<long double>(operations_per_invocation) * repeats;
    const long double total_bytes = static_cast<long double>(logical_bytes_per_invocation) * repeats;

    for (int sample = 0; sample < samples; ++sample) {
        std::uint64_t checksum = 0;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto begin = Clock::now();
        for (std::uint64_t r = 0; r < repeats; ++r) {
            checksum ^= kernel(r + static_cast<std::uint64_t>(sample) * 0x100000ull);
        }
        const auto end = Clock::now();
        std::atomic_signal_fence(std::memory_order_seq_cst);
        g_sink.fetch_xor(checksum, std::memory_order_relaxed);

        const double elapsed_ns = duration<double, std::nano>(end - begin).count();
        ns.push_back(elapsed_ns / static_cast<double>(total_ops));
        if (logical_bytes_per_invocation > 0) {
            const double seconds = elapsed_ns * 1e-9;
            if (seconds > 0.0) gib.push_back(static_cast<double>(total_bytes) / kGiB / seconds);
        }
    }

    Measurement result;
    result.ns_per_access = compute_statistics(std::move(ns));
    if (!gib.empty()) result.gib_per_second = compute_statistics(std::move(gib));
    result.operations = static_cast<std::uint64_t>(
        std::min<long double>(total_ops, static_cast<long double>(std::numeric_limits<std::uint64_t>::max())));
    return result;
}

// Small working sets finish a full traversal in well under a microsecond, where
// the call/return overhead of one invocation would be a visible fraction of the
// measurement. Chasing extra laps around the same cycle removes that bias
// without changing the resident footprint.
std::size_t chase_steps(std::size_t elements) {
    constexpr std::size_t kMinimumSteps = 8192;
    if (elements >= kMinimumSteps) return elements;
    return ((kMinimumSteps + elements - 1) / elements) * elements;
}

std::uint32_t representative_line_size(const SystemInfo& system) {
    std::uint32_t line = 0;
    for (const auto& c : system.caches) {
        if (c.type == CacheType::data || c.type == CacheType::unified) {
            line = std::max(line, c.line_size_bytes);
        }
    }
    if (line < sizeof(std::uint32_t) || line > 4096) line = 64;
    return line;
}

// ---------------------------------------------------------------------------
// Individual experiments
// ---------------------------------------------------------------------------

SweepPoint benchmark_latency(std::uint64_t bytes, const BenchmarkConfig& config, std::uint32_t line_size) {
    const std::size_t stride = std::max<std::size_t>(sizeof(std::uint32_t), line_size);
    const auto elements = static_cast<std::size_t>(std::max<std::uint64_t>(64, bytes / stride));
    const std::size_t actual = elements * stride;
    const std::size_t alignment = std::bit_ceil(std::max<std::size_t>(64, stride));
    AlignedBuffer storage(actual, alignment);
    auto next = make_random_cycle(elements, 0xCACE5C0Eull ^ bytes);
    auto* raw = storage.as<std::byte>();
    std::memset(raw, 0xA5, actual);
    for (std::size_t i = 0; i < elements; ++i) {
        *reinterpret_cast<std::uint32_t*>(raw + i * stride) = next[i];
    }
    next.clear();
    next.shrink_to_fit();

    const std::size_t steps = chase_steps(elements);
    const auto m = measure_kernel(
        [&](std::uint64_t seed) {
            return pointer_chase(raw, stride, steps, static_cast<std::uint32_t>(seed % elements));
        },
        steps, 0, config);

    SweepPoint p;
    p.working_set_bytes = actual;
    p.stride_bytes = stride;
    p.element_count = elements;
    p.ns_per_access = m.ns_per_access;
    p.operations_per_sample = m.operations;
    return p;
}

SweepPoint benchmark_read(std::uint64_t bytes, const BenchmarkConfig& config) {
    const auto words = static_cast<std::size_t>(std::max<std::uint64_t>(8, bytes / sizeof(std::uint64_t)));
    const std::size_t actual = words * sizeof(std::uint64_t);
    AlignedBuffer buffer(actual);
    auto* data = buffer.as<std::uint64_t>();
    for (std::size_t i = 0; i < words; ++i) {
        data[i] = static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull + 1;
    }
    const auto m = measure_kernel(
        [&](std::uint64_t seed) { return sequential_read_kernel(data, words, seed + 1); }, words, actual,
        config);

    SweepPoint p;
    p.working_set_bytes = actual;
    p.element_count = words;
    p.ns_per_access = m.ns_per_access;
    p.gib_per_second = m.gib_per_second;
    p.operations_per_sample = m.operations;
    return p;
}

SweepPoint benchmark_write(std::uint64_t bytes, const BenchmarkConfig& config) {
    const auto words = static_cast<std::size_t>(std::max<std::uint64_t>(8, bytes / sizeof(std::uint64_t)));
    const std::size_t actual = words * sizeof(std::uint64_t);
    AlignedBuffer buffer(actual);
    auto* data = buffer.as<std::uint64_t>();
    std::fill_n(data, words, 0ull);
    const auto m = measure_kernel(
        [&](std::uint64_t seed) { return sequential_write_kernel(data, words, seed + 1); }, words, actual,
        config);

    SweepPoint p;
    p.working_set_bytes = actual;
    p.element_count = words;
    p.ns_per_access = m.ns_per_access;
    p.gib_per_second = m.gib_per_second;
    p.operations_per_sample = m.operations;
    return p;
}

SweepPoint benchmark_copy(std::uint64_t bytes, const BenchmarkConfig& config) {
    // memcpy touches a source and a destination, so each buffer gets half of the
    // requested footprint. Otherwise the x-axis would understate the working set
    // by 2x relative to every other curve.
    const auto half = static_cast<std::size_t>(std::max<std::uint64_t>(64, bytes / 2));
    AlignedBuffer source(half);
    AlignedBuffer destination(half);
    std::memset(source.as<void>(), 0xA5, half);
    std::memset(destination.as<void>(), 0x5A, half);
    const auto m = measure_kernel(
        [&](std::uint64_t seed) {
            return copy_kernel(destination.as<std::byte>(), source.as<std::byte>(), half, seed);
        },
        half / sizeof(std::uint64_t), half, config);

    SweepPoint p;
    p.working_set_bytes = half * 2;
    p.element_count = half / sizeof(std::uint64_t);
    p.ns_per_access = m.ns_per_access;
    p.gib_per_second = m.gib_per_second;  // bytes copied per second (read+write traffic is 2x)
    p.operations_per_sample = m.operations;
    return p;
}

std::vector<SweepPoint> benchmark_line_probe(std::uint64_t bytes,
                                             std::uint32_t reported_line,
                                             const BenchmarkConfig& config,
                                             const std::function<bool()>& tick) {
    const std::size_t region_stride =
        std::bit_ceil(std::max<std::size_t>(512, static_cast<std::size_t>(reported_line) * 4));
    const auto requested_regions =
        static_cast<std::size_t>(std::max<std::uint64_t>(1024, bytes / region_stride));
    const std::size_t regions = std::bit_floor(requested_regions);
    const std::size_t actual = regions * region_stride;
    AlignedBuffer storage(actual, std::bit_ceil(std::max<std::size_t>(64, region_stride)));
    auto* raw = storage.as<std::byte>();
    std::memset(raw, 0, actual);

    auto next = make_random_cycle(regions, 0x11AE51E5ull ^ bytes);
    for (std::size_t i = 0; i < regions; ++i) {
        *reinterpret_cast<std::uint32_t*>(raw + i * region_stride) = next[i];
    }
    next.clear();
    next.shrink_to_fit();

    constexpr std::array<std::size_t, 18> candidates{
        8, 16, 24, 32, 40, 48, 56, 60, 64, 80, 96, 112, 128, 160, 192, 256, 384, 512};

    // This probe runs against a last-level cache that other processes are also
    // using, so it needs more samples than the sweeps to get a usable median.
    BenchmarkConfig probe_config = config;
    probe_config.samples = std::max(config.samples, 11);

    const std::size_t steps = chase_steps(regions);
    std::vector<SweepPoint> points;
    for (const auto delta : candidates) {
        if (delta + 1 >= region_stride) continue;
        const auto m = measure_kernel(
            [&](std::uint64_t seed) {
                return line_pair_chase(raw, region_stride, steps, delta,
                                       static_cast<std::uint32_t>(seed % regions));
            },
            steps, 0, probe_config);
        SweepPoint p;
        p.working_set_bytes = actual;
        p.stride_bytes = delta;
        p.element_count = regions;
        p.ns_per_access = m.ns_per_access;
        p.operations_per_sample = m.operations;
        points.push_back(p);
        if (!tick()) break;
    }
    return points;
}

std::vector<SweepPoint> benchmark_stride(std::uint64_t bytes,
                                         const BenchmarkConfig& config,
                                         const std::function<bool()>& tick) {
    const auto actual = static_cast<std::size_t>(std::max<std::uint64_t>(64 * 1024, bytes));
    AlignedBuffer buffer(actual, 4096);
    auto* data = buffer.as<std::byte>();
    for (std::size_t i = 0; i < actual; ++i) {
        data[i] = static_cast<std::byte>((i * 131u + 17u) & 0xffu);
    }

    std::vector<SweepPoint> points;
    for (std::size_t stride = 4; stride <= 2048; stride *= 2) {
        const auto m = measure_kernel(
            [&](std::uint64_t seed) {
                const std::size_t offset = static_cast<std::size_t>(seed % stride);
                return stride_kernel(data, actual, stride, offset);
            },
            // The offset only shifts the first access, so the touched-element
            // count is the same to within one element for every offset.
            (actual + stride - 1) / stride, 0, config);
        SweepPoint p;
        p.working_set_bytes = actual;
        p.stride_bytes = stride;
        p.element_count = (actual + stride - 1) / stride;
        p.ns_per_access = m.ns_per_access;
        p.operations_per_sample = m.operations;
        points.push_back(p);
        if (!tick()) break;
    }
    return points;
}

// Conflict-miss / associativity probe.
//
// Blocks spaced by a power-of-two `stride` all map to the same set of a cache
// whose (size / ways) equals that stride. Chasing N such blocks therefore hits
// while N <= ways and starts missing on every access once N exceeds ways, which
// makes the number of ways directly observable as a latency knee.
std::vector<SweepPoint> benchmark_associativity(std::size_t stride,
                                                std::size_t max_blocks,
                                                const BenchmarkConfig& config,
                                                const std::function<bool()>& tick) {
    AlignedBuffer storage(stride * max_blocks, std::min<std::size_t>(stride, 1u << 21));
    auto* raw = storage.as<std::byte>();
    std::memset(raw, 0, stride * max_blocks);

    std::vector<SweepPoint> points;
    for (std::size_t blocks = 1; blocks <= max_blocks; ++blocks) {
        auto next = make_random_cycle(blocks, 0xA55051A7ull ^ (stride * 131u + blocks));
        for (std::size_t i = 0; i < blocks; ++i) {
            *reinterpret_cast<std::uint32_t*>(raw + i * stride) = next[i];
        }
        const std::size_t steps = chase_steps(blocks);
        const auto m = measure_kernel(
            [&](std::uint64_t seed) {
                return pointer_chase(raw, stride, steps, static_cast<std::uint32_t>(seed % blocks));
            },
            steps, 0, config);
        SweepPoint p;
        p.working_set_bytes = blocks * 64;  // bytes actually resident, one line per block
        p.stride_bytes = stride;
        p.element_count = blocks;
        p.ns_per_access = m.ns_per_access;
        p.operations_per_sample = m.operations;
        points.push_back(p);
        if (!tick()) break;
    }
    return points;
}

// TLB reach probe.
//
// One cache line is touched per page, so the resident *data* stays small while
// the number of distinct pages grows. Latency knees therefore expose the
// first- and second-level data TLB, not the data caches. The extra line-sized
// offset keeps successive blocks off a single cache set, which would otherwise
// turn this into a conflict-miss experiment.
std::vector<SweepPoint> benchmark_tlb(std::uint64_t page_size,
                                      std::size_t max_pages,
                                      const BenchmarkConfig& config,
                                      const std::function<bool()>& tick) {
    const std::size_t stride = static_cast<std::size_t>(page_size) + 64;
    AlignedBuffer storage(stride * max_pages, 4096);
    auto* raw = storage.as<std::byte>();
    std::memset(raw, 0, stride * max_pages);

    std::vector<SweepPoint> points;
    for (std::size_t pages = 8; pages <= max_pages; pages *= 2) {
        auto next = make_random_cycle(pages, 0x71B70B5Eull ^ pages);
        for (std::size_t i = 0; i < pages; ++i) {
            *reinterpret_cast<std::uint32_t*>(raw + i * stride) = next[i];
        }
        const std::size_t steps = chase_steps(pages);
        const auto m = measure_kernel(
            [&](std::uint64_t seed) {
                return pointer_chase(raw, stride, steps, static_cast<std::uint32_t>(seed % pages));
            },
            steps, 0, config);
        SweepPoint p;
        p.working_set_bytes = pages * stride;
        p.stride_bytes = stride;
        p.element_count = pages;
        p.ns_per_access = m.ns_per_access;
        p.operations_per_sample = m.operations;
        points.push_back(p);
        if (!tick()) break;
    }
    return points;
}

// ---------------------------------------------------------------------------
// Cache-coherence / false-sharing probe
// ---------------------------------------------------------------------------

struct alignas(64) PaddedAtomic {
    std::atomic<std::uint64_t> value{0};
    char padding[64 - sizeof(std::atomic<std::uint64_t>)]{};
};

double run_contended(std::atomic<std::uint64_t>* a,
                     std::atomic<std::uint64_t>* b,
                     std::uint64_t iterations,
                     int cpu_a,
                     int cpu_b,
                     bool pin) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    auto worker = [&](std::atomic<std::uint64_t>* target, int cpu) {
        if (pin) detail::pin_current_thread(cpu);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
        }
        for (std::uint64_t i = 0; i < iterations; ++i) {
            target->fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread t1(worker, a, cpu_a);
    std::thread t2(worker, b, cpu_b);
    while (ready.load(std::memory_order_acquire) < 2) {
    }
    const auto begin = Clock::now();
    go.store(true, std::memory_order_release);
    t1.join();
    t2.join();
    const auto end = Clock::now();
    const double ns = std::chrono::duration<double, std::nano>(end - begin).count();
    return ns / static_cast<double>(iterations * 2);
}

double run_uncontended(std::atomic<std::uint64_t>* a, std::uint64_t iterations, int cpu, bool pin) {
    double result = 0.0;
    std::thread t([&]() {
        if (pin) detail::pin_current_thread(cpu);
        const auto begin = Clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) a->fetch_add(1, std::memory_order_relaxed);
        const auto end = Clock::now();
        result = std::chrono::duration<double, std::nano>(end - begin).count() /
                 static_cast<double>(iterations);
    });
    t.join();
    return result;
}

// Store-to-observe round trip between two logical CPUs. Both threads always
// make progress, so this cannot deadlock even if one is descheduled.
double run_ping_pong(PaddedAtomic& x, PaddedAtomic& y, std::uint64_t rounds, int cpu_a, int cpu_b, bool pin) {
    x.value.store(0, std::memory_order_relaxed);
    y.value.store(0, std::memory_order_relaxed);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::thread responder([&]() {
        if (pin) detail::pin_current_thread(cpu_b);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
        }
        for (std::uint64_t i = 1; i <= rounds; ++i) {
            while (x.value.load(std::memory_order_acquire) < i) {
            }
            y.value.store(i, std::memory_order_release);
        }
    });

    if (pin) detail::pin_current_thread(cpu_a);
    ready.fetch_add(1, std::memory_order_release);
    while (ready.load(std::memory_order_acquire) < 2) {
    }
    const auto begin = Clock::now();
    go.store(true, std::memory_order_release);
    for (std::uint64_t i = 1; i <= rounds; ++i) {
        x.value.store(i, std::memory_order_release);
        while (y.value.load(std::memory_order_acquire) < i) {
        }
    }
    const auto end = Clock::now();
    responder.join();
    return std::chrono::duration<double, std::nano>(end - begin).count() / static_cast<double>(rounds);
}

CoherencyPair measure_pair(const std::string& label,
                           int cpu_a,
                           int cpu_b,
                           bool pin,
                           const BenchmarkConfig& config) {
    // Two atomics inside one 64-byte line, versus one per line.
    struct alignas(128) SharedLine {
        std::atomic<std::uint64_t> first{0};
        std::atomic<std::uint64_t> second{0};
    };
    SharedLine shared;
    PaddedAtomic padded_a;
    PaddedAtomic padded_b;
    PaddedAtomic ping;
    PaddedAtomic pong;

    const std::uint64_t iterations = config.preset == "quick" ? 200'000 : 600'000;
    const std::uint64_t rounds = config.preset == "quick" ? 20'000 : 60'000;
    const int samples = std::max(3, std::min(config.samples, 7));

    std::vector<double> shared_ns, padded_ns, single_ns, pingpong_ns;
    for (int s = 0; s < samples; ++s) {
        shared_ns.push_back(run_contended(&shared.first, &shared.second, iterations, cpu_a, cpu_b, pin));
        padded_ns.push_back(run_contended(&padded_a.value, &padded_b.value, iterations, cpu_a, cpu_b, pin));
        single_ns.push_back(run_uncontended(&padded_a.value, iterations, cpu_a, pin));
        pingpong_ns.push_back(run_ping_pong(ping, pong, rounds, cpu_a, cpu_b, pin));
    }

    CoherencyPair pair;
    pair.label = label;
    pair.cpu_a = cpu_a;
    pair.cpu_b = cpu_b;
    pair.pinned = pin;
    pair.shared_line_ns = compute_statistics(std::move(shared_ns));
    pair.padded_line_ns = compute_statistics(std::move(padded_ns));
    pair.single_thread_ns = compute_statistics(std::move(single_ns));
    pair.ping_pong_ns = compute_statistics(std::move(pingpong_ns));
    return pair;
}

// ---------------------------------------------------------------------------
// Summaries
// ---------------------------------------------------------------------------

const SweepPoint* closest_point(const SweepSeries* series, std::uint64_t target) {
    if (!series || series->points.empty()) return nullptr;
    return &*std::min_element(series->points.begin(), series->points.end(),
                              [&](const SweepPoint& a, const SweepPoint& b) {
                                  const auto da = a.working_set_bytes > target
                                                      ? a.working_set_bytes - target
                                                      : target - a.working_set_bytes;
                                  const auto db = b.working_set_bytes > target
                                                      ? b.working_set_bytes - target
                                                      : target - b.working_set_bytes;
                                  return da < db;
                              });
}

std::vector<LevelSummary> summarize_levels(const BenchmarkResult& result, double ghz) {
    const auto* latency = result.find("dependent_latency");
    const auto* read = result.find("sequential_read");
    const auto* write = result.find("sequential_write");
    const auto* copy = result.find("copy");
    std::vector<LevelSummary> summaries;
    if (!latency || latency->points.empty()) return summaries;

    auto fill = [&](LevelSummary& s, std::uint64_t target) {
        if (const auto* p = closest_point(latency, target)) {
            s.representative_working_set_bytes = p->working_set_bytes;
            s.latency_ns = p->ns_per_access.median;
            if (ghz > 0.0) s.latency_cycles = p->ns_per_access.median * ghz;
        }
        if (const auto* p = closest_point(read, target)) s.sequential_read_gib_s = p->gib_per_second.median;
        if (const auto* p = closest_point(write, target)) s.sequential_write_gib_s = p->gib_per_second.median;
        if (const auto* p = closest_point(copy, target)) s.copy_gib_s = p->gib_per_second.median;
    };

    std::uint64_t last_cache = 0;
    for (int level = 1; level <= 3; ++level) {
        const auto* cache = representative_cache(result.system, level);
        if (!cache || cache->size_bytes == 0) continue;
        last_cache = std::max(last_cache, cache->size_bytes);
        const std::uint64_t target =
            std::max<std::uint64_t>(result.config.minimum_working_set_bytes, cache->size_bytes / 2);
        if (latency->points.back().working_set_bytes < target) continue;
        LevelSummary s;
        s.label = "L" + std::to_string(level);
        s.reported_size_bytes = cache->size_bytes;
        s.reported_line_size_bytes = cache->line_size_bytes;
        s.reported_ways = cache->associativity_ways;
        fill(s, target);
        summaries.push_back(s);
    }

    if (last_cache == 0 || latency->points.back().working_set_bytes > last_cache) {
        const std::uint64_t target =
            last_cache > 0 ? std::min(latency->points.back().working_set_bytes, last_cache * 2)
                           : latency->points.back().working_set_bytes;
        LevelSummary s;
        s.label = "Beyond LLC / memory";
        fill(s, target);
        summaries.push_back(s);
    }
    return summaries;
}

// Where the dependent-latency curve reached 2x, 4x, 8x ... its own floor.
//
// Real curves on modern CPUs ramp rather than step, because prefetchers, sliced
// last-level caches and non-inclusive hierarchies smear the transitions. Trying
// to name "the knee" on such a curve produces a different answer every run.
// Multiples of the machine's own fastest access are well defined, monotone and
// reproducible, and they compare sensibly across very different machines.
void compute_latency_escalation(const SweepSeries& latency, Experimental& experimental) {
    if (latency.points.size() < 3) return;
    double floor_ns = latency.points.front().ns_per_access.median;
    for (const auto& p : latency.points) {
        if (p.ns_per_access.median > 0.0) floor_ns = std::min(floor_ns, p.ns_per_access.median);
    }
    if (!(floor_ns > 0.0)) return;
    experimental.fastest_latency_ns = floor_ns;

    for (const std::uint32_t multiple : {2u, 4u, 8u, 16u, 32u}) {
        for (const auto& p : latency.points) {
            if (p.ns_per_access.median >= floor_ns * multiple) {
                experimental.latency_escalation.emplace_back(multiple, p.working_set_bytes);
                break;
            }
        }
    }
}

// Spatial-locality saturation stride.
//
// While the stride is smaller than the unit the machine effectively transfers,
// every doubling of the stride halves the number of touched elements without
// changing the number of transfers, so the cost per touched element doubles.
// Once each element needs its own transfer, the per-element cost flattens.
//
// On a machine with no prefetching this lands exactly on the cache line size.
// Real CPUs prefetch, so it is an upper bound: hardware that keeps 128- or
// 256-byte strides cheap pushes the saturation point out past the line size.
std::uint32_t infer_fetch_granularity(const std::vector<SweepPoint>& points) {
    std::uint32_t granularity = 0;
    bool doubling_started = false;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const double previous = points[i - 1].ns_per_access.median;
        const double current = points[i].ns_per_access.median;
        if (previous <= 0.0) continue;
        if (points[i].stride_bytes != points[i - 1].stride_bytes * 2) continue;

        if (current / previous >= 1.5) {
            granularity = static_cast<std::uint32_t>(points[i].stride_bytes);
            doubling_started = true;
        } else if (doubling_started) {
            // The per-element cost has stopped doubling: every element now needs
            // its own fetch. Stop here, because at very large strides TLB and
            // prefetcher effects can make the curve rise again and would
            // otherwise be mistaken for a much larger fetch unit.
            break;
        }
    }
    return granularity;
}

// Cache-line size from the serialized line-pair probe.
//
// The step at the boundary is real but modest: crossing it turns the second
// load into its own cache access, and on a cache-resident working set that adds
// a fraction of the pair's cost rather than doubling it. So instead of demanding
// a huge jump from one point, require a moderate jump that *stays* elevated for
// every larger offset. That is what distinguishes a boundary from noise.
std::uint32_t infer_line_size(const std::vector<SweepPoint>& points) {
    if (points.size() < 8) return 0;

    auto median_of = [&](std::size_t begin, std::size_t end) {
        std::vector<double> values;
        for (std::size_t i = begin; i < end; ++i) values.push_back(points[i].ns_per_access.median);
        std::sort(values.begin(), values.end());
        return values.empty() ? 0.0 : values[values.size() / 2];
    };

    auto mean_of = [&](std::size_t begin, std::size_t end) {
        double total = 0.0;
        for (std::size_t i = begin; i < end; ++i) total += points[i].ns_per_access.median;
        return end > begin ? total / static_cast<double>(end - begin) : 0.0;
    };

    // Treat this as a change-point problem rather than a threshold crossing: a
    // first-crossing rule lets one noisy offset anywhere in the flat region
    // decide the answer, which made the estimate jump between 16 and 80 bytes
    // from run to run on the same machine.
    //
    // Score every split with the standard change-point statistic, which rewards
    // a large gap between the two groups *and* balanced group sizes. Scoring on
    // the gap alone is not enough: an early split still shows a large gap
    // because its tail simply mixes in the not-yet-elevated offsets.
    //
    // Splits start at index 3 so both sides always have several points.
    const double n = static_cast<double>(points.size());
    double best_score = 0.0;
    double best_separation = 0.0;
    std::size_t best_split = 0;
    for (std::size_t i = 3; i + 2 < points.size(); ++i) {
        const double head_mean = mean_of(0, i);
        const double tail_mean = mean_of(i, points.size());
        if (head_mean <= 0.0 || tail_mean <= head_mean) continue;

        const double head_count = static_cast<double>(i);
        const double tail_count = n - head_count;
        const double gap = tail_mean - head_mean;
        const double score = head_count * tail_count / n * gap * gap;
        if (score > best_score) {
            best_score = score;
            best_split = i;
            // Accept or reject on medians, which a single slow offset cannot skew.
            const double head_median = median_of(0, i);
            best_separation =
                head_median > 0.0 ? (median_of(i, points.size()) - head_median) / head_median : 0.0;
        }
    }
    // A shared last-level cache is contended by whatever else the machine is
    // doing, so this curve is the noisiest one CacheScope produces. Demand a
    // clearly separated step: reporting "not decisive" costs nothing, while
    // reporting a line size no hardware has is actively misleading.
    if (best_split == 0 || best_separation < 0.20) return 0;

    // Cache lines are powers of two while the probe's offsets are not, so snap
    // down: a boundary observed at offset 80 is the 64-byte line it belongs to.
    return std::bit_floor(static_cast<std::uint32_t>(points[best_split].stride_bytes));
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::uint64_t SweepSeries::x_value(const SweepPoint& p) const {
    switch (x) {
        case XAxis::stride: return p.stride_bytes;
        case XAxis::blocks:
        case XAxis::pages: return p.element_count;
        case XAxis::working_set:
        default: return p.working_set_bytes;
    }
}

const SweepSeries* BenchmarkResult::find(std::string_view id) const {
    for (const auto& s : series) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::string x_axis_name(XAxis axis) {
    switch (axis) {
        case XAxis::stride: return "stride";
        case XAxis::blocks: return "blocks";
        case XAxis::pages: return "pages";
        case XAxis::working_set:
        default: return "working_set";
    }
}

std::string y_axis_name(YAxis axis) {
    return axis == YAxis::gib_per_second ? "gib_per_second" : "ns_per_access";
}

XAxis parse_x_axis(std::string_view name) {
    if (name == "stride") return XAxis::stride;
    if (name == "blocks") return XAxis::blocks;
    if (name == "pages") return XAxis::pages;
    return XAxis::working_set;
}

YAxis parse_y_axis(std::string_view name) {
    return name == "gib_per_second" ? YAxis::gib_per_second : YAxis::ns_per_access;
}

Statistics compute_statistics(std::vector<double> values) {
    Statistics stats;
    if (values.empty()) return stats;
    std::sort(values.begin(), values.end());
    stats.sample_count = values.size();
    stats.minimum = values.front();
    stats.maximum = values.back();
    stats.p05 = quantile_sorted(values, 0.05);
    stats.median = quantile_sorted(values, 0.50);
    stats.p95 = quantile_sorted(values, 0.95);
    stats.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());

    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const double v : values) deviations.push_back(std::abs(v - stats.median));
    std::sort(deviations.begin(), deviations.end());
    stats.mad = quantile_sorted(deviations, 0.50);

    double sq = 0.0;
    for (const double v : values) {
        const double d = v - stats.mean;
        sq += d * d;
    }
    const double variance = sq / static_cast<double>(values.size());
    stats.coefficient_of_variation = stats.mean != 0.0 ? std::sqrt(variance) / std::abs(stats.mean) : 0.0;
    return stats;
}

std::uint64_t detect_knee(const SweepSeries& series, double ratio) {
    if (series.points.size() < 4) return 0;

    // Seed the baseline with the median of the first few points rather than the
    // single first point: the smallest configuration of a probe is often a
    // degenerate special case (one block chasing itself, for instance), and one
    // unusually fast or slow sample there must not decide where the knee is.
    std::vector<double> head;
    for (std::size_t i = 0; i < std::min<std::size_t>(3, series.points.size()); ++i) {
        head.push_back(series.points[i].ns_per_access.median);
    }
    std::sort(head.begin(), head.end());
    double baseline = head[head.size() / 2];

    for (std::size_t i = 1; i + 1 < series.points.size(); ++i) {
        const double current = series.points[i].ns_per_access.median;
        const double following = series.points[i + 1].ns_per_access.median;
        // A knee has to persist into the next point, so that a single noisy
        // sample cannot be mistaken for a hardware boundary.
        if (baseline > 0.0 && current > baseline * ratio &&
            following > baseline * (1.0 + (ratio - 1.0) * 0.7)) {
            return series.x_value(series.points[i]);
        }
        baseline = std::min(baseline, current);
    }
    return 0;
}

BenchmarkConfig preset_config(std::string_view name) {
    BenchmarkConfig c;
    c.preset = std::string(name);
    if (name == "quick") {
        c.samples = 5;
        c.warmup_samples = 1;
        c.target_sample_ms = 6.0;
        c.include_line_probe = false;
        c.include_stride_probe = false;
        c.include_associativity_probe = false;
    } else if (name == "deep") {
        c.samples = 17;
        c.warmup_samples = 4;
        c.target_sample_ms = 60.0;
    } else {
        c.preset = "standard";
        c.samples = 9;
        c.warmup_samples = 2;
        c.target_sample_ms = 20.0;
    }
    return c;
}

std::vector<std::uint64_t> make_working_set_sizes(const SystemInfo& system, const BenchmarkConfig& config) {
    std::uint64_t maximum = config.maximum_working_set_bytes;
    if (maximum == 0) {
        const std::uint64_t l3 = largest_cache(system, 3);
        const std::uint64_t l2 = largest_cache(system, 2);
        const std::uint64_t largest = std::max(l3, l2);
        const std::uint64_t baseline =
            config.preset == "deep" ? 256ull * 1024ull * 1024ull : 128ull * 1024ull * 1024ull;
        const std::uint64_t multiple = config.preset == "deep" ? 4 : 2;
        maximum = std::max(baseline, largest > 0 ? largest * multiple : 0);

        const std::uint64_t cap = sizeof(void*) == 4
                                      ? 192ull * 1024ull * 1024ull
                                      : (config.preset == "deep" ? 512ull * 1024ull * 1024ull
                                                                 : 256ull * 1024ull * 1024ull);
        maximum = std::min(maximum, cap);

        // Never sweep so far that the machine starts paging: peak residency is
        // roughly one working set, and swapping would measure the disk.
        if (system.total_ram_bytes > 0) {
            const std::uint64_t ram_cap =
                std::max<std::uint64_t>(8ull * 1024ull * 1024ull, system.total_ram_bytes / 8);
            maximum = std::min(maximum, ram_cap);
        }
    }
    maximum = std::max(maximum, config.minimum_working_set_bytes);

    std::set<std::uint64_t> sizes;
    const double factor = std::sqrt(2.0);
    double current = static_cast<double>(config.minimum_working_set_bytes);
    while (current <= static_cast<double>(maximum) * 1.001) {
        sizes.insert(std::min(maximum, round_to_64(static_cast<std::uint64_t>(std::llround(current)))));
        current *= factor;
    }
    sizes.insert(maximum);

    // Extra points bracketing every reported cache boundary, so a transition is
    // never hidden between two geometric steps.
    for (int level = 1; level <= 3; ++level) {
        const auto cache = largest_cache(system, level);
        if (cache == 0) continue;
        for (const double ratio : {0.50, 0.75, 1.00, 1.25, 1.50, 2.00}) {
            const auto candidate =
                round_to_64(static_cast<std::uint64_t>(std::llround(static_cast<double>(cache) * ratio)));
            if (candidate >= config.minimum_working_set_bytes && candidate <= maximum) {
                sizes.insert(candidate);
            }
        }
    }
    return {sizes.begin(), sizes.end()};
}

std::vector<std::uint32_t> make_random_cycle(std::size_t element_count, std::uint64_t seed) {
    if (element_count == 0 || element_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("pointer-chase element count must fit in uint32_t");
    }
    std::vector<std::uint32_t> order(element_count);
    std::iota(order.begin(), order.end(), 0u);
    std::mt19937_64 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<std::uint32_t> next(element_count);
    for (std::size_t i = 0; i < element_count; ++i) {
        next[order[i]] = order[(i + 1) % element_count];
    }
    return next;
}

bool validate_single_cycle(const std::vector<std::uint32_t>& next) {
    if (next.empty()) return false;
    std::vector<bool> visited(next.size(), false);
    std::uint32_t current = 0;
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (current >= next.size() || visited[current]) return false;
        visited[current] = true;
        current = next[current];
    }
    return current == 0 && std::all_of(visited.begin(), visited.end(), [](bool v) { return v; });
}

BenchmarkResult run_benchmark(BenchmarkConfig config, const ProgressCallback& progress) {
    if (config.samples < 3) config.samples = 3;
    if (config.warmup_samples < 0) config.warmup_samples = 0;
    if (config.target_sample_ms < 0.25) config.target_sample_ms = 0.25;
    if (config.minimum_working_set_bytes < 1024) config.minimum_working_set_bytes = 1024;

    const auto wall_start = Clock::now();
    BenchmarkResult result;
    result.timestamp_utc = utc_timestamp();
    result.config = config;
    result.system = detect_system();
    result.label = config.label.empty() ? result.system.cpu_name : config.label;

    if (config.pin_thread) {
        if (result.system.affinity_supported) {
            if (!detail::apply_affinity(result.system, config.preferred_cpu)) {
                result.warnings.emplace_back(
                    "CPU affinity was requested but could not be applied; scheduler migration can "
                    "increase noise.");
            }
        } else {
            result.warnings.emplace_back(
                "Strict CPU affinity is unavailable on this operating system; scheduler migration can "
                "increase noise.");
        }
    }
    if (result.system.caches.empty()) {
        result.warnings.emplace_back(
            "Hardware cache geometry could not be queried; cache boundaries are shown only as working-set "
            "sweeps.");
    }
    if (result.system.steady_clock_resolution_ns > 100.0) {
        result.warnings.emplace_back(
            "The observed monotonic-clock resolution is relatively coarse; use the deep preset for stronger "
            "timing stability.");
    }
    if (sizeof(void*) == 4) {
        result.warnings.emplace_back(
            "32-bit address space detected; the automatic working-set ceiling is capped to reduce "
            "allocation failures.");
    }
#if !defined(NDEBUG)
    result.warnings.emplace_back(
        "This binary was built without NDEBUG. Timings from an unoptimized build are not meaningful; "
        "rebuild in Release.");
#endif

    const auto sizes = make_working_set_sizes(result.system, config);
    const auto line_size = representative_line_size(result.system);
    const auto page_size = result.system.page_size_bytes ? result.system.page_size_bytes : 4096ull;

    // Probe sizes are bounded independently of the sweep ceiling.
    const std::uint64_t probe_bytes =
        std::min<std::uint64_t>(sizes.empty() ? 16ull * 1024 * 1024 : sizes.back(), 64ull * 1024 * 1024);

    // The line-pair probe needs a working set that misses the inner caches but
    // still lands in a cache. Run it against DRAM and the neighbouring line is
    // already in the open DRAM row or has been pulled in by a prefetcher, which
    // flattens the curve and hides the boundary entirely.
    const std::uint64_t l3 = largest_cache(result.system, 3);
    const std::uint64_t l2 = largest_cache(result.system, 2);
    std::uint64_t line_probe_bytes = 4ull * 1024 * 1024;
    if (l3 > 0) line_probe_bytes = l3 / 2;
    else if (l2 > 0) line_probe_bytes = l2 * 2;
    line_probe_bytes = std::clamp<std::uint64_t>(line_probe_bytes, 1ull * 1024 * 1024,
                                                 std::min<std::uint64_t>(32ull * 1024 * 1024, probe_bytes));

    const std::array<std::size_t, 4> assoc_strides{4096, 32768, 262144, 2097152};
    const std::size_t assoc_max_blocks = 24;
    const std::size_t tlb_max_pages = sizeof(void*) == 4 ? 2048 : 8192;
    std::size_t tlb_steps = 0;
    for (std::size_t p = 8; p <= tlb_max_pages; p *= 2) ++tlb_steps;

    int total = static_cast<int>(sizes.size()) * (3 + (config.include_copy ? 1 : 0));
    if (config.include_line_probe) total += 18;
    if (config.include_stride_probe) total += 10;
    if (config.include_associativity_probe) {
        total += static_cast<int>(assoc_strides.size() * assoc_max_blocks);
    }
    if (config.include_tlb_probe) total += static_cast<int>(tlb_steps);
    if (config.include_coherency_probe) total += 2;

    int completed = 0;
    bool cancelled = false;
    auto report = [&](const std::string& stage, const std::string& detail) {
        ++completed;
        // Sample the core clock under load. The idle clock read before the run
        // usually understates it badly, and cycle counts derived from the idle
        // clock would be wrong by the turbo ratio.
        if (completed % 8 == 0) {
            result.system.cpu_mhz_peak =
                std::max(result.system.cpu_mhz_peak, detail::current_cpu_mhz(result.system.pinned_cpu));
        }
        if (!progress) return !cancelled;
        if (!progress(Progress{stage, detail, completed, total})) cancelled = true;
        return !cancelled;
    };

    SweepSeries latency{"dependent_latency", "Dependent random-load latency", XAxis::working_set,
                        YAxis::ns_per_access, {}};
    SweepSeries read{"sequential_read", "Sequential read throughput", XAxis::working_set,
                     YAxis::gib_per_second, {}};
    SweepSeries write{"sequential_write", "Sequential write throughput", XAxis::working_set,
                      YAxis::gib_per_second, {}};
    SweepSeries copy{"copy", "memcpy throughput", XAxis::working_set, YAxis::gib_per_second, {}};

    for (const auto bytes : sizes) {
        if (cancelled) break;
        try {
            latency.points.push_back(benchmark_latency(bytes, config, line_size));
            if (!report("dependent latency", format_bytes(bytes))) break;
            read.points.push_back(benchmark_read(bytes, config));
            if (!report("sequential read", format_bytes(bytes))) break;
            write.points.push_back(benchmark_write(bytes, config));
            if (!report("sequential write", format_bytes(bytes))) break;
            if (config.include_copy) {
                copy.points.push_back(benchmark_copy(bytes, config));
                if (!report("memcpy", format_bytes(bytes))) break;
            }
        } catch (const std::bad_alloc&) {
            result.warnings.push_back("Allocation failed at working set " + format_bytes(bytes) +
                                      "; larger sweep points were skipped.");
            break;
        }
    }
    result.series.push_back(std::move(latency));
    result.series.push_back(std::move(read));
    result.series.push_back(std::move(write));
    if (config.include_copy && !copy.points.empty()) result.series.push_back(std::move(copy));

    if (config.include_line_probe && !cancelled) {
        try {
            SweepSeries line{"line_pair", "Serialized cache-line pair probe", XAxis::stride,
                             YAxis::ns_per_access, {}};
            line.points = benchmark_line_probe(line_probe_bytes, line_size, config, [&] {
                return report("line-size probe", "serialized pair");
            });
            result.experimental.line_size_bytes = infer_line_size(line.points);
            if (result.experimental.line_size_bytes == 0) {
                result.warnings.emplace_back(
                    "The experimental cache-line transition was not decisive; prefer the reported line size.");
            } else if (line_size != 0 && result.experimental.line_size_bytes != line_size) {
                result.warnings.emplace_back(
                    "The experimental line transition differs from the reported line size; adjacent-line "
                    "prefetching can shift the observed knee.");
            }
            if (!line.points.empty()) result.series.push_back(std::move(line));
        } catch (const std::bad_alloc&) {
            result.warnings.emplace_back("Line-size probe skipped: buffer allocation failed.");
        }
    }

    if (config.include_stride_probe && !cancelled) {
        try {
            SweepSeries stride{"stride", "Stride / spatial-locality probe", XAxis::stride,
                               YAxis::ns_per_access, {}};
            stride.points = benchmark_stride(probe_bytes, config,
                                             [&] { return report("stride probe", "spatial locality"); });
            result.experimental.spatial_saturation_bytes = infer_fetch_granularity(stride.points);
            if (!stride.points.empty()) result.series.push_back(std::move(stride));
        } catch (const std::bad_alloc&) {
            result.warnings.emplace_back("Stride probe skipped: buffer allocation failed.");
        }
    }

    if (config.include_associativity_probe && !cancelled) {
        for (const auto stride : assoc_strides) {
            if (cancelled) break;
            try {
                SweepSeries assoc;
                assoc.id = "assoc_" + std::to_string(stride);
                assoc.title = "Conflict-miss probe, " + format_bytes(stride) + " stride";
                assoc.x = XAxis::blocks;
                assoc.y = YAxis::ns_per_access;
                assoc.points = benchmark_associativity(stride, assoc_max_blocks, config, [&] {
                    return report("associativity probe", format_bytes(stride) + " stride");
                });
                const auto ways = detect_knee(assoc, 1.6);
                if (ways > 1) {
                    result.experimental.associativity_ways.emplace_back(
                        stride, static_cast<std::uint32_t>(ways - 1));
                }
                if (!assoc.points.empty()) result.series.push_back(std::move(assoc));
            } catch (const std::bad_alloc&) {
                result.warnings.push_back("Associativity probe skipped at " + format_bytes(stride) +
                                          " stride: allocation failed.");
                break;
            }
        }
    }

    if (config.include_tlb_probe && !cancelled) {
        try {
            SweepSeries tlb{"tlb", "TLB reach probe (one line per page)", XAxis::pages,
                            YAxis::ns_per_access, {}};
            tlb.points = benchmark_tlb(page_size, tlb_max_pages, config,
                                       [&] { return report("TLB probe", "one line per page"); });
            result.experimental.tlb_reach_pages = detect_knee(tlb, 1.5);
            if (!tlb.points.empty()) result.series.push_back(std::move(tlb));
        } catch (const std::bad_alloc&) {
            result.warnings.emplace_back("TLB probe skipped: buffer allocation failed.");
        }
    }

    if (config.include_coherency_probe && !cancelled && result.system.logical_cpus >= 2) {
        const auto cpus = detail::allowed_cpus();
        const bool pin = result.system.affinity_supported && config.pin_thread && cpus.size() >= 2;
        if (cpus.size() >= 2) {
            const int cpu_a = cpus.front();
            result.coherency.pairs.push_back(measure_pair(
                "adjacent CPU (often an SMT sibling)", cpu_a, cpus[1], pin, config));
            report("coherency probe", "adjacent CPU");
            if (cpus.size() >= 4) {
                result.coherency.pairs.push_back(
                    measure_pair("distant CPU (different physical core)", cpu_a, cpus.back(), pin, config));
                report("coherency probe", "distant CPU");
            }
            result.coherency.measured = true;
            if (!pin) {
                result.warnings.emplace_back(
                    "The coherency probe could not pin its worker threads; the operating system chose "
                    "where they ran, so the CPU pair labels are indicative only.");
            }
        }
        // Re-pin the main thread: the probe threads are separate, but on some
        // platforms the pool used above can perturb the caller's placement.
        if (result.system.affinity_applied) detail::pin_current_thread(result.system.pinned_cpu);
    }

    if (cancelled) {
        result.warnings.emplace_back("The run was cancelled; partial results are reported.");
    }

    result.system.cpu_mhz_end = detail::current_cpu_mhz(result.system.pinned_cpu);
    result.system.cpu_mhz_peak = std::max({result.system.cpu_mhz_peak, result.system.cpu_mhz_start,
                                           result.system.cpu_mhz_end});
    // Cycle counts use the highest clock observed under load, which is the one
    // the measured code actually ran at.
    const double ghz = result.system.cpu_mhz_peak / 1000.0;
    if (result.system.cpu_mhz_peak > 0.0 && result.system.cpu_mhz_start > 0.0) {
        const double spread =
            (result.system.cpu_mhz_peak - std::min(result.system.cpu_mhz_start, result.system.cpu_mhz_end)) /
            result.system.cpu_mhz_peak;
        if (spread > 0.10) {
            result.warnings.emplace_back(
                "The core clock moved by more than 10% around this run (turbo, thermal or governor "
                "effects). Nanoseconds are measured directly; cycle counts are derived from the reported "
                "clock and are therefore approximate.");
        }
    } else {
        result.warnings.emplace_back(
            "This operating system did not report a core frequency, so the report shows nanoseconds only "
            "and no cycle counts.");
    }

    // If the OS never moved its reported clock, it is handing back a nominal
    // value. The core almost certainly boosted above it under load, which makes
    // every derived cycle count a lower bound. Say so instead of implying
    // precision that is not there.
    result.system.cpu_mhz_max = detail::maximum_cpu_mhz(result.system.pinned_cpu);
    if (result.system.cpu_mhz_peak > 0.0 && result.system.cpu_mhz_max > result.system.cpu_mhz_peak * 1.05 &&
        result.system.cpu_mhz_peak <= result.system.cpu_mhz_start * 1.01) {
        std::ostringstream message;
        message << "The OS reported a static core clock of " << static_cast<long long>(result.system.cpu_mhz_peak)
                << " MHz even under load, while the hardware maximum is "
                << static_cast<long long>(result.system.cpu_mhz_max)
                << " MHz. Nanosecond values are measured directly and are unaffected, but cycle counts "
                   "assume the static value and are therefore a lower bound.";
        result.warnings.push_back(message.str());
    }

    // A hybrid CPU reports several different geometries for the same level, so
    // which core the run landed on changes the answer.
    for (int level = 1; level <= 3; ++level) {
        std::set<std::uint64_t> distinct;
        for (const auto& c : result.system.caches) {
            if (c.level == level && (c.type == CacheType::data || c.type == CacheType::unified)) {
                distinct.insert(c.size_bytes);
            }
        }
        if (distinct.size() > 1) {
            result.warnings.push_back(
                "This CPU reports more than one L" + std::to_string(level) +
                " geometry, which means it has more than one core type. Results depend on which core "
                "class the run was pinned to; use --cpu to compare like with like.");
            break;
        }
    }

    if (const auto* lat = result.find("dependent_latency")) {
        compute_latency_escalation(*lat, result.experimental);
    }
    result.level_summary = summarize_levels(result, ghz);
    result.duration_seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();
    return result;
}

} // namespace cachescope
