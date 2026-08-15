// Optional Dear ImGui front end (SDL3 + OpenGL3).
//
// The GUI is a thin shell around exactly the same measurement engine the CLI
// uses: it inspects hardware, starts a run on a worker thread, shows live
// progress, and exports the same reports.

#include <cachescope/benchmark.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <atomic>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct SharedState {
    std::mutex mutex;
    std::optional<cachescope::BenchmarkResult> result;
    std::optional<cachescope::ReportPaths> reports;
    std::string error;
    std::string stage;
    std::string detail;
    int completed = 0;
    int total = 0;
};

std::vector<float> values_from(const cachescope::SweepSeries* series) {
    std::vector<float> values;
    if (!series) return values;
    values.reserve(series->points.size());
    for (const auto& p : series->points) {
        values.push_back(static_cast<float>(series->y == cachescope::YAxis::gib_per_second
                                                ? p.gib_per_second.median
                                                : p.ns_per_access.median));
    }
    return values;
}

void cache_table(const cachescope::SystemInfo& system) {
    if (system.caches.empty()) {
        ImGui::TextDisabled("This operating system did not report cache geometry.");
        return;
    }
    constexpr auto flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("cache-table", 6, flags)) {
        ImGui::TableSetupColumn("Cache");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Line");
        ImGui::TableSetupColumn("Ways");
        ImGui::TableSetupColumn("Shared by");
        ImGui::TableSetupColumn("Instances");
        ImGui::TableHeadersRow();
        for (const auto& c : system.caches) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("L%d %s", c.level, cachescope::cache_type_name(c.type).c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cachescope::format_bytes(c.size_bytes).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u B", c.line_size_bytes);
            ImGui::TableSetColumnIndex(3);
            if (c.associativity_ways == 0xFFFFFFFFu) ImGui::TextUnformatted("full");
            else if (c.associativity_ways) ImGui::Text("%u", c.associativity_ways);
            else ImGui::TextUnformatted("-");
            ImGui::TableSetColumnIndex(4);
            if (c.sharing_threads) ImGui::Text("%u", c.sharing_threads);
            else ImGui::TextUnformatted("-");
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%u", c.instances);
        }
        ImGui::EndTable();
    }
}

