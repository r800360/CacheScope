#pragma once

// CacheScope measurement engine.
//
// This header is the entire public surface of the headless benchmark library.
// It has no third-party dependencies so that the CLI can be built on any
// conforming C++20 (or newer) toolchain without a package manager.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cachescope {

inline constexpr std::string_view kResultSchema = "cachescope-result-v2";
inline constexpr std::string_view kVersion = "1.1.0";

enum class CacheType { data, instruction, unified, unknown };

// Where a piece of hardware-reported geometry came from. Experimental values
// are never mixed into these fields; they are reported separately.
enum class GeometrySource { none, operating_system, cpuid };

struct CacheInfo {
    int level = 0;
    CacheType type = CacheType::unknown;
    std::uint64_t size_bytes = 0;
    std::uint32_t line_size_bytes = 0;
    std::uint32_t sharing_threads = 0;
    std::uint32_t instances = 1;
    std::uint32_t associativity_ways = 0;  // 0 = unknown, 0xFFFFFFFF = fully associative
    GeometrySource source = GeometrySource::none;
};

struct SystemInfo {
    std::string cpu_name;
    std::string architecture;
    std::string operating_system;
    std::string compiler;
    std::string build_flavor;  // "optimized (NDEBUG)" or "unoptimized/debug"
    int pointer_bits = 0;
    unsigned logical_cpus = 0;
    std::uint64_t page_size_bytes = 0;
    std::uint64_t total_ram_bytes = 0;
    double steady_clock_resolution_ns = 0.0;
    bool affinity_supported = false;
    bool affinity_applied = false;
    int pinned_cpu = -1;
    double cpu_mhz_start = 0.0;  // 0 = unknown, sampled before the run (idle clock)
    double cpu_mhz_peak = 0.0;   // highest value seen while the benchmark was running
    double cpu_mhz_end = 0.0;    // sampled after the run, to expose clock drift
    double cpu_mhz_max = 0.0;    // hardware maximum (turbo) as reported by the OS
    std::vector<CacheInfo> caches;
    std::vector<std::string> notes;  // OS-level context (THP, virtualization hints, ...)
};

struct Statistics {
    double minimum = 0.0;
    double p05 = 0.0;
    double median = 0.0;
    double mean = 0.0;
    double p95 = 0.0;
    double maximum = 0.0;
    double mad = 0.0;
    double coefficient_of_variation = 0.0;
    std::size_t sample_count = 0;
};

struct SweepPoint {
    std::uint64_t working_set_bytes = 0;  // total memory footprint touched
    std::uint64_t stride_bytes = 0;       // access stride / probe offset, when meaningful
    std::uint64_t element_count = 0;      // blocks, pages, or nodes, when meaningful
    Statistics ns_per_access;
    Statistics gib_per_second;
    std::uint64_t operations_per_sample = 0;
};

// Which column of SweepPoint carries the independent variable for a series.
enum class XAxis { working_set, stride, blocks, pages };
enum class YAxis { ns_per_access, gib_per_second };

struct SweepSeries {
    std::string id;     // stable machine-readable key, also the CSV "probe" value
    std::string title;  // human-readable chart title
    XAxis x = XAxis::working_set;
    YAxis y = YAxis::ns_per_access;
    std::vector<SweepPoint> points;

    [[nodiscard]] std::uint64_t x_value(const SweepPoint& p) const;
};

// Two-thread cache-coherence experiment. Measures the cost of a contended
// cache line versus one private line per thread.
struct CoherencyPair {
    std::string label;   // "sibling (adjacent CPU)" / "distant CPU"
    int cpu_a = -1;
    int cpu_b = -1;
    bool pinned = false;
    Statistics shared_line_ns;   // two threads, atomics in the SAME cache line
    Statistics padded_line_ns;   // two threads, atomics in DIFFERENT cache lines
    Statistics single_thread_ns; // one thread, uncontended baseline
    Statistics ping_pong_ns;     // round-trip store/observe latency between the two CPUs
};

struct CoherencyResult {
    bool measured = false;
    std::vector<CoherencyPair> pairs;
};

struct LevelSummary {
    std::string label;
    std::uint64_t reported_size_bytes = 0;
    std::uint32_t reported_line_size_bytes = 0;
    std::uint32_t reported_ways = 0;
    std::uint64_t representative_working_set_bytes = 0;
    double latency_ns = 0.0;
    double latency_cycles = 0.0;  // 0 when the core frequency is unknown
    double sequential_read_gib_s = 0.0;
    double sequential_write_gib_s = 0.0;
    double copy_gib_s = 0.0;
};

