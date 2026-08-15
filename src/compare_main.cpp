#include <cachescope/benchmark.hpp>
#include <cachescope/compare.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::cout << R"(cachescope_compare - overlay CacheScope CSV reports

Usage:
  cachescope_compare [--out comparison.html] <csv-or-directory> [more...]

Arguments may be individual CacheScope .csv reports or directories, in which
case every .csv inside is used. One input is allowed (useful for re-plotting a
single machine); two or more produce overlaid curves.

Examples:
  cachescope_compare --out comparison.html old_laptop.csv new_laptop.csv
  cachescope_compare --out comparison.html results
)";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path out = "cachescope_comparison.html";
        std::vector<std::filesystem::path> inputs;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--out" || arg == "-o") {
                if (++i >= argc) throw std::runtime_error("--out requires a path");
                out = argv[i];
            } else if (arg == "-h" || arg == "--help") {
                usage();
                return 0;
            } else if (arg == "--version") {
                std::cout << "cachescope_compare " << cachescope::kVersion << '\n';
                return 0;
            } else if (!arg.empty() && arg.front() == '-') {
                throw std::runtime_error("unknown option: " + std::string(arg));
            } else {
                inputs.emplace_back(argv[i]);
            }
        }
        if (inputs.empty()) {
            usage();
            throw std::runtime_error("provide at least one CacheScope CSV report or a directory");
        }

        const auto files = cachescope::expand_csv_inputs(inputs);
        if (files.empty()) throw std::runtime_error("no .csv reports found in the given inputs");

        std::vector<cachescope::ComparisonRun> runs;
        runs.reserve(files.size());
        for (const auto& path : files) {
            runs.push_back(cachescope::read_csv_run(path));
            std::cout << "  loaded " << path.filename().string() << "  (" << runs.back().label << ")\n";
        }
        cachescope::write_comparison(runs, out);
        std::cout << "\nComparison report: " << std::filesystem::absolute(out).string() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "CacheScope compare error: " << e.what() << '\n';
        return 1;
    }
}