void result_view(const cachescope::BenchmarkResult& result) {
    constexpr auto flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("summary-table", 7, flags)) {
        ImGui::TableSetupColumn("Region");
        ImGui::TableSetupColumn("Reported");
        ImGui::TableSetupColumn("Measured set");
        ImGui::TableSetupColumn("Latency ns");
        ImGui::TableSetupColumn("Read GiB/s");
        ImGui::TableSetupColumn("Write GiB/s");
        ImGui::TableSetupColumn("Copy GiB/s");
        ImGui::TableHeadersRow();
        for (const auto& s : result.level_summary) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(s.label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.reported_size_bytes
                                       ? cachescope::format_bytes(s.reported_size_bytes).c_str()
                                       : "-");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(cachescope::format_bytes(s.representative_working_set_bytes).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.2f", s.latency_ns);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", s.sequential_read_gib_s);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.2f", s.sequential_write_gib_s);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%.2f", s.copy_gib_s);
        }
        ImGui::EndTable();
    }

    const auto latency = values_from(result.find("dependent_latency"));
    const auto read = values_from(result.find("sequential_read"));
    if (!latency.empty()) {
        ImGui::PlotLines("Dependent latency (ns/access)", latency.data(), static_cast<int>(latency.size()),
                         0, nullptr, 0.0f, FLT_MAX, ImVec2(-1, 130));
    }
    if (!read.empty()) {
        ImGui::PlotLines("Sequential read (GiB/s)", read.data(), static_cast<int>(read.size()), 0, nullptr,
                         0.0f, FLT_MAX, ImVec2(-1, 130));
    }

    for (const auto& pair : result.coherency.pairs) {
        ImGui::BulletText("%s: same line %.2f ns, separate lines %.2f ns, ping-pong %.2f ns",
                          pair.label.c_str(), pair.shared_line_ns.median, pair.padded_line_ns.median,
                          pair.ping_pong_ns.median);
    }
    if (result.experimental.line_size_bytes) {
        ImGui::BulletText("Experimental line transition: %u B", result.experimental.line_size_bytes);
    }
    if (result.experimental.tlb_reach_pages) {
        ImGui::BulletText("Experimental TLB reach knee: %llu pages",
                          static_cast<unsigned long long>(result.experimental.tlb_reach_pages));
    }
}

} // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    const auto flags = static_cast<SDL_WindowFlags>(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                                   SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Window* window = SDL_CreateWindow("CacheScope", 1280, 860, flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    const auto system = cachescope::detect_system();
    const char* presets[] = {"quick", "standard", "deep"};
    int preset_index = 1;
    int cpu = -1;
    bool pin = true;
    bool probes = true;
    char label[128] = {};
    std::snprintf(label, sizeof(label), "%s", system.cpu_name.c_str());
    const std::filesystem::path output_dir = "results";

    SharedState state;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::thread worker;

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGui::Begin("CacheScope", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        ImGui::TextUnformatted(system.cpu_name.c_str());
        ImGui::TextDisabled("%s | %s | %d-bit | %u logical CPUs | %s", system.operating_system.c_str(),
                            system.architecture.c_str(), system.pointer_bits, system.logical_cpus,
                            system.compiler.c_str());
        ImGui::SeparatorText("Hardware-reported cache geometry");
        cache_table(system);

        ImGui::SeparatorText("Experiment");
        ImGui::TextWrapped(
            "Close heavy applications and use a stable power profile before measuring. The benchmark runs "
            "on a worker thread, so this window stays responsive.");

        ImGui::BeginDisabled(running.load());
        ImGui::InputText("Label", label, sizeof(label));
        ImGui::Combo("Preset", &preset_index, presets, 3);
        ImGui::InputInt("Preferred logical CPU (-1 = auto)", &cpu);
        ImGui::Checkbox("Pin benchmark thread", &pin);
        ImGui::SameLine();
        ImGui::Checkbox("Run extra probes (line, stride, conflict, TLB, coherency)", &probes);
        const bool start = ImGui::Button("Run and export report", ImVec2(220, 0));
        ImGui::EndDisabled();

        if (start) {
            if (worker.joinable()) worker.join();
            running.store(true);
            cancel.store(false);
            {
                std::lock_guard lock(state.mutex);
                state.error.clear();
                state.stage = "starting";
                state.detail.clear();
                state.completed = 0;
                state.total = 0;
            }
            const std::string chosen_preset = presets[preset_index];
            const std::string chosen_label = label;
            worker = std::thread([&state, &running, &cancel, chosen_preset, chosen_label, cpu, pin, probes,
                                  output_dir]() {
                try {
                    auto cfg = cachescope::preset_config(chosen_preset);
                    cfg.label = chosen_label;
                    cfg.preferred_cpu = cpu;
                    cfg.pin_thread = pin;
                    if (!probes) {
                        cfg.include_line_probe = false;
                        cfg.include_stride_probe = false;
                        cfg.include_associativity_probe = false;
                        cfg.include_tlb_probe = false;
                        cfg.include_coherency_probe = false;
                    }
                    auto result = cachescope::run_benchmark(cfg, [&](const cachescope::Progress& p) {
                        std::lock_guard lock(state.mutex);
                        state.stage = p.stage;
                        state.detail = p.detail;
                        state.completed = p.completed;
                        state.total = p.total;
                        return !cancel.load();
                    });
                    auto reports = cachescope::write_reports(result, output_dir);
                    std::lock_guard lock(state.mutex);
                    state.result = std::move(result);
                    state.reports = std::move(reports);
                } catch (const std::exception& e) {
                    std::lock_guard lock(state.mutex);
                    state.error = e.what();
                }
                running.store(false);
            });
        }

        if (running.load()) {
            std::lock_guard lock(state.mutex);
            const float fraction =
                state.total > 0 ? static_cast<float>(state.completed) / static_cast<float>(state.total) : 0.0f;
            ImGui::ProgressBar(fraction, ImVec2(-1, 0));
            ImGui::Text("%s%s%s", state.stage.c_str(), state.detail.empty() ? "" : " - ",
                        state.detail.c_str());
            if (ImGui::Button("Cancel")) cancel.store(true);
        }

        {
            std::lock_guard lock(state.mutex);
            if (!state.error.empty()) ImGui::TextWrapped("Error: %s", state.error.c_str());
            if (state.result) {
                ImGui::SeparatorText("Latest result");
                result_view(*state.result);
                if (state.reports) {
                    for (const auto* path :
                         {&state.reports->html, &state.reports->csv, &state.reports->json,
                          &state.reports->markdown}) {
                        if (path->empty()) continue;
                        ImGui::TextWrapped("%s", std::filesystem::absolute(*path).string().c_str());
                    }
                }
                for (const auto& warning : state.result->warnings) {
                    ImGui::TextWrapped("warning: %s", warning.c_str());
                }
            }
        }

        ImGui::End();
        ImGui::Render();

        int display_w = 0, display_h = 0;
        SDL_GetWindowSizeInPixels(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    cancel.store(true);
    if (worker.joinable()) worker.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