// Findings produced by the experiments themselves, kept strictly separate from
// the hardware-reported geometry above.
struct Experimental {
    std::uint32_t line_size_bytes = 0; // from the serialized line-pair probe
    // Stride beyond which each touched element costs a full transfer. It is an
    // upper bound on the cache line size, inflated wherever a hardware
    // prefetcher keeps larger strides cheap.
    std::uint32_t spatial_saturation_bytes = 0;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> associativity_ways;  // stride -> observed ways
    std::uint64_t tlb_reach_pages = 0;  // pages touched before the first latency knee
    // Working set at which dependent latency first reached 2x, 4x, 8x, ... the
    // fastest measured access. Unlike "knee" detection this is well defined even
    // on the smooth curves modern CPUs produce, so it is reproducible and
    // directly comparable between machines.
    std::vector<std::pair<std::uint32_t, std::uint64_t>> latency_escalation;
    double fastest_latency_ns = 0.0;
};

struct BenchmarkConfig {
    std::string preset = "standard";
    std::string label;  // free-form machine label used in reports and comparisons
    std::uint64_t minimum_working_set_bytes = 4ull * 1024ull;
    std::uint64_t maximum_working_set_bytes = 0;  // 0 = auto
    int samples = 9;
    int warmup_samples = 2;
    double target_sample_ms = 20.0;
    bool pin_thread = true;
    int preferred_cpu = -1;
    bool include_copy = true;
    bool include_line_probe = true;
    bool include_stride_probe = true;
    bool include_associativity_probe = true;
    bool include_tlb_probe = true;
    bool include_coherency_probe = true;
};

struct BenchmarkResult {
    std::string schema{kResultSchema};
    std::string version{kVersion};
    std::string timestamp_utc;
    std::string label;
    double duration_seconds = 0.0;
    SystemInfo system;
    BenchmarkConfig config;
    std::vector<SweepSeries> series;
    CoherencyResult coherency;
    Experimental experimental;
    std::vector<LevelSummary> level_summary;
    std::vector<std::string> warnings;

    [[nodiscard]] const SweepSeries* find(std::string_view id) const;
};

struct Progress {
    std::string stage;
    std::string detail;
    int completed = 0;
    int total = 0;
};

// Return false from the callback to request cancellation. A cancelled run still
// returns every measurement completed so far.
using ProgressCallback = std::function<bool(const Progress&)>;

enum class ReportFormat : unsigned {
    none = 0u,
    html = 1u << 0,
    csv = 1u << 1,
    json = 1u << 2,
    markdown = 1u << 3,
    all = html | csv | json | markdown
};

[[nodiscard]] constexpr ReportFormat operator|(ReportFormat a, ReportFormat b) {
    return static_cast<ReportFormat>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
[[nodiscard]] constexpr bool has_format(ReportFormat set, ReportFormat one) {
    return (static_cast<unsigned>(set) & static_cast<unsigned>(one)) != 0u;
}

struct ReportPaths {
    std::filesystem::path json;
    std::filesystem::path csv;
    std::filesystem::path html;
    std::filesystem::path markdown;
};

[[nodiscard]] BenchmarkConfig preset_config(std::string_view name);
[[nodiscard]] SystemInfo detect_system();
[[nodiscard]] Statistics compute_statistics(std::vector<double> values);
[[nodiscard]] std::vector<std::uint64_t> make_working_set_sizes(const SystemInfo& system,
                                                                const BenchmarkConfig& config);
[[nodiscard]] BenchmarkResult run_benchmark(BenchmarkConfig config,
                                            const ProgressCallback& progress = {});
[[nodiscard]] ReportPaths write_reports(const BenchmarkResult& result,
                                        const std::filesystem::path& output_directory,
                                        ReportFormat formats = ReportFormat::all);

[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] std::string cache_type_name(CacheType type);
[[nodiscard]] std::string geometry_source_name(GeometrySource source);
[[nodiscard]] std::string x_axis_name(XAxis axis);
[[nodiscard]] std::string y_axis_name(YAxis axis);
[[nodiscard]] XAxis parse_x_axis(std::string_view name);
[[nodiscard]] YAxis parse_y_axis(std::string_view name);

// Exposed for deterministic unit tests.
[[nodiscard]] bool validate_single_cycle(const std::vector<std::uint32_t>& next);
[[nodiscard]] std::vector<std::uint32_t> make_random_cycle(std::size_t element_count,
                                                           std::uint64_t seed);
// Returns the x-value of the first point whose median is a sustained `ratio`
// jump above the running baseline, or 0 when no such knee exists.
[[nodiscard]] std::uint64_t detect_knee(const SweepSeries& series, double ratio);

} // namespace cachescope
