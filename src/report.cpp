// Report generation: self-contained HTML (with inline SVG charts), CSV for
// cross-machine comparison, JSON for programmatic re-analysis, and Markdown for
// pasting into a lab write-up.

#include <cachescope/benchmark.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace cachescope {
namespace {

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                    out += hex.str();
                } else {
                    out += c;
                }
        }
    }
    return out;
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
            default: out += c;
        }
    }
    return out;
}

std::string csv_escape(std::string_view text) {
    if (text.find_first_of(",\"\n\r") == std::string_view::npos) return std::string(text);
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

std::string markdown_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '|') out += "\\|";
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}

std::string fixed(double value, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string maybe(double value, int precision = 2, std::string_view suffix = {}) {
    if (!std::isfinite(value) || value <= 0.0) return "-";
    return fixed(value, precision) + std::string(suffix);
}

std::string ways_text(std::uint32_t ways) {
    if (ways == 0) return "-";
    if (ways == 0xFFFFFFFFu) return "full";
    return std::to_string(ways) + "-way";
}

std::string x_label(const SweepSeries& series, const SweepPoint& p) {
    switch (series.x) {
        case XAxis::stride: return std::to_string(p.stride_bytes) + " B";
        case XAxis::blocks: return std::to_string(p.element_count);
        case XAxis::pages: return std::to_string(p.element_count);
        case XAxis::working_set:
        default: return format_bytes(p.working_set_bytes);
    }
}

std::string x_axis_title(const SweepSeries& series) {
    switch (series.x) {
        case XAxis::stride: return "Offset / stride in bytes (log2)";
        case XAxis::blocks: return "Blocks mapping to the same cache set";
        case XAxis::pages: return "Distinct pages touched (log2)";
        case XAxis::working_set:
        default: return "Working set (log2)";
    }
}

std::string y_axis_title(const SweepSeries& series) {
    if (series.y == YAxis::gib_per_second) return "GiB / s";
    if (series.id == "line_pair") return "ns / dependent pair";
    return "ns / access";
}

double point_y(const SweepSeries& series, const SweepPoint& p) {
    return series.y == YAxis::gib_per_second ? p.gib_per_second.median : p.ns_per_access.median;
}

void write_stats_json(std::ostream& out, const Statistics& s) {
    out << "{\"min\":" << s.minimum << ",\"p05\":" << s.p05 << ",\"median\":" << s.median
        << ",\"mean\":" << s.mean << ",\"p95\":" << s.p95 << ",\"max\":" << s.maximum
        << ",\"mad\":" << s.mad << ",\"cv\":" << s.coefficient_of_variation
        << ",\"samples\":" << s.sample_count << '}';
}

std::string compact_timestamp(std::string timestamp) {
    for (char& c : timestamp) {
        if (c == ':' || c == '-' || c == 'T') c = '_';
    }
    if (!timestamp.empty() && timestamp.back() == 'Z') timestamp.pop_back();
    return timestamp;
}

std::string sanitize_stem(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) out += static_cast<char>(std::tolower(uc));
        else if (!out.empty() && out.back() != '_') out += '_';
        if (out.size() >= 40) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// SVG chart
// ---------------------------------------------------------------------------

struct Marker {
    double x = 0.0;      // already log2-transformed
    std::string label;
};

std::string svg_chart(const SweepSeries& series, const std::vector<Marker>& markers) {
    if (series.points.size() < 2) return {};
    constexpr double width = 880.0;
    constexpr double height = 320.0;
    constexpr double left = 76.0;
    constexpr double right = 24.0;
    constexpr double top = 20.0;
    constexpr double bottom = 56.0;
    const double plot_w = width - left - right;
    const double plot_h = height - top - bottom;

    std::vector<double> xs;
    std::vector<double> ys;
    std::vector<const SweepPoint*> kept;
    for (const auto& p : series.points) {
        const auto raw_x = series.x_value(p);
        if (raw_x == 0) continue;
        xs.push_back(std::log2(static_cast<double>(raw_x)));
        ys.push_back(point_y(series, p));
        kept.push_back(&p);
    }
    if (xs.size() < 2) return {};

    const auto [xmin_it, xmax_it] = std::minmax_element(xs.begin(), xs.end());
    const auto [ymin_it, ymax_it] = std::minmax_element(ys.begin(), ys.end());
    const double xmin = *xmin_it;
    const double xmax = *xmax_it;
    const double ymin = std::min(0.0, *ymin_it);
    const double ymax = *ymax_it > ymin ? *ymax_it * 1.05 : ymin + 1.0;

    auto sx = [&](double x) { return left + (x - xmin) / std::max(1e-12, xmax - xmin) * plot_w; };
    auto sy = [&](double y) { return top + plot_h - (y - ymin) / std::max(1e-12, ymax - ymin) * plot_h; };

    std::ostringstream svg;
    svg << std::fixed << std::setprecision(2);
    svg << "<div class='chart'><h3>" << html_escape(series.title) << "</h3>"
        << "<svg viewBox='0 0 " << width << ' ' << height << "' role='img' aria-label='"
        << html_escape(series.title) << "'>";

    for (int i = 0; i <= 4; ++i) {
        const double y = ymin + (ymax - ymin) * i / 4.0;
        const double py = sy(y);
        svg << "<line class='grid' x1='" << left << "' y1='" << py << "' x2='" << (left + plot_w)
            << "' y2='" << py << "'/>"
            << "<text class='tick' x='" << (left - 8) << "' y='" << (py + 4) << "' text-anchor='end'>"
            << std::setprecision(ymax < 10 ? 2 : 1) << y << std::setprecision(2) << "</text>";
    }

    // Hardware-reported boundaries, drawn as reference lines so the reader can
    // see where a measured knee sits relative to reported geometry.
    for (const auto& marker : markers) {
        if (marker.x < xmin || marker.x > xmax) continue;
        const double px = sx(marker.x);
        svg << "<line class='marker' x1='" << px << "' y1='" << top << "' x2='" << px << "' y2='"
            << (top + plot_h) << "'/>"
            << "<text class='markerlabel' x='" << (px + 4) << "' y='" << (top + 12) << "'>"
            << html_escape(marker.label) << "</text>";
    }

    svg << "<line class='axis' x1='" << left << "' y1='" << (top + plot_h) << "' x2='" << (left + plot_w)
        << "' y2='" << (top + plot_h) << "'/><line class='axis' x1='" << left << "' y1='" << top
        << "' x2='" << left << "' y2='" << (top + plot_h) << "'/>";

    svg << "<polyline class='series' fill='none' points='";
    for (std::size_t i = 0; i < xs.size(); ++i) svg << sx(xs[i]) << ',' << sy(ys[i]) << ' ';
    svg << "'/>";
    for (std::size_t i = 0; i < xs.size(); ++i) {
        svg << "<circle class='point' cx='" << sx(xs[i]) << "' cy='" << sy(ys[i]) << "' r='3'><title>"
            << html_escape(x_label(series, *kept[i])) << ": " << fixed(ys[i], 2) << ' '
            << html_escape(y_axis_title(series)) << "</title></circle>";
    }

    const std::size_t tick_count = std::min<std::size_t>(7, kept.size());
    for (std::size_t i = 0; i < tick_count; ++i) {
        const std::size_t idx = i * (kept.size() - 1) / std::max<std::size_t>(1, tick_count - 1);
        const double px = sx(xs[idx]);
        svg << "<text class='tick' x='" << px << "' y='" << (top + plot_h + 22) << "' text-anchor='middle'>"
            << html_escape(x_label(series, *kept[idx])) << "</text>";
    }
    svg << "<text class='axislabel' x='" << (left + plot_w / 2) << "' y='" << (height - 10)
        << "' text-anchor='middle'>" << html_escape(x_axis_title(series)) << "</text>";
    svg << "<text class='axislabel' transform='translate(16," << (top + plot_h / 2)
        << ") rotate(-90)' text-anchor='middle'>" << html_escape(y_axis_title(series)) << "</text>";
    svg << "</svg></div>";
    return svg.str();
}

std::vector<Marker> cache_markers(const BenchmarkResult& result, const SweepSeries& series) {
    std::vector<Marker> markers;
    if (series.x != XAxis::working_set) return markers;
    for (int level = 1; level <= 3; ++level) {
        std::uint64_t size = 0;
        for (const auto& c : result.system.caches) {
            if (c.level == level && (c.type == CacheType::data || c.type == CacheType::unified)) {
                size = std::max(size, c.size_bytes);
            }
        }
        if (size == 0) continue;
        markers.push_back({std::log2(static_cast<double>(size)), "L" + std::to_string(level)});
    }
    return markers;
}

const char* kStyle = R"CSS(
:root{color-scheme:light dark;--bg:#f5f7fb;--fg:#172033;--card:#ffffff;--line:#e5e7eb;--muted:#64748b;--accent:#2563eb;--warnbg:#fff7ed;--warnfg:#9a3412;--warnline:#fed7aa;--head:#111827;--headfg:#ffffff;--thead:#f8fafc}
@media (prefers-color-scheme:dark){:root{--bg:#0b1020;--fg:#e5e9f2;--card:#151b2d;--line:#2a3350;--muted:#9aa6c0;--accent:#7aa2ff;--warnbg:#2c1c0d;--warnfg:#fbbf77;--warnline:#5a3a16;--head:#0a0f1e;--headfg:#f3f6ff;--thead:#1b2338}}
*{box-sizing:border-box}
body{margin:0;font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;background:var(--bg);color:var(--fg)}
.wrap{max-width:1120px;margin:auto;padding:28px}
.hero{background:var(--head);color:var(--headfg);border-radius:18px;padding:26px 28px}
.hero h1{margin:0 0 8px;font-size:30px}.hero p{margin:6px 0;opacity:.82}
.grid2{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px}
.card,.chart{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:18px;margin-top:16px}
.chart svg{width:100%;height:auto}
.axis{stroke:var(--muted);stroke-width:1}.grid{stroke:var(--line);stroke-width:1}
.series{stroke:var(--accent);stroke-width:2.5}.point{fill:var(--accent)}
.marker{stroke:var(--muted);stroke-width:1;stroke-dasharray:4 4}
.markerlabel{font-size:10px;fill:var(--muted)}
.tick{font-size:10px;fill:var(--muted)}.axislabel{font-size:11px;fill:var(--muted)}
table{border-collapse:collapse;width:100%;font-size:14px}
th,td{text-align:left;padding:9px 10px;border-bottom:1px solid var(--line)}
th{background:var(--thead);font-weight:650}
.metric{font-variant-numeric:tabular-nums}
.warn{background:var(--warnbg);border:1px solid var(--warnline);border-radius:10px;padding:10px 12px;margin:8px 0;color:var(--warnfg)}
.note{line-height:1.6}.small{font-size:12px;color:var(--muted)}
h2{margin:0 0 10px;font-size:19px}h3{margin:0 0 10px;font-size:15px}
code{background:var(--thead);padding:1px 5px;border-radius:4px;font-size:12px}
.tag{display:inline-block;font-size:11px;padding:2px 8px;border-radius:999px;background:var(--thead);color:var(--muted);margin-left:6px}
@media(max-width:640px){.wrap{padding:14px}.hero h1{font-size:23px}table{font-size:12.5px}}
)CSS";

void write_json(std::ostream& out, const BenchmarkResult& result) {
    out << std::setprecision(12);
    out << "{\n  \"schema\":\"" << json_escape(result.schema) << "\",\n"
        << "  \"version\":\"" << json_escape(result.version) << "\",\n"
        << "  \"timestamp_utc\":\"" << json_escape(result.timestamp_utc) << "\",\n"
        << "  \"label\":\"" << json_escape(result.label) << "\",\n"
        << "  \"duration_seconds\":" << result.duration_seconds << ",\n"
        << "  \"system\":{"
        << "\"cpu_name\":\"" << json_escape(result.system.cpu_name) << "\","
        << "\"architecture\":\"" << json_escape(result.system.architecture) << "\","
        << "\"operating_system\":\"" << json_escape(result.system.operating_system) << "\","
        << "\"compiler\":\"" << json_escape(result.system.compiler) << "\","
        << "\"build_flavor\":\"" << json_escape(result.system.build_flavor) << "\","
        << "\"pointer_bits\":" << result.system.pointer_bits << ','
        << "\"logical_cpus\":" << result.system.logical_cpus << ','
        << "\"page_size_bytes\":" << result.system.page_size_bytes << ','
        << "\"total_ram_bytes\":" << result.system.total_ram_bytes << ','
        << "\"steady_clock_resolution_ns\":" << result.system.steady_clock_resolution_ns << ','
        << "\"affinity_supported\":" << (result.system.affinity_supported ? "true" : "false") << ','
        << "\"affinity_applied\":" << (result.system.affinity_applied ? "true" : "false") << ','
        << "\"pinned_cpu\":" << result.system.pinned_cpu << ','
        << "\"cpu_mhz_start\":" << result.system.cpu_mhz_start << ','
        << "\"cpu_mhz_peak\":" << result.system.cpu_mhz_peak << ','
        << "\"cpu_mhz_end\":" << result.system.cpu_mhz_end << ','
        << "\"cpu_mhz_max\":" << result.system.cpu_mhz_max << ',' << "\"caches\":[";
    for (std::size_t i = 0; i < result.system.caches.size(); ++i) {
        if (i) out << ',';
        const auto& c = result.system.caches[i];
        out << "{\"level\":" << c.level << ",\"type\":\"" << cache_type_name(c.type) << "\""
            << ",\"size_bytes\":" << c.size_bytes << ",\"line_size_bytes\":" << c.line_size_bytes
            << ",\"sharing_threads\":" << c.sharing_threads << ",\"instances\":" << c.instances
            << ",\"associativity_ways\":" << c.associativity_ways << ",\"source\":\""
            << geometry_source_name(c.source) << "\"}";
    }
    out << "],\"notes\":[";
    for (std::size_t i = 0; i < result.system.notes.size(); ++i) {
        if (i) out << ',';
        out << '"' << json_escape(result.system.notes[i]) << '"';
    }
    out << "]},\n";

    out << "  \"config\":{\"preset\":\"" << json_escape(result.config.preset) << "\","
        << "\"minimum_working_set_bytes\":" << result.config.minimum_working_set_bytes << ','
        << "\"maximum_working_set_bytes\":" << result.config.maximum_working_set_bytes << ','
        << "\"samples\":" << result.config.samples << ','
        << "\"warmup_samples\":" << result.config.warmup_samples << ','
        << "\"target_sample_ms\":" << result.config.target_sample_ms << ','
        << "\"pin_thread\":" << (result.config.pin_thread ? "true" : "false") << ','
        << "\"preferred_cpu\":" << result.config.preferred_cpu << "},\n";

    out << "  \"series\":[";
    for (std::size_t s = 0; s < result.series.size(); ++s) {
        if (s) out << ',';
        const auto& series = result.series[s];
        out << "\n    {\"id\":\"" << json_escape(series.id) << "\",\"title\":\""
            << json_escape(series.title) << "\",\"x_axis\":\"" << x_axis_name(series.x)
            << "\",\"y_axis\":\"" << y_axis_name(series.y) << "\",\"points\":[";
        for (std::size_t i = 0; i < series.points.size(); ++i) {
            if (i) out << ',';
            const auto& p = series.points[i];
            out << "{\"working_set_bytes\":" << p.working_set_bytes
                << ",\"stride_bytes\":" << p.stride_bytes << ",\"element_count\":" << p.element_count
                << ",\"operations_per_sample\":" << p.operations_per_sample << ",\"ns_per_access\":";
            write_stats_json(out, p.ns_per_access);
            out << ",\"gib_per_second\":";
            write_stats_json(out, p.gib_per_second);
            out << '}';
        }
        out << "]}";
    }
    out << "\n  ],\n";

    out << "  \"coherency\":{\"measured\":" << (result.coherency.measured ? "true" : "false")
        << ",\"pairs\":[";
    for (std::size_t i = 0; i < result.coherency.pairs.size(); ++i) {
        if (i) out << ',';
        const auto& p = result.coherency.pairs[i];
        out << "{\"label\":\"" << json_escape(p.label) << "\",\"cpu_a\":" << p.cpu_a
            << ",\"cpu_b\":" << p.cpu_b << ",\"pinned\":" << (p.pinned ? "true" : "false")
            << ",\"shared_line_ns\":";
        write_stats_json(out, p.shared_line_ns);
        out << ",\"padded_line_ns\":";
        write_stats_json(out, p.padded_line_ns);
        out << ",\"single_thread_ns\":";
        write_stats_json(out, p.single_thread_ns);
        out << ",\"ping_pong_ns\":";
        write_stats_json(out, p.ping_pong_ns);
        out << '}';
    }
    out << "]},\n";

    out << "  \"experimental\":{\"line_size_bytes\":" << result.experimental.line_size_bytes
        << ",\"spatial_saturation_bytes\":" << result.experimental.spatial_saturation_bytes
        << ",\"tlb_reach_pages\":" << result.experimental.tlb_reach_pages << ",\"associativity_ways\":[";
    for (std::size_t i = 0; i < result.experimental.associativity_ways.size(); ++i) {
        if (i) out << ',';
        out << "{\"stride_bytes\":" << result.experimental.associativity_ways[i].first
            << ",\"observed_ways\":" << result.experimental.associativity_ways[i].second << '}';
    }
    out << "],\"fastest_latency_ns\":" << result.experimental.fastest_latency_ns
        << ",\"latency_escalation\":[";
    for (std::size_t i = 0; i < result.experimental.latency_escalation.size(); ++i) {
        if (i) out << ',';
        out << "{\"multiple\":" << result.experimental.latency_escalation[i].first
            << ",\"working_set_bytes\":" << result.experimental.latency_escalation[i].second << '}';
    }
    out << "]},\n";

    out << "  \"level_summary\":[";
    for (std::size_t i = 0; i < result.level_summary.size(); ++i) {
        if (i) out << ',';
        const auto& s = result.level_summary[i];
        out << "{\"label\":\"" << json_escape(s.label) << "\","
            << "\"reported_size_bytes\":" << s.reported_size_bytes << ','
            << "\"reported_line_size_bytes\":" << s.reported_line_size_bytes << ','
            << "\"reported_ways\":" << s.reported_ways << ','
            << "\"representative_working_set_bytes\":" << s.representative_working_set_bytes << ','
            << "\"latency_ns\":" << s.latency_ns << ',' << "\"latency_cycles\":" << s.latency_cycles << ','
            << "\"sequential_read_gib_s\":" << s.sequential_read_gib_s << ','
            << "\"sequential_write_gib_s\":" << s.sequential_write_gib_s << ','
            << "\"copy_gib_s\":" << s.copy_gib_s << '}';
    }
    out << "],\n  \"warnings\":[";
    for (std::size_t i = 0; i < result.warnings.size(); ++i) {
        if (i) out << ',';
        out << '"' << json_escape(result.warnings[i]) << '"';
    }
    out << "]\n}\n";
}

// A conflict knee only means something if some cache actually maps that stride
// onto a single set, i.e. size / ways == stride. Saying which level (if any)
// that is turns a bare number into an interpretable result.
std::string associativity_context(const BenchmarkResult& result, std::uint64_t stride,
                                  std::uint32_t observed_ways) {
    for (const auto& c : result.system.caches) {
        if (c.type != CacheType::data && c.type != CacheType::unified) continue;
        if (c.associativity_ways == 0 || c.associativity_ways == 0xFFFFFFFFu) continue;
        if (c.size_bytes / c.associativity_ways != stride) continue;
        // The probe steps one block at a time, so an off-by-one against the
        // reported way count is measurement granularity, not a disagreement.
        const auto difference = observed_ways > c.associativity_ways
                                    ? observed_ways - c.associativity_ways
                                    : c.associativity_ways - observed_ways;
        std::ostringstream out;
        out << "- this stride maps onto one set of the reported L" << c.level << " ("
            << format_bytes(c.size_bytes) << ", " << c.associativity_ways << "-way), which the observed "
            << "knee " << (difference <= 1 ? "matches" : "does not match");
        return out.str();
    }
    return "- no reported cache has size/ways equal to this stride, so the knee reflects a "
           "combination of levels rather than one of them";
}

std::uint64_t level_size(const BenchmarkResult& result, int level) {
    std::uint64_t size = 0;
    for (const auto& c : result.system.caches) {
        if (c.level == level && (c.type == CacheType::data || c.type == CacheType::unified)) {
            size = std::max(size, c.size_bytes);
        }
    }
    return size;
}

void write_csv(std::ostream& out, const BenchmarkResult& result) {
    out << std::setprecision(12);
    out << "label,timestamp_utc,cpu,os,architecture,pointer_bits,compiler,preset,probe,probe_title,"
           "x_axis,y_axis,x_value,working_set_bytes,stride_bytes,element_count,median_ns_per_access,"
           "p05_ns_per_access,p95_ns_per_access,mad_ns,cv,median_gib_s,p05_gib_s,p95_gib_s,"
           "operations_per_sample,l1d_bytes,l2_bytes,l3_bytes,line_bytes,cpu_mhz\n";

    const auto l1 = level_size(result, 1);
    const auto l2 = level_size(result, 2);
    const auto l3 = level_size(result, 3);
    std::uint32_t line = 0;
    for (const auto& c : result.system.caches) line = std::max(line, c.line_size_bytes);
    // Peak-under-load is the clock the measured code actually ran at, so it is
    // the right basis for the cycle counts the comparison tool derives.
    const double mhz = result.system.cpu_mhz_peak > 0.0 ? result.system.cpu_mhz_peak
                                                        : result.system.cpu_mhz_start;

    for (const auto& series : result.series) {
        for (const auto& p : series.points) {
            out << csv_escape(result.label) << ',' << csv_escape(result.timestamp_utc) << ','
                << csv_escape(result.system.cpu_name) << ',' << csv_escape(result.system.operating_system)
                << ',' << csv_escape(result.system.architecture) << ',' << result.system.pointer_bits << ','
                << csv_escape(result.system.compiler) << ',' << csv_escape(result.config.preset) << ','
                << csv_escape(series.id) << ',' << csv_escape(series.title) << ','
                << x_axis_name(series.x) << ',' << y_axis_name(series.y) << ',' << series.x_value(p) << ','
                << p.working_set_bytes << ',' << p.stride_bytes << ',' << p.element_count << ','
                << p.ns_per_access.median << ',' << p.ns_per_access.p05 << ',' << p.ns_per_access.p95 << ','
                << p.ns_per_access.mad << ',' << p.ns_per_access.coefficient_of_variation << ','
                << p.gib_per_second.median << ',' << p.gib_per_second.p05 << ',' << p.gib_per_second.p95
                << ',' << p.operations_per_sample << ',' << l1 << ',' << l2 << ',' << l3 << ',' << line
                << ',' << mhz << '\n';
        }
    }
}

void write_markdown(std::ostream& out, const BenchmarkResult& result) {
    out << "# CacheScope report - " << markdown_escape(result.label) << "\n\n";
    out << "| Field | Value |\n|---|---|\n"
        << "| CPU | " << markdown_escape(result.system.cpu_name) << " |\n"
        << "| Operating system | " << markdown_escape(result.system.operating_system) << " |\n"
        << "| Architecture | " << markdown_escape(result.system.architecture) << " ("
        << result.system.pointer_bits << "-bit) |\n"
        << "| Logical CPUs | " << result.system.logical_cpus << " |\n"
        << "| Page size | " << format_bytes(result.system.page_size_bytes) << " |\n"
        << "| Installed RAM | "
        << (result.system.total_ram_bytes ? format_bytes(result.system.total_ram_bytes) : "unknown")
        << " |\n"
        << "| Compiler | " << markdown_escape(result.system.compiler) << " |\n"
        << "| Build | " << markdown_escape(result.system.build_flavor) << " |\n"
        << "| Preset | " << markdown_escape(result.config.preset) << ", " << result.config.samples
        << " samples/point |\n"
        << "| Thread pinned | "
        << (result.system.affinity_applied ? "CPU " + std::to_string(result.system.pinned_cpu) : "no")
        << " |\n"
        << "| Core MHz (idle / peak under load) | "
        << (result.system.cpu_mhz_start > 0 ? fixed(result.system.cpu_mhz_start, 0) : "unknown") << " / "
        << (result.system.cpu_mhz_peak > 0 ? fixed(result.system.cpu_mhz_peak, 0) : "unknown") << " |\n"
        << "| Run duration | " << fixed(result.duration_seconds, 1) << " s |\n"
        << "| Timestamp (UTC) | " << markdown_escape(result.timestamp_utc) << " |\n\n";

    out << "## Hardware-reported cache geometry\n\n";
    if (result.system.caches.empty()) {
        out << "The operating system did not report cache geometry on this machine.\n\n";
    } else {
        out << "| Cache | Size | Line | Ways | Shared by | Instances | Source |\n|---|---|---|---|---|---|---|\n";
        for (const auto& c : result.system.caches) {
            out << "| L" << c.level << ' ' << cache_type_name(c.type) << " | " << format_bytes(c.size_bytes)
                << " | " << c.line_size_bytes << " B | " << ways_text(c.associativity_ways) << " | "
                << (c.sharing_threads ? std::to_string(c.sharing_threads) : "-") << " | " << c.instances
                << " | " << geometry_source_name(c.source) << " |\n";
        }
        out << '\n';
    }

    out << "## Measured summary\n\n";
    out << "| Region | Reported | Measured set | Latency | Cycles | Read | Write | Copy |\n"
           "|---|---|---|---|---|---|---|---|\n";
    for (const auto& s : result.level_summary) {
        out << "| " << markdown_escape(s.label) << " | "
            << (s.reported_size_bytes ? format_bytes(s.reported_size_bytes) : "-") << " | "
            << format_bytes(s.representative_working_set_bytes) << " | " << maybe(s.latency_ns, 2, " ns")
            << " | " << maybe(s.latency_cycles, 1) << " | " << maybe(s.sequential_read_gib_s, 2, " GiB/s")
            << " | " << maybe(s.sequential_write_gib_s, 2, " GiB/s") << " | "
            << maybe(s.copy_gib_s, 2, " GiB/s") << " |\n";
    }
    out << '\n';

    out << "## Experimental findings\n\n";
    out << "These come from the measurements themselves, not from the operating system.\n\n";
    out << "- Serialized line-pair transition: "
        << (result.experimental.line_size_bytes
                ? std::to_string(result.experimental.line_size_bytes) + " B"
                : "not decisive")
        << '\n';
    out << "- Spatial-locality saturation stride (upper bound on the line size): "
        << (result.experimental.spatial_saturation_bytes
                ? std::to_string(result.experimental.spatial_saturation_bytes) + " B"
                : "not decisive")
        << '\n';
    out << "- Fastest dependent access: " << maybe(result.experimental.fastest_latency_ns, 2, " ns") << '\n';
    out << "- Latency escalation (working set at which latency reached each multiple of that floor): ";
    if (result.experimental.latency_escalation.empty()) {
        out << "latency never doubled inside the swept range";
    } else {
        for (std::size_t i = 0; i < result.experimental.latency_escalation.size(); ++i) {
            if (i) out << ", ";
            out << result.experimental.latency_escalation[i].first << "x at "
                << format_bytes(result.experimental.latency_escalation[i].second);
        }
    }
    out << '\n';
    out << "- TLB reach knee: "
        << (result.experimental.tlb_reach_pages
                ? std::to_string(result.experimental.tlb_reach_pages) + " pages"
                : "not detected")
        << '\n';
    for (const auto& [stride, ways] : result.experimental.associativity_ways) {
        out << "- Conflict-miss knee at " << format_bytes(stride) << " stride: " << ways
            << " blocks fit before thrashing " << markdown_escape(associativity_context(result, stride, ways))
            << '\n';
    }
    out << '\n';

    if (result.coherency.measured) {
        out << "## Cache coherence / false sharing\n\n";
        out << "| CPU pair | Same line | Separate lines | Single thread | Ping-pong |\n|---|---|---|---|---|\n";
        for (const auto& p : result.coherency.pairs) {
            out << "| " << markdown_escape(p.label) << " (cpu " << p.cpu_a << " + " << p.cpu_b << ") | "
                << maybe(p.shared_line_ns.median, 2, " ns") << " | "
                << maybe(p.padded_line_ns.median, 2, " ns") << " | "
                << maybe(p.single_thread_ns.median, 2, " ns") << " | "
                << maybe(p.ping_pong_ns.median, 2, " ns") << " |\n";
        }
        out << "\nThe \"same line\" column is the false-sharing cost: identical work, "
               "identical instruction count, only the memory layout differs.\n\n";
    }

    if (!result.warnings.empty()) {
        out << "## Warnings\n\n";
        for (const auto& w : result.warnings) out << "- " << markdown_escape(w) << '\n';
        out << '\n';
    }
    if (!result.system.notes.empty()) {
        out << "## System notes\n\n";
        for (const auto& n : result.system.notes) out << "- " << markdown_escape(n) << '\n';
        out << '\n';
    }
}

void write_html(std::ostream& out, const BenchmarkResult& result) {
    out << "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>CacheScope - "
        << html_escape(result.label) << "</title><style>" << kStyle << "</style></head><body><div class='wrap'>";

    out << "<section class='hero'><h1>CacheScope report</h1><p><strong>"
        << html_escape(result.label) << "</strong></p><p>" << html_escape(result.system.cpu_name) << " &middot; "
        << html_escape(result.system.architecture) << " &middot; " << result.system.pointer_bits
        << "-bit &middot; " << result.system.logical_cpus << " logical CPUs</p><p>"
        << html_escape(result.system.operating_system) << " &middot; "
        << html_escape(result.system.compiler) << " &middot; " << html_escape(result.timestamp_utc)
        << "</p></section>";

    out << "<div class='grid2'><section class='card'><h2>Run quality</h2><table>"
        << "<tr><th>Preset</th><td>" << html_escape(result.config.preset) << "</td></tr>"
        << "<tr><th>Samples / point</th><td>" << result.config.samples << "</td></tr>"
        << "<tr><th>Target sample time</th><td>" << result.config.target_sample_ms << " ms</td></tr>"
        << "<tr><th>Build</th><td>" << html_escape(result.system.build_flavor) << "</td></tr>"
        << "<tr><th>Clock resolution observed</th><td>" << fixed(result.system.steady_clock_resolution_ns, 1)
        << " ns</td></tr>"
        << "<tr><th>Thread pinned</th><td>"
        << (result.system.affinity_applied ? "yes, CPU " + std::to_string(result.system.pinned_cpu) : "no")
        << "</td></tr>"
        << "<tr><th>Core MHz idle / peak under load</th><td>"
        << (result.system.cpu_mhz_start > 0 ? fixed(result.system.cpu_mhz_start, 0) : "unknown") << " / "
        << (result.system.cpu_mhz_peak > 0 ? fixed(result.system.cpu_mhz_peak, 0) : "unknown") << "</td></tr>"
        << "<tr><th>Page size</th><td>" << format_bytes(result.system.page_size_bytes) << "</td></tr>"
        << "<tr><th>Installed RAM</th><td>"
        << (result.system.total_ram_bytes ? format_bytes(result.system.total_ram_bytes) : "unknown")
        << "</td></tr>"
        << "<tr><th>Run duration</th><td>" << fixed(result.duration_seconds, 1) << " s</td></tr>"
        << "</table></section>";

    out << "<section class='card'><h2>Hardware-reported cache geometry</h2>";
    if (result.system.caches.empty()) {
        out << "<p class='small'>This operating system did not report cache geometry. The experimental "
               "curves below still characterize the memory hierarchy.</p>";
    } else {
        out << "<table><tr><th>Cache</th><th>Size</th><th>Line</th><th>Ways</th><th>Shared by</th>"
               "<th>Instances</th><th>Source</th></tr>";
        for (const auto& c : result.system.caches) {
            out << "<tr><td>L" << c.level << ' ' << html_escape(cache_type_name(c.type)) << "</td><td>"
                << format_bytes(c.size_bytes) << "</td><td>" << c.line_size_bytes << " B</td><td>"
                << ways_text(c.associativity_ways) << "</td><td>"
                << (c.sharing_threads ? std::to_string(c.sharing_threads) : "-") << "</td><td>"
                << c.instances << "</td><td>" << geometry_source_name(c.source) << "</td></tr>";
        }
        out << "</table>";
    }
    out << "</section></div>";

    out << "<section class='card'><h2>Comparison summary</h2>"
           "<p class='small'>Representative points sit near 50% of each reported cache size, which limits "
           "boundary mixing. This is not a claim of perfect cache isolation. Cycle counts use the core "
           "frequency reported by the OS during the run and are approximate.</p>"
           "<table><tr><th>Region</th><th>Reported</th><th>Ways</th><th>Measured set</th>"
           "<th>Dependent latency</th><th>Cycles</th><th>Read</th><th>Write</th><th>Copy</th></tr>";
    for (const auto& s : result.level_summary) {
        out << "<tr><td>" << html_escape(s.label) << "</td><td>"
            << (s.reported_size_bytes ? format_bytes(s.reported_size_bytes) : "-") << "</td><td>"
            << ways_text(s.reported_ways) << "</td><td>"
            << format_bytes(s.representative_working_set_bytes) << "</td><td class='metric'>"
            << maybe(s.latency_ns, 2, " ns") << "</td><td class='metric'>" << maybe(s.latency_cycles, 1)
            << "</td><td class='metric'>" << maybe(s.sequential_read_gib_s, 2, " GiB/s")
            << "</td><td class='metric'>" << maybe(s.sequential_write_gib_s, 2, " GiB/s")
            << "</td><td class='metric'>" << maybe(s.copy_gib_s, 2, " GiB/s") << "</td></tr>";
    }
    out << "</table></section>";

    out << "<section class='card'><h2>Experimental findings<span class='tag'>measured, not reported</span></h2><table>"
        << "<tr><th>Serialized line-pair transition</th><td>"
        << (result.experimental.line_size_bytes
                ? std::to_string(result.experimental.line_size_bytes) + " B"
                : "not decisive")
        << "</td></tr><tr><th>Spatial-locality saturation stride</th><td>"
        << (result.experimental.spatial_saturation_bytes
                ? std::to_string(result.experimental.spatial_saturation_bytes) +
                      " B <span class='small'>stride beyond which each element costs a full transfer. "
                      "An upper bound on the line size: hardware prefetchers keep larger strides cheap "
                      "and push this outwards.</span>"
                : "not decisive")
        << "</td></tr><tr><th>Fastest dependent access</th><td>"
        << maybe(result.experimental.fastest_latency_ns, 2, " ns")
        << " <span class='small'>the machine's own floor, used as the reference below</span>"
        << "</td></tr><tr><th>Latency escalation</th><td>";
    if (result.experimental.latency_escalation.empty()) {
        out << "latency never doubled inside the swept range";
    } else {
        for (std::size_t i = 0; i < result.experimental.latency_escalation.size(); ++i) {
            if (i) out << ", ";
            out << result.experimental.latency_escalation[i].first << "x at "
                << format_bytes(result.experimental.latency_escalation[i].second);
        }
    }
    out << "</td></tr><tr><th>TLB reach knee</th><td>"
        << (result.experimental.tlb_reach_pages
                ? std::to_string(result.experimental.tlb_reach_pages) + " pages (" +
                      format_bytes(result.experimental.tlb_reach_pages * result.system.page_size_bytes) +
                      " of address space)"
                : "not detected")
        << "</td></tr>";
    for (const auto& [stride, ways] : result.experimental.associativity_ways) {
        out << "<tr><th>Conflict knee at " << format_bytes(stride) << " stride</th><td>" << ways
            << " blocks fit before thrashing <span class='small'>"
            << html_escape(associativity_context(result, stride, ways)) << "</span></td></tr>";
    }
    out << "</table></section>";

    if (result.coherency.measured) {
        out << "<section class='card'><h2>Cache coherence and false sharing</h2>"
               "<p class='small'>Two threads perform exactly the same number of atomic increments. The only "
               "difference between the first two columns is whether their counters share one cache line.</p>"
               "<table><tr><th>CPU pair</th><th>Same line</th><th>Separate lines</th><th>Single thread</th>"
               "<th>Ping-pong round trip</th><th>False-sharing penalty</th></tr>";
        for (const auto& p : result.coherency.pairs) {
            const double penalty = p.padded_line_ns.median > 0.0
                                       ? p.shared_line_ns.median / p.padded_line_ns.median
                                       : 0.0;
            out << "<tr><td>" << html_escape(p.label) << " <span class='small'>(cpu " << p.cpu_a << " + "
                << p.cpu_b << (p.pinned ? ", pinned" : ", not pinned") << ")</span></td>"
                << "<td class='metric'>" << maybe(p.shared_line_ns.median, 2, " ns") << "</td>"
                << "<td class='metric'>" << maybe(p.padded_line_ns.median, 2, " ns") << "</td>"
                << "<td class='metric'>" << maybe(p.single_thread_ns.median, 2, " ns") << "</td>"
                << "<td class='metric'>" << maybe(p.ping_pong_ns.median, 2, " ns") << "</td>"
                << "<td class='metric'>" << maybe(penalty, 1, "x") << "</td></tr>";
        }
        out << "</table></section>";
    }

    if (!result.warnings.empty()) {
        out << "<section class='card'><h2>Warnings</h2>";
        for (const auto& w : result.warnings) out << "<div class='warn'>" << html_escape(w) << "</div>";
        out << "</section>";
    }
    if (!result.system.notes.empty()) {
        out << "<section class='card'><h2>System notes</h2><ul class='note'>";
        for (const auto& n : result.system.notes) out << "<li>" << html_escape(n) << "</li>";
        out << "</ul></section>";
    }

    for (const auto& series : result.series) {
        out << svg_chart(series, cache_markers(result, series));
    }

    out << "<section class='card note'><h2>How to read this report</h2>"
           "<p><strong>Dependent latency.</strong> Each load's address comes from the previous load, so the "
           "CPU cannot overlap misses. The curve therefore steps up as the working set outgrows each cache. "
           "Streaming read/write/copy curves are the opposite: they deliberately allow vectorization, "
           "prefetching and multiple outstanding requests, so they show practical throughput rather than "
           "single-access latency.</p>"
           "<p><strong>Conflict-miss probe.</strong> Blocks spaced by a power-of-two stride compete for the "
           "same cache set. When the number of blocks passes the associativity of the cache whose "
           "size/ways equals that stride, every access starts missing. The knee is the observed way count "
           "for that level.</p>"
           "<p><strong>TLB probe.</strong> Only one cache line is touched per page, so the resident data "
           "stays small while the number of translations grows. Knees here are TLB effects, not cache "
           "effects, which is why huge pages change them.</p>"
           "<p><strong>Reported versus measured.</strong> The geometry table is what the OS or CPUID says. "
           "Everything else is what this machine actually did. Real CPUs have sliced and non-inclusive "
           "caches, adjacent-line and stride prefetchers, hybrid core types, NUMA effects, cache hashing, "
           "SMT contention, frequency scaling and virtualization, so a measured knee need not land exactly "
           "on a reported boundary. Compare machines using the same CacheScope version, the same preset, "
           "and comparable power and thermal conditions, and prefer three runs per machine over one.</p>"
           "</section>";

    out << "</div></body></html>";
}

std::filesystem::path unique_path(const std::filesystem::path& directory,
                                  const std::string& stem,
                                  const std::string& extension) {
    std::filesystem::path candidate = directory / (stem + extension);
    int suffix = 2;
    std::error_code ec;
    while (std::filesystem::exists(candidate, ec) && suffix < 1000) {
        candidate = directory / (stem + "_" + std::to_string(suffix) + extension);
        ++suffix;
    }
    return candidate;
}

} // namespace

ReportPaths write_reports(const BenchmarkResult& result,
                          const std::filesystem::path& output_directory,
                          ReportFormat formats) {
    std::error_code ec;
    std::filesystem::create_directories(output_directory, ec);
    if (ec && !std::filesystem::exists(output_directory)) {
        throw std::runtime_error("could not create report directory: " + ec.message());
    }

    std::string stem = "cachescope_" + compact_timestamp(result.timestamp_utc);
    const auto label_part = sanitize_stem(result.label);
    if (!label_part.empty()) stem += "_" + label_part;

    ReportPaths paths;
    auto emit = [&](ReportFormat format, const char* extension, auto&& writer) {
        if (!has_format(formats, format)) return std::filesystem::path{};
        const auto path = unique_path(output_directory, stem, extension);
        std::ofstream out(path, std::ios::binary);
        if (!out) throw std::runtime_error("could not open report file: " + path.string());
        writer(out);
        out.flush();
        if (!out) throw std::runtime_error("could not write report file: " + path.string());
        return path;
    };

    paths.json = emit(ReportFormat::json, ".json", [&](std::ostream& o) { write_json(o, result); });
    paths.csv = emit(ReportFormat::csv, ".csv", [&](std::ostream& o) { write_csv(o, result); });
    paths.html = emit(ReportFormat::html, ".html", [&](std::ostream& o) { write_html(o, result); });
    paths.markdown = emit(ReportFormat::markdown, ".md", [&](std::ostream& o) { write_markdown(o, result); });
    return paths;
}

} // namespace cachescope
