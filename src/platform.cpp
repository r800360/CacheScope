// Platform introspection: CPU identity, hardware-reported cache geometry,
// page size, memory size, thread affinity and core frequency.
//
// Everything in here is best effort by design. When an operating system does
// not expose a fact, the corresponding field stays zero/empty and the report
// says "unknown" instead of guessing, so that experimental observations are
// never confused with hardware-reported geometry.

#include <cachescope/benchmark.hpp>
#include "platform_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  if defined(CACHESCOPE_HAVE_POWRPROF)
#    if __has_include(<powerbase.h>)
#      include <powerbase.h>
#    elif __has_include(<powrprof.h>)
#      include <powrprof.h>
#    else
#      undef CACHESCOPE_HAVE_POWRPROF
#    endif
#  endif
#  if defined(CACHESCOPE_HAVE_POWRPROF)
// PROCESSOR_POWER_INFORMATION is documented but is not declared by every
// Windows SDK, so declare a layout-compatible struct under our own name. The
// layout has been stable since Windows XP.
struct CacheScopeProcessorPowerInformation {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};
#  endif
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#elif defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#else
#  if __has_include(<unistd.h>)
#    include <unistd.h>
#    define CACHESCOPE_HAS_UNISTD 1
#  endif
#  if __has_include(<sys/utsname.h>)
#    include <sys/utsname.h>
#    define CACHESCOPE_HAS_UNAME 1
#  endif
#endif

// x86 CPUID is used only as a fallback when the OS reports no cache geometry
// (containers with no /sys, exotic or very old systems).
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#  define CACHESCOPE_X86 1
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif
#endif

namespace cachescope {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string architecture_name() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
#elif defined(__riscv) && (__riscv_xlen == 32)
    return "riscv32";
#elif defined(__powerpc64__)
    return "ppc64";
#elif defined(__powerpc__)
    return "ppc";
#elif defined(__s390x__)
    return "s390x";
#elif defined(__loongarch64)
    return "loongarch64";
#else
    return "unknown";
#endif
}

std::string compiler_name() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#else
    return "unknown";
#endif
}

double empirical_clock_resolution_ns() {
    using clock = std::chrono::steady_clock;
    auto previous = clock::now();
    auto best = std::chrono::nanoseconds::max();
    for (int i = 0; i < 20'000; ++i) {
        const auto now = clock::now();
        const auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous);
        if (delta.count() > 0 && delta < best) best = delta;
        previous = now;
    }
    if (best == std::chrono::nanoseconds::max()) return 0.0;
    return static_cast<double>(best.count());
}

// ---------------------------------------------------------------------------
// x86 CPUID fallback (Intel leaf 4 / AMD leaf 0x8000001D deterministic cache
// parameters). Covers essentially every 64-bit x86 part and late 32-bit parts.
// ---------------------------------------------------------------------------
#if defined(CACHESCOPE_X86)
struct CpuidRegs { unsigned eax, ebx, ecx, edx; };

bool cpuid_count(unsigned leaf, unsigned subleaf, CpuidRegs& out) {
#if defined(_MSC_VER)
    int regs[4]{};
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    out = {static_cast<unsigned>(regs[0]), static_cast<unsigned>(regs[1]),
           static_cast<unsigned>(regs[2]), static_cast<unsigned>(regs[3])};
    return true;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid_count(leaf, subleaf, &a, &b, &c, &d) == 0) return false;
    out = {a, b, c, d};
    return true;
#endif
}

unsigned cpuid_max_leaf(unsigned base) {
    CpuidRegs r{};
    if (!cpuid_count(base, 0, r)) return 0;
    return r.eax;
}

std::string cpuid_brand_string() {
    if (cpuid_max_leaf(0x80000000u) < 0x80000004u) return {};
    std::array<char, 49> buffer{};
    for (unsigned i = 0; i < 3; ++i) {
        CpuidRegs r{};
        if (!cpuid_count(0x80000002u + i, 0, r)) return {};
        std::memcpy(buffer.data() + i * 16 + 0, &r.eax, 4);
        std::memcpy(buffer.data() + i * 16 + 4, &r.ebx, 4);
        std::memcpy(buffer.data() + i * 16 + 8, &r.ecx, 4);
        std::memcpy(buffer.data() + i * 16 + 12, &r.edx, 4);
    }
    return trim(buffer.data());
}

