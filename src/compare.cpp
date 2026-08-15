#include <cachescope/compare.hpp>
#include <cachescope/benchmark.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cachescope {
namespace {

std::vector<std::string> parse_csv_line(std::string_view line) {
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                current += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                current += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fields.push_back(std::move(current));
            current.clear();
        } else if (c != '\r') {
            current += c;
        }
    }
    fields.push_back(std::move(current));
    return fields;
}

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string fixed(double v, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << v;
    return out.str();
}

std::uint64_t to_u64(const std::string& text) {
    try {
        return std::stoull(text);
    } catch (...) {
        return 0;
    }
}

double to_double(const std::string& text) {
    try {
        return std::stod(text);
    } catch (...) {
        return 0.0;
    }
}

constexpr const char* kPalette[] = {"#2563eb", "#dc2626", "#059669", "#7c3aed",
                                    "#d97706", "#0891b2", "#be185d", "#4b5563"};

std::string axis_title(const std::string& x_axis) {
    if (x_axis == "stride") return "Offset / stride in bytes (log2)";
    if (x_axis == "blocks") return "Blocks mapping to the same cache set";
    if (x_axis == "pages") return "Distinct pages touched (log2)";
    return "Working set (log2 bytes)";
}

std::string x_tick_label(const std::string& x_axis, std::uint64_t value) {
    if (x_axis == "blocks" || x_axis == "pages") return std::to_string(value);
    if (x_axis == "stride") return std::to_string(value) + " B";
    return format_bytes(value);
}

// Representative point on a curve: the sample whose x is closest to `target`.
const ComparisonPoint* closest(const ComparisonSeries* series, std::uint64_t target) {
    if (!series || series->points.empty()) return nullptr;
    return &*std::min_element(series->points.begin(), series->points.end(),
                              [&](const ComparisonPoint& a, const ComparisonPoint& b) {
                                  const auto da = a.x > target ? a.x - target : target - a.x;
                                  const auto db = b.x > target ? b.x - target : target - b.x;
                                  return da < db;
                              });
}

const ComparisonSeries* find_probe(const ComparisonRun& run, const std::string& id) {
    const auto it = run.probes.find(id);
    return it == run.probes.end() ? nullptr : &it->second;
}

