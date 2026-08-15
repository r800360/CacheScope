cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# Options must be declared before project(), because the vcpkg toolchain reads
# VCPKG_MANIFEST_FEATURES while project() runs.
# ---------------------------------------------------------------------------
option(CACHESCOPE_BUILD_GUI "Build the Dear ImGui + SDL3 GUI (requires vcpkg)" OFF)
option(CACHESCOPE_BUILD_TESTS "Build CacheScope tests" ON)
option(CACHESCOPE_WERROR "Treat compiler warnings as errors" OFF)

if(CACHESCOPE_BUILD_GUI)
    set(VCPKG_MANIFEST_FEATURES "gui" CACHE STRING "vcpkg manifest features" FORCE)

    # Convenience: pick up a vcpkg checkout from the environment so that
    # -DCACHESCOPE_BUILD_GUI=ON works without also spelling out the toolchain.
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
        set(_cachescope_vcpkg "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        if(EXISTS "${_cachescope_vcpkg}")
            set(CMAKE_TOOLCHAIN_FILE "${_cachescope_vcpkg}" CACHE FILEPATH "vcpkg toolchain")
            message(STATUS "CacheScope: using vcpkg toolchain from VCPKG_ROOT")
        endif()
        unset(_cachescope_vcpkg)
    endif()
else()
    set(VCPKG_MANIFEST_FEATURES "" CACHE STRING "vcpkg manifest features" FORCE)
endif()

project(CacheScope
    VERSION 1.1.0
    DESCRIPTION "Portable CPU cache characterization suite"
    LANGUAGES CXX)

# Benchmark numbers from an unoptimized build are meaningless, so default to
# Release for single-configuration generators rather than to no build type.
get_property(_cachescope_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _cachescope_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "CacheScope: no CMAKE_BUILD_TYPE given, defaulting to Release")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)

# Prefer C++23, but fall back to C++20 so the suite still builds on older
# toolchains. Nothing in the measurement engine needs a C++23-only feature.
if("cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    set(CACHESCOPE_CXX_FEATURE cxx_std_23)
elseif("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    set(CACHESCOPE_CXX_FEATURE cxx_std_20)
    message(STATUS "CacheScope: compiler does not advertise C++23, falling back to C++20")
else()
    message(FATAL_ERROR
        "CacheScope needs a C++20 or newer compiler. Detected ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}. Known-good: GCC 11+, Clang 14+, MSVC 19.30+ (VS 2022).")
endif()

# One predictable location for every executable, on single- and multi-config
# generators alike, so docs, scripts and CI can all use build/bin/<name>.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${_config}" _config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/bin")
endforeach()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core measurement library (no third-party dependencies)
# ---------------------------------------------------------------------------
add_library(cachescope_core STATIC
    src/benchmark.cpp
    src/compare.cpp
    src/platform.cpp
    src/report.cpp
)
add_library(CacheScope::core ALIAS cachescope_core)

target_compile_features(cachescope_core PUBLIC ${CACHESCOPE_CXX_FEATURE})
target_include_directories(cachescope_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(cachescope_core PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(cachescope_core PRIVATE Advapi32)
    # PowrProf exposes the *current* core frequency, which lets the report show
    # clock drift across a run. MSVC resolves SDK import libraries by name; for
    # other Windows toolchains look the library up before relying on it.
    if(MSVC)
        target_link_libraries(cachescope_core PRIVATE PowrProf)
        target_compile_definitions(cachescope_core PRIVATE CACHESCOPE_HAVE_POWRPROF=1)
    else()
        find_library(CACHESCOPE_POWRPROF_LIBRARY NAMES powrprof PowrProf)
        if(CACHESCOPE_POWRPROF_LIBRARY)
            target_link_libraries(cachescope_core PRIVATE ${CACHESCOPE_POWRPROF_LIBRARY})
            target_compile_definitions(cachescope_core PRIVATE CACHESCOPE_HAVE_POWRPROF=1)
        else()
            message(STATUS "CacheScope: PowrProf not found, falling back to the registry for core MHz")
        endif()
    endif()
endif()

if(MSVC)
    # C4324 fires on every deliberately cache-line-aligned struct, which is the
    # entire point of the false-sharing probe.
    target_compile_options(cachescope_core PRIVATE /W4 /permissive- /EHsc /wd4324)
    if(CACHESCOPE_WERROR)
        target_compile_options(cachescope_core PRIVATE /WX)
    endif()
else()
    target_compile_options(cachescope_core PRIVATE -Wall -Wextra -Wpedantic)
    if(CACHESCOPE_WERROR)
        target_compile_options(cachescope_core PRIVATE -Werror)
    endif()
endif()

# Interprocedural optimization is deliberately left off: the timing harness
# depends on kernels staying separately compiled and not inlined into the
# measurement loop.
set_target_properties(cachescope_core PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(cachescope_cli src/main.cpp)
target_link_libraries(cachescope_cli PRIVATE CacheScope::core)

add_executable(cachescope_compare src/compare_main.cpp)
target_link_libraries(cachescope_compare PRIVATE CacheScope::core)

if(CACHESCOPE_BUILD_GUI)
    find_package(imgui CONFIG REQUIRED)
    find_package(SDL3 CONFIG REQUIRED)
    set(OpenGL_GL_PREFERENCE GLVND)
    find_package(OpenGL REQUIRED)

    add_executable(cachescope_gui src/gui_main.cpp)
    target_link_libraries(cachescope_gui PRIVATE
        CacheScope::core
        imgui::imgui
        SDL3::SDL3
        OpenGL::GL
    )
    if(WIN32)
        # Keep the console attached: the GUI still prints fatal init errors.
        set_target_properties(cachescope_gui PROPERTIES WIN32_EXECUTABLE OFF)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CACHESCOPE_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(cachescope_tests tests/test_main.cpp)
        target_link_libraries(cachescope_tests PRIVATE CacheScope::core)
        add_test(NAME cachescope_tests COMMAND cachescope_tests)
        set_tests_properties(cachescope_tests PROPERTIES TIMEOUT 600)

        # End-to-end check that the shipped CLI actually produces a report.
        add_test(NAME cachescope_cli_smoke
            COMMAND cachescope_cli --preset quick --max-mib 2 --samples 3 --sample-ms 0.5
                    --only-sweeps --no-copy --quiet --formats all
                    --label "ctest smoke" --out "${CMAKE_CURRENT_BINARY_DIR}/ctest-results")
        set_tests_properties(cachescope_cli_smoke PROPERTIES TIMEOUT 600)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS cachescope_cli cachescope_compare RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(CACHESCOPE_BUILD_GUI)
    install(TARGETS cachescope_gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)

set(CPACK_PACKAGE_NAME CacheScope)
set(CPACK_PACKAGE_VENDOR CacheScope)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)

message(STATUS "CacheScope ${PROJECT_VERSION}: standard=${CACHESCOPE_CXX_FEATURE} gui=${CACHESCOPE_BUILD_GUI} tests=${CACHESCOPE_BUILD_TESTS}")
