#include <cachescope/benchmark.hpp>
#include <cachescope/compare.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace {

void usage() {
    std::cout << R"(CacheScope - portable CPU cache characterization

Usage:
  cachescope_cli [options]

Run selection
  --preset quick|standard|deep  Accuracy/runtime preset (default: standard)
  --label NAME                  Machine label used in reports and comparisons
  --out DIR                     Report directory (default: results)
  --repeat N                    Run the whole benchmark N times and also emit a
                                comparison of the runs (default: 1)
  --formats LIST                Comma-separated: html,csv,json,md,all (default: all)

Sweep control
  --min-kib N                   Smallest working set (default: 4)
  --max-mib N                   Largest working set (default: automatic)
  --samples N                   Samples per point (minimum 3)
  --sample-ms N                 Target duration of each timing sample
  --cpu N                       Prefer logical CPU N for the benchmark thread
  --no-pin                      Do not pin the benchmark thread

Experiment selection
  --no-copy                     Skip memcpy throughput
  --no-line                     Skip the serialized cache-line probe
  --no-stride                   Skip the stride / spatial-locality probe
  --no-assoc                    Skip the conflict-miss (associativity) probe
  --no-tlb                      Skip the TLB reach probe
  --no-coherency                Skip the two-thread false-sharing probe
  --only-sweeps                 Only latency and throughput sweeps

Output
  --quiet                       No progress output
  --open                        Open the HTML report when finished
  --info                        Print detected hardware and exit
  -h, --help                    Show this help
  --version                     Print version and exit

Recommended comparable run:
  cachescope_cli --preset deep --repeat 3 --label "my laptop" --out results
)";
}

std::string require_value(int& i, int argc, char** argv, std::string_view option) {
    if (++i >= argc) throw std::runtime_error("missing value for " + std::string(option));
    return argv[i];
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == separator) {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

cachescope::ReportFormat parse_formats(std::string_view text) {
    auto formats = cachescope::ReportFormat::none;
    for (const auto& part : split(text, ',')) {
        if (part == "all") formats = formats | cachescope::ReportFormat::all;
        else if (part == "html") formats = formats | cachescope::ReportFormat::html;
        else if (part == "csv") formats = formats | cachescope::ReportFormat::csv;
        else if (part == "json") formats = formats | cachescope::ReportFormat::json;
        else if (part == "md" || part == "markdown") formats = formats | cachescope::ReportFormat::markdown;
        else throw std::runtime_error("unknown report format: " + part);
    }
    if (formats == cachescope::ReportFormat::none) throw std::runtime_error("--formats selected nothing");
    return formats;
}

void print_system(const cachescope::SystemInfo& system) {
    std::cout << "  CPU:   " << system.cpu_name << '\n'
              << "  OS:    " << system.operating_system << '\n'
              << "  Arch:  " << system.architecture << " (" << system.pointer_bits << "-bit), "
              << system.logical_cpus << " logical CPUs\n"
              << "  Page:  " << cachescope::format_bytes(system.page_size_bytes);
    if (system.total_ram_bytes) std::cout << ", RAM " << cachescope::format_bytes(system.total_ram_bytes);
    std::cout << '\n' << "  Build: " << system.compiler << ", " << system.build_flavor << '\n';
    if (system.cpu_mhz_start > 0.0) {
        std::cout << "  Clock: " << static_cast<long long>(system.cpu_mhz_start) << " MHz (as reported now)\n";
    }
    if (system.caches.empty()) {
        std::cout << "  Reported caches: none exposed by this OS\n";
    } else {
        std::cout << "  Reported caches:\n";
        for (const auto& c : system.caches) {
            std::cout << "    L" << c.level << ' ' << cachescope::cache_type_name(c.type) << ": "
                      << cachescope::format_bytes(c.size_bytes) << ", line " << c.line_size_bytes << " B";
            if (c.associativity_ways == 0xFFFFFFFFu) std::cout << ", fully associative";
            else if (c.associativity_ways) std::cout << ", " << c.associativity_ways << "-way";
            if (c.sharing_threads) std::cout << ", shared by " << c.sharing_threads;
            if (c.instances > 1) std::cout << ", " << c.instances << " instances";
            std::cout << " [" << cachescope::geometry_source_name(c.source) << "]\n";
        }
    }
    for (const auto& note : system.notes) std::cout << "  note: " << note << '\n';
}

void open_in_browser(const std::filesystem::path& path) {
    if (path.empty()) return;
    const auto absolute = std::filesystem::absolute(path).string();
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", absolute.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    const auto command = "open \"" + absolute + "\" >/dev/null 2>&1 &";
    if (std::system(command.c_str()) != 0) std::cerr << "Could not open " << absolute << '\n';
#else
    const auto command = "xdg-open \"" + absolute + "\" >/dev/null 2>&1 &";
    if (std::system(command.c_str()) != 0) std::cerr << "Could not open " << absolute << '\n';
#endif
}

class ProgressPrinter {
public:
    explicit ProgressPrinter(bool enabled) : enabled_(enabled) {}

    bool operator()(const cachescope::Progress& p) {
        if (!enabled_) return true;
        const auto now = std::chrono::steady_clock::now();
        const bool last = p.total > 0 && p.completed >= p.total;
        if (!last && now - last_draw_ < std::chrono::milliseconds(120)) return true;
        last_draw_ = now;

        const int percent = p.total > 0 ? static_cast<int>(100.0 * p.completed / p.total) : 0;
        constexpr int width = 24;
        const int filled = p.total > 0 ? std::min(width, percent * width / 100) : 0;
        std::ostringstream line;
        line << '\r' << '[';
        for (int i = 0; i < width; ++i) line << (i < filled ? '#' : '.');
        line << "] " << std::setw(3) << percent << "%  " << p.stage;
        if (!p.detail.empty()) line << " (" << p.detail << ')';
        std::string text = line.str();
        // Pad so shorter stage names fully erase the previous line.
        if (text.size() < previous_width_) text.append(previous_width_ - text.size(), ' ');
        previous_width_ = std::max<std::size_t>(text.size(), 1);
        std::cerr << text << std::flush;
        return true;
    }

    void finish() const {
        if (enabled_) std::cerr << '\r' << std::string(previous_width_, ' ') << '\r' << std::flush;
    }

private:
    bool enabled_;
    std::size_t previous_width_ = 0;
    std::chrono::steady_clock::time_point last_draw_{};
};

} // namespace