std::string chart(const std::vector<ComparisonRun>& runs, const std::string& probe) {
    struct Series {
        std::string label;
        const ComparisonSeries* data;
    };
    std::vector<Series> series;
    std::string title;
    std::string x_axis = "working_set";
    std::string y_axis = "ns_per_access";
    for (const auto& run : runs) {
        const auto* found = find_probe(run, probe);
        if (!found || found->points.empty()) continue;
        series.push_back({run.label, found});
        if (title.empty()) {
            title = found->title;
            x_axis = found->x_axis;
            y_axis = found->y_axis;
        }
    }
    if (series.empty()) return {};

    double xmin = std::numeric_limits<double>::max();
    double xmax = -std::numeric_limits<double>::max();
    double ymax = 0.0;
    for (const auto& s : series) {
        for (const auto& p : s.data->points) {
            if (p.x == 0) continue;
            const double x = std::log2(static_cast<double>(p.x));
            xmin = std::min(xmin, x);
            xmax = std::max(xmax, x);
            ymax = std::max(ymax, p.y);
        }
    }
    if (!(xmax > xmin) || !(ymax > 0.0)) return {};
    ymax *= 1.05;

    constexpr double W = 940, H = 340, L = 72, R = 22, T = 20, B = 54;
    const double pw = W - L - R, ph = H - T - B;
    auto sx = [&](double x) { return L + (x - xmin) / (xmax - xmin) * pw; };
    auto sy = [&](double y) { return T + ph - y / ymax * ph; };

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "<section class='card'><h2>" << html_escape(title.empty() ? probe : title) << "</h2><svg viewBox='0 0 "
        << W << ' ' << H << "'>";
    for (int i = 0; i <= 4; ++i) {
        const double y = ymax * i / 4.0;
        const double py = sy(y);
        out << "<line class='grid' x1='" << L << "' y1='" << py << "' x2='" << (L + pw) << "' y2='" << py
            << "'/><text class='tick' x='" << (L - 8) << "' y='" << (py + 4) << "' text-anchor='end'>"
            << std::setprecision(ymax < 10 ? 2 : 1) << y << std::setprecision(2) << "</text>";
    }
    out << "<line class='axis' x1='" << L << "' y1='" << (T + ph) << "' x2='" << (L + pw) << "' y2='"
        << (T + ph) << "'/><line class='axis' x1='" << L << "' y1='" << T << "' x2='" << L << "' y2='"
        << (T + ph) << "'/>";

    for (std::size_t si = 0; si < series.size(); ++si) {
        const char* color = kPalette[si % std::size(kPalette)];
        out << "<polyline fill='none' stroke='" << color << "' stroke-width='2.2' points='";
        for (const auto& p : series[si].data->points) {
            if (p.x == 0) continue;
            out << sx(std::log2(static_cast<double>(p.x))) << ',' << sy(p.y) << ' ';
        }
        out << "'/>";
        for (const auto& p : series[si].data->points) {
            if (p.x == 0) continue;
            out << "<circle cx='" << sx(std::log2(static_cast<double>(p.x))) << "' cy='" << sy(p.y)
                << "' r='2.5' fill='" << color << "'><title>" << html_escape(series[si].label) << " - "
                << html_escape(x_tick_label(x_axis, p.x)) << ": " << fixed(p.y, 2) << "</title></circle>";
        }
    }

    // Ticks come from the union of x positions of the first series.
    const auto& reference = series.front().data->points;
    const std::size_t tick_count = std::min<std::size_t>(7, reference.size());
    for (std::size_t i = 0; i < tick_count && reference.size() > 1; ++i) {
        const std::size_t idx = i * (reference.size() - 1) / std::max<std::size_t>(1, tick_count - 1);
        if (reference[idx].x == 0) continue;
        out << "<text class='tick' x='" << sx(std::log2(static_cast<double>(reference[idx].x))) << "' y='"
            << (T + ph + 22) << "' text-anchor='middle'>"
            << html_escape(x_tick_label(x_axis, reference[idx].x)) << "</text>";
    }

    out << "<text class='axislabel' x='" << (L + pw / 2) << "' y='" << (H - 8) << "' text-anchor='middle'>"
        << html_escape(axis_title(x_axis)) << "</text><text class='axislabel' transform='translate(14,"
        << (T + ph / 2) << ") rotate(-90)' text-anchor='middle'>"
        << (y_axis == "gib_per_second" ? "GiB / s" : "ns / access") << "</text></svg>";

    out << "<div class='legend'>";
    for (std::size_t si = 0; si < series.size(); ++si) {
        out << "<span><i style='background:" << kPalette[si % std::size(kPalette)] << "'></i>"
            << html_escape(series[si].label) << "</span>";
    }
    out << "</div></section>";
    return out.str();
}

void write_level_table(std::ostream& out, const std::vector<ComparisonRun>& runs) {
    out << "<section class='card'><h2>Cache-level comparison</h2>"
           "<p class='note'>Each cell is taken from the curve at a working set near half of that machine's "
           "own reported cache size, so machines with different cache sizes are still compared at a "
           "comparable point in their own hierarchy. Cells are blank when the machine did not report that "
           "level.</p>"
           "<table><tr><th>Run</th><th>Level</th><th>Reported size</th><th>Latency</th><th>Cycles</th>"
           "<th>Read</th><th>Write</th></tr>";
    for (const auto& run : runs) {
        const auto* latency = find_probe(run, "dependent_latency");
        const auto* read = find_probe(run, "sequential_read");
        const auto* write = find_probe(run, "sequential_write");
        const std::uint64_t sizes[3] = {run.l1d_bytes, run.l2_bytes, run.l3_bytes};
        bool any = false;
        for (int level = 0; level < 3; ++level) {
            if (sizes[level] == 0) continue;
            any = true;
            const std::uint64_t target = sizes[level] / 2;
            const auto* lp = closest(latency, target);
            const auto* rp = closest(read, target);
            const auto* wp = closest(write, target);
            out << "<tr><td>" << html_escape(run.label) << "</td><td>L" << (level + 1) << "</td><td>"
                << format_bytes(sizes[level]) << "</td><td class='metric'>"
                << (lp ? fixed(lp->y, 2) + " ns" : "-") << "</td><td class='metric'>"
                << (lp && run.cpu_mhz > 0 ? fixed(lp->y * run.cpu_mhz / 1000.0, 1) : "-")
                << "</td><td class='metric'>" << (rp ? fixed(rp->y, 2) + " GiB/s" : "-")
                << "</td><td class='metric'>" << (wp ? fixed(wp->y, 2) + " GiB/s" : "-") << "</td></tr>";
        }
        if (!any) {
            out << "<tr><td>" << html_escape(run.label)
                << "</td><td colspan='6'>no reported cache geometry in this CSV</td></tr>";
        }
        // Deepest measured point: a practical stand-in for main memory.
        if (latency && !latency->points.empty()) {
            const auto& deepest = latency->points.back();
            const auto* rp = closest(read, deepest.x);
            out << "<tr><td>" << html_escape(run.label) << "</td><td>DRAM</td><td>"
                << format_bytes(deepest.x) << "</td><td class='metric'>" << fixed(deepest.y, 2)
                << " ns</td><td class='metric'>"
                << (run.cpu_mhz > 0 ? fixed(deepest.y * run.cpu_mhz / 1000.0, 1) : "-")
                << "</td><td class='metric'>" << (rp ? fixed(rp->y, 2) + " GiB/s" : "-")
                << "</td><td class='metric'>-</td></tr>";
        }
    }
    out << "</table></section>";
}