std::vector<CacheInfo> detect_x86_caches() {
    std::vector<CacheInfo> caches;
    unsigned leaf = 0;
    if (cpuid_max_leaf(0) >= 4) {
        leaf = 4;
    } else if (cpuid_max_leaf(0x80000000u) >= 0x8000001Du) {
        leaf = 0x8000001Du;
    } else {
        return caches;
    }

    for (unsigned sub = 0; sub < 16; ++sub) {
        CpuidRegs r{};
        if (!cpuid_count(leaf, sub, r)) break;
        const unsigned type = r.eax & 0x1Fu;
        if (type == 0) break;  // no more caches

        CacheInfo c;
        c.level = static_cast<int>((r.eax >> 5) & 0x7u);
        switch (type) {
            case 1: c.type = CacheType::data; break;
            case 2: c.type = CacheType::instruction; break;
            case 3: c.type = CacheType::unified; break;
            default: c.type = CacheType::unknown; break;
        }
        const std::uint64_t ways = ((r.ebx >> 22) & 0x3FFu) + 1u;
        const std::uint64_t partitions = ((r.ebx >> 12) & 0x3FFu) + 1u;
        const std::uint64_t line = (r.ebx & 0xFFFu) + 1u;
        const std::uint64_t sets = static_cast<std::uint64_t>(r.ecx) + 1u;
        c.size_bytes = ways * partitions * line * sets;
        c.line_size_bytes = static_cast<std::uint32_t>(line);
        c.associativity_ways = (r.eax & (1u << 9)) ? 0xFFFFFFFFu : static_cast<std::uint32_t>(ways);
        c.sharing_threads = ((r.eax >> 14) & 0xFFFu) + 1u;
        c.source = GeometrySource::cpuid;
        if (c.size_bytes > 0) caches.push_back(c);
    }
    std::sort(caches.begin(), caches.end(), [](const CacheInfo& a, const CacheInfo& b) {
        if (a.level != b.level) return a.level < b.level;
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    });
    return caches;
}
#endif  // CACHESCOPE_X86

// ---------------------------------------------------------------------------
// Linux
// ---------------------------------------------------------------------------
#if defined(__linux__)
std::uint64_t parse_linux_size(const std::string& text) {
    const auto s = trim(text);
    if (s.empty()) return 0;
    std::size_t used = 0;
    std::uint64_t value = 0;
    try {
        value = std::stoull(s, &used);
    } catch (...) {
        return 0;
    }
    if (used < s.size()) {
        const char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(s[used])));
        if (suffix == 'K') value *= 1024ull;
        else if (suffix == 'M') value *= 1024ull * 1024ull;
        else if (suffix == 'G') value *= 1024ull * 1024ull * 1024ull;
    }
    return value;
}

std::uint32_t count_cpu_list(const std::string& text) {
    std::uint32_t count = 0;
    std::stringstream ss(text);
    std::string token;
    while (std::getline(ss, token, ',')) {
        const auto dash = token.find('-');
        try {
            if (dash == std::string::npos) {
                (void)std::stoi(token);
                ++count;
            } else {
                const int a = std::stoi(token.substr(0, dash));
                const int b = std::stoi(token.substr(dash + 1));
                if (b >= a) count += static_cast<std::uint32_t>(b - a + 1);
            }
        } catch (...) {
        }
    }
    return count;
}

std::string read_first_line(const std::filesystem::path& path) {
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    return trim(line);
}

std::vector<CacheInfo> detect_linux_caches() {
    std::vector<CacheInfo> result;
    const std::filesystem::path base = "/sys/devices/system/cpu/cpu0/cache";
    std::error_code ec;
    if (!std::filesystem::exists(base, ec)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
        if (ec || !entry.is_directory()) continue;
        const auto name = entry.path().filename().string();
        if (name.rfind("index", 0) != 0) continue;

        CacheInfo c;
        try {
            c.level = std::stoi(read_first_line(entry.path() / "level"));
        } catch (...) {
            continue;
        }
        const auto type = read_first_line(entry.path() / "type");
        if (type == "Data") c.type = CacheType::data;
        else if (type == "Instruction") c.type = CacheType::instruction;
        else if (type == "Unified") c.type = CacheType::unified;
        c.size_bytes = parse_linux_size(read_first_line(entry.path() / "size"));
        try {
            c.line_size_bytes =
                static_cast<std::uint32_t>(std::stoul(read_first_line(entry.path() / "coherency_line_size")));
        } catch (...) {
        }
        try {
            c.associativity_ways =
                static_cast<std::uint32_t>(std::stoul(read_first_line(entry.path() / "ways_of_associativity")));
        } catch (...) {
        }
        c.sharing_threads = count_cpu_list(read_first_line(entry.path() / "shared_cpu_list"));
        c.source = GeometrySource::operating_system;
        if (c.size_bytes > 0) result.push_back(c);
    }
    std::sort(result.begin(), result.end(), [](const CacheInfo& a, const CacheInfo& b) {
        if (a.level != b.level) return a.level < b.level;
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    });
    return result;
}