int main(int argc, char** argv) {
    try {
        std::string preset = "standard";
        std::filesystem::path out_dir = "results";
        auto formats = cachescope::ReportFormat::all;
        int repeat = 1;
        bool quiet = false;
        bool open_report = false;

        // Parse --preset first so that later explicit overrides always win.
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--preset") preset = require_value(i, argc, argv, "--preset");
        }
        if (preset != "quick" && preset != "standard" && preset != "deep") {
            throw std::runtime_error("unknown preset '" + preset + "' (use quick, standard or deep)");
        }
        auto config = cachescope::preset_config(preset);

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--preset") ++i;
            else if (arg == "--label") config.label = require_value(i, argc, argv, arg);
            else if (arg == "--out") out_dir = require_value(i, argc, argv, arg);
            else if (arg == "--repeat") repeat = std::stoi(require_value(i, argc, argv, arg));
            else if (arg == "--formats") formats = parse_formats(require_value(i, argc, argv, arg));
            else if (arg == "--max-mib")
                config.maximum_working_set_bytes = std::stoull(require_value(i, argc, argv, arg)) * 1024ull * 1024ull;
            else if (arg == "--min-kib")
                config.minimum_working_set_bytes = std::stoull(require_value(i, argc, argv, arg)) * 1024ull;
            else if (arg == "--samples") config.samples = std::stoi(require_value(i, argc, argv, arg));
            else if (arg == "--sample-ms") config.target_sample_ms = std::stod(require_value(i, argc, argv, arg));
            else if (arg == "--cpu") config.preferred_cpu = std::stoi(require_value(i, argc, argv, arg));
            else if (arg == "--no-pin") config.pin_thread = false;
            else if (arg == "--no-copy") config.include_copy = false;
            else if (arg == "--no-line") config.include_line_probe = false;
            else if (arg == "--no-stride") config.include_stride_probe = false;
            else if (arg == "--no-assoc") config.include_associativity_probe = false;
            else if (arg == "--no-tlb") config.include_tlb_probe = false;
            else if (arg == "--no-coherency") config.include_coherency_probe = false;
            else if (arg == "--only-sweeps") {
                config.include_line_probe = false;
                config.include_stride_probe = false;
                config.include_associativity_probe = false;
                config.include_tlb_probe = false;
                config.include_coherency_probe = false;
            } else if (arg == "--quiet") quiet = true;
            else if (arg == "--open") open_report = true;
            else if (arg == "--info") {
                std::cout << "CacheScope " << cachescope::kVersion << "\n";
                print_system(cachescope::detect_system());
                return 0;
            } else if (arg == "--version") {
                std::cout << "cachescope " << cachescope::kVersion << '\n';
                return 0;
            } else if (arg == "-h" || arg == "--help") {
                usage();
                return 0;
            } else {
                throw std::runtime_error("unknown option: " + std::string(arg));
            }
        }
        if (repeat < 1) throw std::runtime_error("--repeat must be at least 1");

        const auto system = cachescope::detect_system();
        std::cout << "CacheScope " << cachescope::kVersion << '\n';
        print_system(system);
        std::cout << "  Mode:  " << config.preset << ", " << config.samples << " samples/point, ~"
                  << config.target_sample_ms << " ms/sample";
        if (repeat > 1) std::cout << ", " << repeat << " runs";
        std::cout << "\n\nFor comparable results keep the machine idle and on a stable power profile.\n";

        std::vector<std::filesystem::path> csv_reports;
        std::filesystem::path last_html;

        for (int run_index = 1; run_index <= repeat; ++run_index) {
            if (repeat > 1) std::cout << "\nRun " << run_index << " of " << repeat << '\n';
            ProgressPrinter printer(!quiet);
            auto result = cachescope::run_benchmark(config, std::ref(printer));
            printer.finish();

            const auto reports = cachescope::write_reports(result, out_dir, formats);
            if (!reports.csv.empty()) csv_reports.push_back(reports.csv);
            if (!reports.html.empty()) last_html = reports.html;

            std::cout << "\nSummary for " << result.label << " (" << std::fixed << std::setprecision(1)
                      << result.duration_seconds << " s):\n";
            for (const auto& s : result.level_summary) {
                std::cout << "  " << std::left << std::setw(20) << s.label << std::right
                          << " set " << std::setw(10) << cachescope::format_bytes(s.representative_working_set_bytes)
                          << "   latency " << std::setw(8) << std::setprecision(2) << s.latency_ns << " ns";
                if (s.latency_cycles > 0.0) {
                    std::cout << " (" << std::setprecision(1) << s.latency_cycles << " cyc)";
                }
                std::cout << "   read " << std::setprecision(2) << s.sequential_read_gib_s
                          << " GiB/s   write " << s.sequential_write_gib_s << " GiB/s";
                if (s.copy_gib_s > 0.0) std::cout << "   copy " << s.copy_gib_s << " GiB/s";
                std::cout << '\n';
            }
            if (result.experimental.line_size_bytes) {
                std::cout << "  experimental line size: " << result.experimental.line_size_bytes << " B\n";
            }
            if (result.experimental.spatial_saturation_bytes) {
                std::cout << "  spatial-locality saturation stride: "
                          << result.experimental.spatial_saturation_bytes << " B\n";
            }
            if (result.experimental.tlb_reach_pages) {
                std::cout << "  experimental TLB reach knee: " << result.experimental.tlb_reach_pages
                          << " pages\n";
            }
            for (const auto& [stride, ways] : result.experimental.associativity_ways) {
                std::cout << "  conflict knee at " << cachescope::format_bytes(stride) << " stride: " << ways
                          << " blocks\n";
            }
            for (const auto& pair : result.coherency.pairs) {
                std::cout << "  coherency, " << pair.label << ": same line "
                          << std::setprecision(2) << pair.shared_line_ns.median << " ns vs separate lines "
                          << pair.padded_line_ns.median << " ns\n";
            }
            for (const auto& warning : result.warnings) std::cout << "  warning: " << warning << '\n';

            std::cout << "  reports:";
            for (const auto* path : {&reports.html, &reports.csv, &reports.json, &reports.markdown}) {
                if (!path->empty()) std::cout << ' ' << path->filename().string();
            }
            std::cout << "\n  in " << std::filesystem::absolute(out_dir).string() << '\n';
        }

        if (csv_reports.size() > 1) {
            std::vector<cachescope::ComparisonRun> runs;
            for (const auto& path : csv_reports) runs.push_back(cachescope::read_csv_run(path));
            const auto comparison = out_dir / "comparison_of_runs.html";
            cachescope::write_comparison(runs, comparison);
            std::cout << "\nRun-to-run comparison: " << std::filesystem::absolute(comparison).string()
                      << "\nCurves that do not overlap indicate an unstable machine, not a hardware "
                         "difference.\n";
        }

        if (open_report && !last_html.empty()) open_in_browser(last_html);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "CacheScope error: " << e.what() << '\n';
        return 1;
    }
}
