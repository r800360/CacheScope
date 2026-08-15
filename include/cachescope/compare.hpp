#pragma once

// Cross-machine comparison: reads CacheScope CSV reports back in and renders a
// single HTML page with overlaid curves. Lives in the core library so it can be
// unit tested and reused by the GUI, not only by cachescope_compare.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cachescope {

struct ComparisonPoint {
    std::uint64_t x = 0;
    double y = 0.0;
};

struct ComparisonSeries {
    std::string title;
    std::string x_axis;  // working_set | stride | blocks | pages
    std::string y_axis;  // ns_per_access | gib_per_second
    std::vector<ComparisonPoint> points;
};

struct ComparisonRun {
    std::string label;
    std::string cpu;
    std::string os;
    std::string architecture;
    std::string pointer_bits;
    std::string compiler;
    std::string preset;
    std::string source_file;
    std::uint64_t l1d_bytes = 0;
    std::uint64_t l2_bytes = 0;
    std::uint64_t l3_bytes = 0;
    std::uint64_t line_bytes = 0;
    double cpu_mhz = 0.0;
    std::map<std::string, ComparisonSeries> probes;  // keyed by probe id
};

// Throws std::runtime_error when the file is missing, empty, or not a
// CacheScope CSV.
[[nodiscard]] ComparisonRun read_csv_run(const std::filesystem::path& path);

// Expands directories into the CSV files they contain, sorted by name.
[[nodiscard]] std::vector<std::filesystem::path> expand_csv_inputs(
    const std::vector<std::filesystem::path>& inputs);

void write_comparison(const std::vector<ComparisonRun>& runs, const std::filesystem::path& output);

} // namespace cachescope