std::string detect_linux_cpu_name() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto key = trim(line.substr(0, colon));
        if (key == "model name" || key == "Hardware" || key == "Processor" || key == "cpu model") {
            auto value = trim(line.substr(colon + 1));
            if (!value.empty()) return value;
        }
    }
    return {};
}

void collect_linux_notes(SystemInfo& info) {
    const auto thp = read_first_line("/sys/kernel/mm/transparent_hugepage/enabled");
    if (!thp.empty()) info.notes.push_back("transparent huge pages: " + thp);

    const auto governor =
        read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (!governor.empty()) info.notes.push_back("cpufreq governor (cpu0): " + governor);

    std::ifstream numa("/sys/devices/system/node/online");
    std::string nodes;
    if (numa && std::getline(numa, nodes) && !trim(nodes).empty() && trim(nodes) != "0") {
        info.notes.push_back("NUMA nodes online: " + trim(nodes) +
                             " (cross-node placement can change memory latency)");
    }
}
#endif  // __linux__

// ---------------------------------------------------------------------------
// Apple
// ---------------------------------------------------------------------------
#if defined(__APPLE__)
template <typename T>
bool sysctl_value(const char* name, T& out) {
    T value{};
    std::size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) == 0 && size == sizeof(value)) {
        out = value;
        return true;
    }
    return false;
}

std::string sysctl_string(const char* name) {
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return trim(value);
}

void push_apple_cache(std::vector<CacheInfo>& caches, int level, CacheType type, const char* key,
                      std::uint32_t line, std::uint32_t sharing) {
    std::uint64_t size = 0;
    if (!sysctl_value(key, size) || size == 0) return;
    CacheInfo c;
    c.level = level;
    c.type = type;
    c.size_bytes = size;
    c.line_size_bytes = line;
    c.sharing_threads = sharing;
    c.source = GeometrySource::operating_system;
    caches.push_back(c);
}

std::vector<CacheInfo> detect_apple_caches(std::vector<std::string>& notes) {
    std::vector<CacheInfo> caches;
    std::uint64_t line64 = 0;
    sysctl_value("hw.cachelinesize", line64);
    const auto line = static_cast<std::uint32_t>(line64);

    // Apple Silicon exposes per-performance-level geometry; Intel Macs use the
    // flat hw.l*cachesize keys. Query both and keep whatever exists.
    std::uint32_t perf_levels = 0;
    sysctl_value("hw.nperflevels", perf_levels);
    if (perf_levels > 1) {
        for (std::uint32_t level = 0; level < perf_levels && level < 4; ++level) {
            const std::string prefix = "hw.perflevel" + std::to_string(level) + ".";
            std::uint32_t sharing = 0;
            sysctl_value((prefix + "cpusperl2").c_str(), sharing);
            push_apple_cache(caches, 1, CacheType::data, (prefix + "l1dcachesize").c_str(), line, 1);
            push_apple_cache(caches, 1, CacheType::instruction, (prefix + "l1icachesize").c_str(), line, 1);
            push_apple_cache(caches, 2, CacheType::unified, (prefix + "l2cachesize").c_str(), line, sharing);
        }
        notes.emplace_back(
            "Apple heterogeneous CPU: performance and efficiency cores have different cache "
            "geometry; pin comparisons to the same core class.");
    }
    if (caches.empty()) {
        push_apple_cache(caches, 1, CacheType::data, "hw.l1dcachesize", line, 1);
        push_apple_cache(caches, 1, CacheType::instruction, "hw.l1icachesize", line, 1);
        push_apple_cache(caches, 2, CacheType::unified, "hw.l2cachesize", line, 0);
    }
    push_apple_cache(caches, 3, CacheType::unified, "hw.l3cachesize", line, 0);
    std::sort(caches.begin(), caches.end(), [](const CacheInfo& a, const CacheInfo& b) {
        if (a.level != b.level) return a.level < b.level;
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    });
    return caches;
}
#endif  // __APPLE__

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------
#if defined(_WIN32)
std::string windows_cpu_name() {
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    char buffer[512]{};
    DWORD type = 0;
    DWORD size = sizeof(buffer) - 1;
    const auto status = RegQueryValueExA(key, "ProcessorNameString", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) return trim(buffer);
    return {};
}