const char* kStyle = R"CSS(
:root{color-scheme:light dark;--bg:#f5f7fb;--fg:#172033;--card:#fff;--line:#e5e7eb;--muted:#64748b;--head:#111827;--headfg:#fff;--thead:#f8fafc}
@media(prefers-color-scheme:dark){:root{--bg:#0b1020;--fg:#e5e9f2;--card:#151b2d;--line:#2a3350;--muted:#9aa6c0;--head:#0a0f1e;--headfg:#f3f6ff;--thead:#1b2338}}
*{box-sizing:border-box}
body{font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;background:var(--bg);color:var(--fg);margin:0}
.wrap{max-width:1120px;margin:auto;padding:28px}
.hero{background:var(--head);color:var(--headfg);border-radius:16px;padding:24px}
.hero h1{margin:0 0 6px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:20px 22px;margin-top:16px}
.card svg{width:100%;height:auto}
.grid{stroke:var(--line)}.axis{stroke:var(--muted)}
.tick{font-size:10px;fill:var(--muted)}.axislabel{font-size:11px;fill:var(--muted)}
table{border-collapse:collapse;width:100%;font-size:13px}
th,td{padding:8px 9px;border-bottom:1px solid var(--line);text-align:left}
th{background:var(--thead)}
.metric{font-variant-numeric:tabular-nums}
.legend{display:flex;flex-wrap:wrap;gap:14px;font-size:12px;color:var(--muted);margin-top:10px}
.legend span{display:flex;align-items:center;gap:6px}
.legend i{display:inline-block;width:18px;height:4px;border-radius:2px}
.note{color:var(--muted);line-height:1.55;font-size:13px}
@media(max-width:640px){.wrap{padding:14px}table{font-size:12px}}
)CSS";

} // namespace

ComparisonRun read_csv_run(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("could not open " + path.string());
    std::string header_line;
    if (!std::getline(in, header_line)) throw std::runtime_error("empty CSV: " + path.string());
    const auto header = parse_csv_line(header_line);
    std::map<std::string, std::size_t> col;
    for (std::size_t i = 0; i < header.size(); ++i) col[header[i]] = i;

    for (const char* name : {"cpu", "os", "architecture", "preset", "probe", "x_value", "x_axis", "y_axis",
                             "median_ns_per_access", "median_gib_s"}) {
        if (col.find(name) == col.end()) {
            throw std::runtime_error("not a CacheScope CSV (missing column '" + std::string(name) +
                                     "'): " + path.string());
        }
    }

    auto field = [&](const std::vector<std::string>& f, const char* name) -> std::string {
        const auto it = col.find(name);
        if (it == col.end() || it->second >= f.size()) return {};
        return f[it->second];
    };

    ComparisonRun run;
    run.source_file = path.filename().string();
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        const auto f = parse_csv_line(line);
        if (f.size() < header.size()) continue;
        if (first) {
            run.cpu = field(f, "cpu");
            run.os = field(f, "os");
            run.architecture = field(f, "architecture");
            run.pointer_bits = field(f, "pointer_bits");
            run.compiler = field(f, "compiler");
            run.preset = field(f, "preset");
            run.l1d_bytes = to_u64(field(f, "l1d_bytes"));
            run.l2_bytes = to_u64(field(f, "l2_bytes"));
            run.l3_bytes = to_u64(field(f, "l3_bytes"));
            run.line_bytes = to_u64(field(f, "line_bytes"));
            run.cpu_mhz = to_double(field(f, "cpu_mhz"));
            run.label = field(f, "label");
            if (run.label.empty()) run.label = run.cpu;
            if (run.label.empty()) run.label = path.stem().string();
            first = false;
        }
        const std::string probe = field(f, "probe");
        if (probe.empty()) continue;
        auto& series = run.probes[probe];
        if (series.title.empty()) {
            series.title = field(f, "probe_title");
            if (series.title.empty()) series.title = probe;
            series.x_axis = field(f, "x_axis");
            series.y_axis = field(f, "y_axis");
        }
        const auto x = to_u64(field(f, "x_value"));
        const double y = to_double(
            field(f, series.y_axis == "gib_per_second" ? "median_gib_s" : "median_ns_per_access"));
        if (x > 0 && std::isfinite(y) && y > 0.0) series.points.push_back({x, y});
    }
    if (first) throw std::runtime_error("CSV contains no data rows: " + path.string());

    // Multiple runs can be concatenated into one file; keep the curves ordered.
    for (auto& [id, series] : run.probes) {
        (void)id;
        std::stable_sort(series.points.begin(), series.points.end(),
                         [](const ComparisonPoint& a, const ComparisonPoint& b) { return a.x < b.x; });
    }
    return run;
}

