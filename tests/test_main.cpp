// Dependency-free test runner. Kept deliberately small so that the test binary
// builds anywhere the library itself builds.

#include <cachescope/benchmark.hpp>
#include <cachescope/compare.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(expr)) {                                                           \
            std::cerr << "CHECK failed: " #expr " at " << __FILE__ << ':'        \
                      << __LINE__ << '\n';                                       \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace cachescope;

void test_statistics() {
    const auto s = compute_statistics({1.0, 2.0, 3.0, 4.0, 100.0});
    CHECK(s.sample_count == 5);
    CHECK(std::abs(s.median - 3.0) < 1e-12);
    CHECK(std::abs(s.minimum - 1.0) < 1e-12);
    CHECK(std::abs(s.maximum - 100.0) < 1e-12);
    CHECK(std::abs(s.mad - 1.0) < 1e-12);
    CHECK(s.p95 > s.median);
    CHECK(s.mean > s.median);  // the outlier pulls the mean up but not the median
    CHECK(s.coefficient_of_variation > 0.0);

    const auto empty = compute_statistics({});
    CHECK(empty.sample_count == 0);

    const auto single = compute_statistics({7.5});
    CHECK(single.sample_count == 1);
    CHECK(std::abs(single.median - 7.5) < 1e-12);
    CHECK(std::abs(single.coefficient_of_variation) < 1e-12);
}