DWORD windows_registry_mhz() {
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    const auto status =
        RegQueryValueExA(key, "~MHz", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return (status == ERROR_SUCCESS && type == REG_DWORD) ? value : 0;
}

// CACHE_RELATIONSHIP grew a GroupCount/GroupMasks[] union in newer Windows SDKs
// while older SDKs only have a single GroupMask. Detect which shape this SDK
// provides instead of assuming, so the same source builds against both.
template <class Cache>
std::uint32_t cache_sharing_threads(const Cache& cache) {
    if constexpr (requires { cache.GroupCount; cache.GroupMasks[0].Mask; }) {
        if (cache.GroupCount > 0) {
            std::uint32_t total = 0;
            for (WORD i = 0; i < cache.GroupCount; ++i) {
                total += static_cast<std::uint32_t>(
                    std::popcount(static_cast<std::uint64_t>(cache.GroupMasks[i].Mask)));
            }
            return total;
        }
    }
    return static_cast<std::uint32_t>(
        std::popcount(static_cast<std::uint64_t>(cache.GroupMask.Mask)));
}

std::vector<CacheInfo> detect_windows_caches() {
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return {};
    std::vector<std::byte> storage(bytes);
    auto* base = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
    if (!GetLogicalProcessorInformationEx(RelationCache, base, &bytes)) return {};

    std::vector<CacheInfo> raw;
    std::byte* cursor = storage.data();
    std::byte* const end = storage.data() + bytes;
    while (cursor < end) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(cursor);
        if (info->Size == 0 || cursor + info->Size > end) break;
        if (info->Relationship == RelationCache) {
            const auto& w = info->Cache;
            CacheInfo c;
            c.level = w.Level;
            c.size_bytes = static_cast<std::uint64_t>(w.CacheSize);
            c.line_size_bytes = w.LineSize;
            c.sharing_threads = cache_sharing_threads(w);
            // 0xFF means "fully associative" in the Windows API.
            c.associativity_ways =
                w.Associativity == 0xFF ? 0xFFFFFFFFu : static_cast<std::uint32_t>(w.Associativity);
            switch (w.Type) {
                case CacheData: c.type = CacheType::data; break;
                case CacheInstruction: c.type = CacheType::instruction; break;
                case CacheUnified: c.type = CacheType::unified; break;
                default: c.type = CacheType::unknown; break;
            }
            c.source = GeometrySource::operating_system;
            if (c.size_bytes > 0) raw.push_back(c);
        }
        cursor += info->Size;
    }

    // Windows reports one record per cache instance; fold identical instances
    // together and keep the count so the report can show "8 instances".
    std::vector<CacheInfo> dedup;
    for (const auto& c : raw) {
        auto it = std::find_if(dedup.begin(), dedup.end(), [&](const CacheInfo& d) {
            return d.level == c.level && d.type == c.type && d.size_bytes == c.size_bytes &&
                   d.line_size_bytes == c.line_size_bytes && d.sharing_threads == c.sharing_threads &&
                   d.associativity_ways == c.associativity_ways;
        });
        if (it == dedup.end()) dedup.push_back(c);
        else ++it->instances;
    }
    std::sort(dedup.begin(), dedup.end(), [](const CacheInfo& a, const CacheInfo& b) {
        if (a.level != b.level) return a.level < b.level;
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    });
    return dedup;
}

std::string windows_version_string() {
    std::string name = "Windows";
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        char product[256]{};
        DWORD type = 0;
        DWORD size = sizeof(product) - 1;
        if (RegQueryValueExA(key, "ProductName", nullptr, &type, reinterpret_cast<LPBYTE>(product),
                             &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            name = trim(product);
        }
        char build[64]{};
        size = sizeof(build) - 1;
        if (RegQueryValueExA(key, "CurrentBuildNumber", nullptr, &type,
                             reinterpret_cast<LPBYTE>(build), &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            name += " build " + trim(build);
        }
        RegCloseKey(key);
    }
    return name;
}
#endif  // _WIN32

std::string os_name() {
#if defined(_WIN32)
    return windows_version_string();
#elif defined(__APPLE__)
    struct utsname u{};
    const auto product = sysctl_string("kern.osproductversion");
    if (!product.empty()) return "macOS " + product;
    if (uname(&u) == 0) return std::string("Darwin ") + u.release;
    return "macOS";
#elif defined(__linux__)
    struct utsname u{};
    if (uname(&u) == 0) return std::string("Linux ") + u.release;
    return "Linux";
#elif defined(CACHESCOPE_HAS_UNAME)
    struct utsname u{};
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
    return "POSIX";
#else
    return "unknown";
#endif
}

std::uint64_t detect_page_size() {
#if defined(_WIN32)
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwPageSize;
#elif defined(_SC_PAGESIZE)
    const long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::uint64_t>(value) : 4096ull;
#else
    return 4096ull;
#endif
}

std::uint64_t detect_total_ram() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) return status.ullTotalPhys;
    return 0;