std::vector<std::filesystem::path> expand_csv_inputs(const std::vector<std::filesystem::path>& inputs) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& input : inputs) {
        if (std::filesystem::is_directory(input, ec)) {
            std::vector<std::filesystem::path> found;
            for (const auto& entry : std::filesystem::directory_iterator(input, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                auto extension = entry.path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (extension == ".csv") found.push_back(entry.path());
            }
            std::sort(found.begin(), found.end());
            files.insert(files.end(), found.begin(), found.end());
        } else {
            files.push_back(input);
        }
    }
    return files;
}

void write_comparison(const std::vector<ComparisonRun>& runs, const std::filesystem::path& output) {
    if (runs.empty()) throw std::runtime_error("no runs to compare");
    if (output.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(output.parent_path(), ec);
    }
    std::ofstream out(output, std::ios::binary);
    if (!out) throw std::runtime_error("could not create " + output.string());

    out << "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>CacheScope comparison</title><style>"
        << kStyle << "</style></head><body><div class='wrap'>";
    out << "<div class='hero'><h1>CacheScope comparison</h1><p>" << runs.size()
        << " run(s), median curves overlaid.</p></div>";

    out << "<section class='card'><h2>Runs</h2><table><tr><th>Label</th><th>CPU</th><th>OS</th>"
           "<th>Arch</th><th>Compiler</th><th>Preset</th><th>File</th></tr>";
    for (const auto& r : runs) {
        out << "<tr><td>" << html_escape(r.label) << "</td><td>" << html_escape(r.cpu) << "</td><td>"
            << html_escape(r.os) << "</td><td>"
            << html_escape(r.architecture + (r.pointer_bits.empty() ? "" : " / " + r.pointer_bits + "-bit"))
            << "</td><td>" << html_escape(r.compiler) << "</td><td>" << html_escape(r.preset) << "</td><td>"
            << html_escape(r.source_file) << "</td></tr>";
    }
    out << "</table><p class='note'>For defensible comparisons use the same CacheScope version and preset, "
           "run on idle physical machines under comparable power and thermal conditions, and prefer three "
           "runs per machine. Numbers from CI runners or virtual machines are validation data, not hardware "
           "rankings.</p></section>";

    write_level_table(out, runs);

    // Emit the well-known probes in a readable order, then anything else the
    // CSVs contained (for example per-stride conflict-miss series).
    std::vector<std::string> ordered{"dependent_latency", "sequential_read", "sequential_write",
                                     "copy",             "line_pair",       "stride",
                                     "tlb"};
    std::set<std::string> seen(ordered.begin(), ordered.end());
    std::set<std::string> extras;
    for (const auto& run : runs) {
        for (const auto& [id, series] : run.probes) {
            (void)series;
            if (seen.find(id) == seen.end()) extras.insert(id);
        }
    }
    ordered.insert(ordered.end(), extras.begin(), extras.end());

    for (const auto& probe : ordered) out << chart(runs, probe);

    out << "</div></body></html>";
    out.flush();
    if (!out) throw std::runtime_error("could not write " + output.string());
}

} // namespace cachescope