void test_cycle() {
    for (const std::size_t n : {1u, 2u, 3u, 64u, 8192u}) {
        const auto cycle = make_random_cycle(n, 12345 + n);
        CHECK(cycle.size() == n);
        CHECK(validate_single_cycle(cycle));
    }
    auto broken = make_random_cycle(8192, 1);
    broken[0] = 0;  // self-loop: no longer a single Hamiltonian cycle
    CHECK(!validate_single_cycle(broken));
    CHECK(!validate_single_cycle({}));

    bool threw = false;
    try {
        (void)make_random_cycle(0, 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void test_size_generation() {
    SystemInfo system;
    system.caches = {
        {1, CacheType::data, 32 * 1024, 64, 1, 1, 8, GeometrySource::operating_system},
        {2, CacheType::unified, 256 * 1024, 64, 1, 1, 4, GeometrySource::operating_system},
        {3, CacheType::unified, 8 * 1024 * 1024, 64, 8, 1, 16, GeometrySource::operating_system}};
    auto cfg = preset_config("quick");
    cfg.maximum_working_set_bytes = 16 * 1024 * 1024;
    const auto sizes = make_working_set_sizes(system, cfg);
    CHECK(!sizes.empty());
    CHECK(std::is_sorted(sizes.begin(), sizes.end()));
    CHECK(std::adjacent_find(sizes.begin(), sizes.end()) == sizes.end());  // no duplicates
    CHECK(sizes.front() >= cfg.minimum_working_set_bytes);
    CHECK(sizes.back() == cfg.maximum_working_set_bytes);
    // Every reported cache boundary must appear as an explicit sweep point.
    CHECK(std::find(sizes.begin(), sizes.end(), 32u * 1024) != sizes.end());
    CHECK(std::find(sizes.begin(), sizes.end(), 256u * 1024) != sizes.end());
    CHECK(std::find(sizes.begin(), sizes.end(), 8u * 1024 * 1024) != sizes.end());

    // A machine that reports nothing must still get a usable sweep.
    SystemInfo blank;
    auto blank_cfg = preset_config("quick");
    blank_cfg.maximum_working_set_bytes = 1024 * 1024;
    const auto blank_sizes = make_working_set_sizes(blank, blank_cfg);
    CHECK(blank_sizes.size() > 4);
    CHECK(blank_sizes.back() == blank_cfg.maximum_working_set_bytes);

    // Small installed RAM must cap the automatic ceiling.
    SystemInfo tiny_ram = system;
    tiny_ram.total_ram_bytes = 512ull * 1024 * 1024;
    auto auto_cfg = preset_config("standard");
    const auto capped = make_working_set_sizes(tiny_ram, auto_cfg);
    CHECK(capped.back() <= tiny_ram.total_ram_bytes / 8);
}

void test_presets() {
    CHECK(preset_config("quick").samples < preset_config("standard").samples);
    CHECK(preset_config("standard").samples < preset_config("deep").samples);
    CHECK(preset_config("nonsense").preset == "standard");  // unknown names fall back
    CHECK(preset_config("deep").include_associativity_probe);
    CHECK(!preset_config("quick").include_associativity_probe);
}

void test_knee_detection() {
    // A synthetic curve that is flat, steps up, then is flat again.
    SweepSeries series;
    series.id = "synthetic";
    series.x = XAxis::blocks;
    series.y = YAxis::ns_per_access;
    for (std::uint64_t blocks = 1; blocks <= 16; ++blocks) {
        SweepPoint p;
        p.element_count = blocks;
        p.ns_per_access.median = blocks <= 8 ? 1.0 : 10.0;
        series.points.push_back(p);
    }
    CHECK(detect_knee(series, 1.6) == 9);  // first block count that thrashes

    SweepSeries flat = series;
    for (auto& p : flat.points) p.ns_per_access.median = 1.0;
    CHECK(detect_knee(flat, 1.6) == 0);  // no knee must not invent one
}

void test_axis_names() {
    CHECK(parse_x_axis(x_axis_name(XAxis::stride)) == XAxis::stride);
    CHECK(parse_x_axis(x_axis_name(XAxis::pages)) == XAxis::pages);
    CHECK(parse_x_axis(x_axis_name(XAxis::blocks)) == XAxis::blocks);
    CHECK(parse_x_axis(x_axis_name(XAxis::working_set)) == XAxis::working_set);
    CHECK(parse_y_axis(y_axis_name(YAxis::gib_per_second)) == YAxis::gib_per_second);
    CHECK(parse_y_axis(y_axis_name(YAxis::ns_per_access)) == YAxis::ns_per_access);
    CHECK(format_bytes(1024) == "1.0 KiB");
    CHECK(format_bytes(512) == "512 B");
}

BenchmarkConfig smoke_config() {
    auto cfg = preset_config("quick");
    cfg.label = "unit test";
    cfg.minimum_working_set_bytes = 4 * 1024;
    cfg.maximum_working_set_bytes = 64 * 1024;
    cfg.samples = 3;
    cfg.warmup_samples = 0;
    cfg.target_sample_ms = 0.5;
    cfg.include_copy = true;
    cfg.include_line_probe = false;
    cfg.include_stride_probe = false;
    cfg.include_associativity_probe = false;
    cfg.include_tlb_probe = false;
    cfg.include_coherency_probe = false;
    cfg.pin_thread = false;
    return cfg;
}

void test_benchmark_and_reports() {
    int progress_calls = 0;
    int last_completed = 0;
    const auto result = run_benchmark(smoke_config(), [&](const Progress& p) {
        ++progress_calls;
        CHECK(p.completed > last_completed);
        CHECK(p.total > 0);
        last_completed = p.completed;
        return true;
    });
    CHECK(progress_calls > 0);
    CHECK(!result.label.empty());
    CHECK(result.duration_seconds > 0.0);
    CHECK(result.schema == kResultSchema);

    const auto* latency = result.find("dependent_latency");
    const auto* read = result.find("sequential_read");
    const auto* write = result.find("sequential_write");
    const auto* copy = result.find("copy");
    CHECK(latency != nullptr);
    CHECK(read != nullptr);
    CHECK(write != nullptr);
    CHECK(copy != nullptr);
    CHECK(result.find("does_not_exist") == nullptr);
    if (!latency || !read || !write) return;

    CHECK(!latency->points.empty());
    CHECK(latency->points.size() == read->points.size());
    CHECK(latency->points.size() == write->points.size());
    for (const auto& p : latency->points) {
        CHECK(p.ns_per_access.median > 0.0);
        CHECK(p.ns_per_access.sample_count >= 3);
        CHECK(p.operations_per_sample > 0);
        // Latency probes report no bandwidth, throughput probes must.
        CHECK(p.gib_per_second.median == 0.0);
    }
    for (const auto& p : read->points) {
        CHECK(p.gib_per_second.median > 0.0);
        // A single core cannot plausibly exceed a few TiB/s; a much larger value
        // means the optimizer deleted the loop.
        CHECK(p.gib_per_second.median < 8192.0);
    }
    for (const auto& p : copy->points) CHECK(p.gib_per_second.median > 0.0);
    CHECK(std::is_sorted(latency->points.begin(), latency->points.end(),
                         [](const SweepPoint& a, const SweepPoint& b) {
                             return a.working_set_bytes < b.working_set_bytes;
                         }));

    const auto temp = std::filesystem::temp_directory_path() / "cachescope_test_report";
    std::error_code cleanup_ec;
    std::filesystem::remove_all(temp, cleanup_ec);
    const auto paths = write_reports(result, temp);
    CHECK(std::filesystem::file_size(paths.json) > 200);
    CHECK(std::filesystem::file_size(paths.csv) > 200);
    CHECK(std::filesystem::file_size(paths.html) > 200);
    CHECK(std::filesystem::file_size(paths.markdown) > 200);

    // Writing twice into the same directory must not clobber the first report.
    const auto second = write_reports(result, temp);
    CHECK(second.html != paths.html);

    // Selective formats.
    const auto only_csv = write_reports(result, temp, ReportFormat::csv);
    CHECK(!only_csv.csv.empty());
    CHECK(only_csv.html.empty());
    CHECK(only_csv.json.empty());

    // The HTML must be self-contained: no external fetches of any kind.
    std::string html_text;
    {
        std::ifstream html(paths.html, std::ios::binary);
        html_text.assign((std::istreambuf_iterator<char>(html)), std::istreambuf_iterator<char>());
    }
    CHECK(html_text.find("<!doctype html>") == 0);
    CHECK(html_text.find("</html>") != std::string::npos);
    CHECK(html_text.find("http://") == std::string::npos);
    CHECK(html_text.find("https://") == std::string::npos);
    CHECK(html_text.find("<script") == std::string::npos);
    CHECK(html_text.find("<svg") != std::string::npos);

    // CSV round trip through the comparison reader.
    const auto run = read_csv_run(paths.csv);
    CHECK(run.label == "unit test");
    CHECK(!run.cpu.empty());
    CHECK(run.probes.count("dependent_latency") == 1);
    CHECK(run.probes.count("sequential_read") == 1);
    CHECK(run.probes.at("dependent_latency").points.size() == latency->points.size());
    CHECK(run.probes.at("sequential_read").y_axis == "gib_per_second");
    for (const auto& p : run.probes.at("dependent_latency").points) {
        CHECK(p.x > 0);
        CHECK(p.y > 0.0);
    }

    const auto comparison = temp / "comparison.html";
    write_comparison({run, run}, comparison);
    CHECK(std::filesystem::file_size(comparison) > 500);

    // Directory expansion should find the CSVs we just wrote.
    const auto expanded = expand_csv_inputs({temp});
    CHECK(expanded.size() >= 3);
    CHECK(std::all_of(expanded.begin(), expanded.end(),
                      [](const std::filesystem::path& p) { return p.extension() == ".csv"; }));

    std::filesystem::remove_all(temp, cleanup_ec);
}

void test_cancellation() {
    // Cancelling after the first callback must return partial, still-valid data
    // instead of throwing or hanging.
    auto cfg = smoke_config();
    cfg.maximum_working_set_bytes = 1024 * 1024;
    const auto result = run_benchmark(cfg, [](const Progress&) { return false; });
    const auto* latency = result.find("dependent_latency");
    CHECK(latency != nullptr);
    if (latency) CHECK(latency->points.size() <= 2);
    CHECK(std::any_of(result.warnings.begin(), result.warnings.end(),
                      [](const std::string& w) { return w.find("cancelled") != std::string::npos; }));
}

void test_bad_csv_is_rejected() {
    const auto temp = std::filesystem::temp_directory_path() / "cachescope_bad_csv";
    std::filesystem::create_directories(temp);
    const auto path = temp / "not_cachescope.csv";
    {
        std::ofstream out(path);
        out << "a,b,c\n1,2,3\n";
    }
    bool threw = false;
    try {
        (void)read_csv_run(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    bool missing_threw = false;
    try {
        (void)read_csv_run(temp / "does_not_exist.csv");
    } catch (const std::runtime_error&) {
        missing_threw = true;
    }
    CHECK(missing_threw);
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

void test_system_detection() {
    const auto system = detect_system();
    CHECK(!system.cpu_name.empty());
    CHECK(!system.operating_system.empty());
    CHECK(!system.architecture.empty());
    CHECK(system.pointer_bits == 32 || system.pointer_bits == 64);
    CHECK(system.logical_cpus >= 1);
    CHECK(system.page_size_bytes >= 512);
    CHECK(system.steady_clock_resolution_ns >= 0.0);
    for (const auto& c : system.caches) {
        CHECK(c.level >= 1 && c.level <= 4);
        CHECK(c.size_bytes > 0);
        CHECK(c.source != GeometrySource::none);
    }
}

} // namespace

// Announce each test before running it, so that a crash (rather than a failed
// check) still tells you which experiment broke on an unfamiliar machine.
void run(const char* name, void (*fn)()) {
    std::cout << "  " << name << std::flush;
    const int before = failures;
    try {
        fn();
    } catch (const std::exception& e) {
        std::cout << "  THREW\n" << std::flush;
        std::cerr << "unexpected exception in " << name << ": " << e.what() << '\n';
        ++failures;
        return;
    }
    std::cout << (failures == before ? "  ok\n" : "  FAILED\n") << std::flush;
}

int main() {
    std::cout << "CacheScope tests\n";
    run("statistics", test_statistics);
    run("random_cycle", test_cycle);
    run("working_set_sizes", test_size_generation);
    run("presets", test_presets);
    run("knee_detection", test_knee_detection);
    run("axis_names", test_axis_names);
    run("system_detection", test_system_detection);
    run("benchmark_and_reports", test_benchmark_and_reports);
    run("cancellation", test_cancellation);
    run("bad_csv_rejected", test_bad_csv_is_rejected);

    if (failures == 0) {
        std::cout << "All CacheScope tests passed (" << checks << " checks).\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " checks failed.\n";
    return 1;
}