#elif defined(__APPLE__)
    std::uint64_t value = 0;
    if (sysctl_value("hw.memsize", value)) return value;
    return 0;
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
    return 0;
#else
    return 0;
#endif
}

std::string build_flavor() {
#if defined(NDEBUG)
    return "optimized (NDEBUG)";
#else
    return "unoptimized/debug";
#endif
}

} // namespace

namespace detail {

std::vector<int> allowed_cpus() {
    std::vector<int> cpus;
#if defined(_WIN32)
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask != 0) {
        for (int cpu = 0; cpu < static_cast<int>(sizeof(DWORD_PTR) * 8); ++cpu) {
            if (process_mask & (static_cast<DWORD_PTR>(1) << cpu)) cpus.push_back(cpu);
        }
    }
    if (cpus.empty()) {
        for (DWORD cpu = 0; cpu < count; ++cpu) cpus.push_back(static_cast<int>(cpu));
    }
#elif defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
        }
    }
#endif
    if (cpus.empty()) {
        const unsigned n = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned cpu = 0; cpu < n; ++cpu) cpus.push_back(static_cast<int>(cpu));
    }
    return cpus;
}

bool pin_current_thread(int cpu) {
    if (cpu < 0) return false;
#if defined(_WIN32)
    GROUP_AFFINITY previous{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &previous)) return false;
    GROUP_AFFINITY target{};
    target.Group = previous.Group;
    target.Mask = static_cast<KAFFINITY>(1) << (cpu % static_cast<int>(sizeof(KAFFINITY) * 8));
    return SetThreadGroupAffinity(GetCurrentThread(), &target, nullptr) != 0;
#elif defined(__linux__)
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    return false;
#endif
}

bool apply_affinity(SystemInfo& info, int preferred_cpu) {
    const auto cpus = allowed_cpus();
    if (cpus.empty()) return false;
    int chosen = cpus.front();
    if (preferred_cpu >= 0) {
        if (std::find(cpus.begin(), cpus.end(), preferred_cpu) != cpus.end()) {
            chosen = preferred_cpu;
        } else {
            chosen = cpus[static_cast<std::size_t>(preferred_cpu) % cpus.size()];
        }
    }
    if (!pin_current_thread(chosen)) return false;
    info.affinity_applied = true;
    info.pinned_cpu = chosen;
    return true;
}

#if defined(_WIN32) && defined(CACHESCOPE_HAVE_POWRPROF)
// Returns CurrentMhz (want_max == false) or MaxMhz for the given logical CPU.
double windows_power_mhz(int cpu, bool want_max) {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count == 0) return 0.0;
    std::vector<CacheScopeProcessorPowerInformation> info(count);
    const LONG status = CallNtPowerInformation(
        ProcessorInformation, nullptr, 0, info.data(),
        static_cast<ULONG>(info.size() * sizeof(CacheScopeProcessorPowerInformation)));
    if (status != 0) return 0.0;
    const std::size_t index =
        (cpu >= 0 && static_cast<std::size_t>(cpu) < info.size()) ? static_cast<std::size_t>(cpu) : 0;
    const ULONG value = want_max ? info[index].MaxMhz : info[index].CurrentMhz;
    return static_cast<double>(value);
}
#endif

double current_cpu_mhz(int cpu) {
#if defined(_WIN32)
#  if defined(CACHESCOPE_HAVE_POWRPROF)
    if (const double mhz = windows_power_mhz(cpu, false); mhz > 0.0) return mhz;
#  else
    (void)cpu;
#  endif
    return static_cast<double>(windows_registry_mhz());
#elif defined(__linux__)
    const int target = cpu >= 0 ? cpu : 0;
    const std::string path =
        "/sys/devices/system/cpu/cpu" + std::to_string(target) + "/cpufreq/scaling_cur_freq";
    const auto text = read_first_line(path);
    if (!text.empty()) {
        try {
            return std::stod(text) / 1000.0;  // kHz -> MHz
        } catch (...) {
        }
    }
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (trim(line.substr(0, colon)) != "cpu MHz") continue;
        try {
            return std::stod(trim(line.substr(colon + 1)));
        } catch (...) {
        }
    }
    return 0.0;
#elif defined(__APPLE__)
    (void)cpu;
    std::uint64_t hz = 0;
    if (sysctl_value("hw.cpufrequency", hz) && hz > 0) return static_cast<double>(hz) / 1e6;
    return 0.0;
#else
    (void)cpu;
    return 0.0;
#endif
}

double maximum_cpu_mhz(int cpu) {
#if defined(_WIN32)
#  if defined(CACHESCOPE_HAVE_POWRPROF)
    if (const double mhz = windows_power_mhz(cpu, true); mhz > 0.0) return mhz;
#  else
    (void)cpu;
#  endif
    return 0.0;
#elif defined(__linux__)
    const int target = cpu >= 0 ? cpu : 0;
    const auto text = read_first_line("/sys/devices/system/cpu/cpu" + std::to_string(target) +
                                      "/cpufreq/cpuinfo_max_freq");
    if (!text.empty()) {
        try {
            return std::stod(text) / 1000.0;  // kHz -> MHz
        } catch (...) {
        }
    }
    return 0.0;
#else
    (void)cpu;
    return 0.0;
#endif
}

} // namespace detail

SystemInfo detect_system() {
    SystemInfo info;
    info.architecture = architecture_name();
    info.operating_system = os_name();
    info.compiler = compiler_name();
    info.build_flavor = build_flavor();
    info.pointer_bits = static_cast<int>(sizeof(void*) * 8);
    info.logical_cpus = std::max(1u, std::thread::hardware_concurrency());
    info.page_size_bytes = detect_page_size();
    info.total_ram_bytes = detect_total_ram();
    info.steady_clock_resolution_ns = empirical_clock_resolution_ns();

#if defined(_WIN32)
    info.cpu_name = windows_cpu_name();
    info.caches = detect_windows_caches();
    info.affinity_supported = true;
#elif defined(__APPLE__)
    info.cpu_name = sysctl_string("machdep.cpu.brand_string");
    if (info.cpu_name.empty()) info.cpu_name = sysctl_string("hw.model");
    info.caches = detect_apple_caches(info.notes);
    // macOS exposes affinity *hints* (THREAD_AFFINITY_POLICY) rather than strict
    // pinning, and Apple Silicon ignores them entirely.
    info.affinity_supported = false;
#elif defined(__linux__)
    info.cpu_name = detect_linux_cpu_name();
    info.caches = detect_linux_caches();
    info.affinity_supported = true;
    collect_linux_notes(info);
#else
    info.affinity_supported = false;
#endif

#if defined(CACHESCOPE_X86)
    if (info.cpu_name.empty()) info.cpu_name = cpuid_brand_string();
    if (info.caches.empty()) {
        info.caches = detect_x86_caches();
        if (!info.caches.empty()) {
            info.notes.emplace_back(
                "Cache geometry came from CPUID because this OS did not report it.");
        }
    }
#endif
    if (info.cpu_name.empty()) info.cpu_name = "unknown CPU";

    info.cpu_mhz_start = detail::current_cpu_mhz(info.pinned_cpu);
    return info;
}

std::string cache_type_name(CacheType type) {
    switch (type) {
        case CacheType::data: return "data";
        case CacheType::instruction: return "instruction";
        case CacheType::unified: return "unified";
        default: return "unknown";
    }
}

std::string geometry_source_name(GeometrySource source) {
    switch (source) {
        case GeometrySource::operating_system: return "os";
        case GeometrySource::cpuid: return "cpuid";
        default: return "none";
    }
}

std::string format_bytes(std::uint64_t bytes) {
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;
    std::ostringstream out;
    out.setf(std::ios::fixed);
    if (bytes >= static_cast<std::uint64_t>(gib)) {
        out.precision(2);
        out << static_cast<double>(bytes) / gib << " GiB";
    } else if (bytes >= static_cast<std::uint64_t>(mib)) {
        out.precision(2);
        out << static_cast<double>(bytes) / mib << " MiB";
    } else if (bytes >= static_cast<std::uint64_t>(kib)) {
        out.precision(1);
        out << static_cast<double>(bytes) / kib << " KiB";
    } else {
        out << bytes << " B";
    }
    return out.str();
}

} // namespace cachescope
