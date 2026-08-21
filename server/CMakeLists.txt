cmake_minimum_required(VERSION 3.21)

# vcpkg manifest mode: pick up the toolchain automatically when present.
# Set VCPKG_ROOT in your environment (CI does this for you).
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

# Read version from vcpkg.json to keep build version in sync with package manifest
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" VCPKG_JSON_CONTENT)
string(JSON LIVEPLAY_VERSION GET "${VCPKG_JSON_CONTENT}" "version-string")

project(liveplay-server
    VERSION ${LIVEPLAY_VERSION}
    DESCRIPTION "LivePlay headless audio engine and control server"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd / IDE integration.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Compile options
# ---------------------------------------------------------------------------
if(MSVC)
    # /bigobj — Crow's heavy template instantiation pushes control_server.cpp
    # past the default COFF section limit.
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /bigobj)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00   # Target Windows 10+
    )
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# macOS: std::format vs. deployment target
# ---------------------------------------------------------------------------
# Instantiating std::format for *any* argument pulls in libc++'s floating-point
# formatter, which calls std::to_chars(float/double). Apple marks that overload
# "unavailable" below macOS 13.3, so the build fails even for string-only calls
# (e.g. Logger::error("Reason : {}", reason)) whenever the deployment target is
# older than 13.3 — which it is for our 11.0 (x64) / 12.0 (arm64) artifacts.
#
# The implementation is header-only in modern libc++ (no dylib symbol), so it
# runs correctly on those older systems; only the availability annotation is
# conservative. Disable the annotations so we can keep the older targets.
if(APPLE)
    add_compile_definitions(_LIBCPP_DISABLE_AVAILABILITY)
endif()

# ---------------------------------------------------------------------------
# Dependencies (resolved via vcpkg manifest — see vcpkg.json)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
# TagLib's exported targets reference ZLIB::ZLIB in their link interface.
# We must resolve it here so the target exists when TagLib is consumed —
# otherwise CMake errors out on macOS / Linux with:
#   "the link interface of target 'taglib::tag' contains: ZLIB::ZLIB but
#    the target was not found."
find_package(ZLIB REQUIRED)
find_package(unofficial-taglib CONFIG QUIET)
if(NOT unofficial-taglib_FOUND)
    find_package(TagLib CONFIG REQUIRED)
endif()
# vcpkg's miniaudio is header-only; the implementation TU is compiled below.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
# miniz — used for .lpa (zip) packaging of project exports/imports.
find_package(miniz CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# miniaudio implementation translation unit
# Header-only libs need exactly one TU that defines MINIAUDIO_IMPLEMENTATION.
# ---------------------------------------------------------------------------
add_library(liveplay_miniaudio STATIC src/audio/miniaudio_impl.c)
target_include_directories(liveplay_miniaudio PUBLIC ${MINIAUDIO_INCLUDE_DIR})
# Suppress narrowing warnings inside the miniaudio header (third-party, not our code).
if(MSVC)
    target_compile_options(liveplay_miniaudio PRIVATE /wd4244)
endif()

if(APPLE)
    target_link_libraries(liveplay_miniaudio
        PUBLIC
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework AudioUnit"
            "-framework CoreFoundation"
    )
elseif(UNIX)
    find_package(ALSA)
    if(ALSA_FOUND)
        target_link_libraries(liveplay_miniaudio PUBLIC ALSA::ALSA)
    endif()
    target_link_libraries(liveplay_miniaudio PUBLIC ${CMAKE_DL_LIBS} Threads::Threads m)
elseif(WIN32)
    target_link_libraries(liveplay_miniaudio PUBLIC ole32 winmm)
endif()

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
if(WIN32)
    # Embed the app icon and version info from the Windows resource script.
    # The .rc file uses a path relative to its own location, so set the
    # RC_FLAGS include directory to assets/ so the compiler finds app.ico.
    set(LIVEPLAY_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets/server.rc")
    set(CMAKE_RC_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")
else()
    set(LIVEPLAY_RC "")
endif()

add_executable(liveplay-server
    src/main.cpp
    src/logger.cpp
    src/crash_handler.cpp
    ${LIVEPLAY_RC}

    # Milestone 2 — audio engine
    src/audio/meter.cpp
    src/audio/limiter.cpp
    src/audio/ltc_generator.cpp
    src/audio/mixer_channel.cpp
    src/audio/playback_item.cpp
    src/audio/engine.cpp

    # Milestone 3 — project state + control plane
    src/core/project_state.cpp
    src/core/output_map.cpp
    src/core/backup_manager.cpp
    src/net/control_server.cpp
    src/net/discovery.cpp

    # Milestone 4 — metadata + waveform
    src/meta/metadata.cpp
    src/meta/waveform.cpp
)

target_include_directories(liveplay-server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(liveplay-server
    PRIVATE
        Threads::Threads
        nlohmann_json::nlohmann_json
        Crow::Crow
        liveplay_miniaudio
        miniz::miniz
)

# Link TagLib under whichever target name vcpkg exposed.
if(TARGET TagLib::tag)
    target_link_libraries(liveplay-server PRIVATE TagLib::tag)
elseif(TARGET TagLib::TagLib)
    target_link_libraries(liveplay-server PRIVATE TagLib::TagLib)
elseif(TARGET unofficial::taglib::tag)
    target_link_libraries(liveplay-server PRIVATE unofficial::taglib::tag)
endif()

if(WIN32)
    target_link_libraries(liveplay-server PRIVATE ws2_32 mswsock dbghelp mpr)
    # Modern Windows console: opt in to UTF-8 + VT for the banner.
    target_compile_definitions(liveplay-server PRIVATE UNICODE _UNICODE)
endif()

# Bake version + build identity into the binary.
target_compile_definitions(liveplay-server PRIVATE
    LIVEPLAY_SERVER_VERSION="${PROJECT_VERSION}"
    LIVEPLAY_SERVER_NAME="${PROJECT_NAME}"
)

# ---------------------------------------------------------------------------
# Tests (opt-in; framework-free assertion binaries)
# ---------------------------------------------------------------------------
option(LIVEPLAY_BUILD_TESTS "Build the liveplay test binaries" OFF)
if(LIVEPLAY_BUILD_TESTS)
    enable_testing()
    add_executable(liveplay-meter-tests
        tests/meter_test.cpp
        src/audio/meter.cpp
    )
    target_include_directories(liveplay-meter-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME meter COMMAND liveplay-meter-tests)

    # Mixer strip gain / fade envelope (peek vs. advance).
    # Also covers the PFL monitor taps, which is why limiter.cpp is here: it
    # carries db_to_linear_precise, and the tap gain has to be the same number
    # the engine uses, not a re-derivation of it.
    add_executable(liveplay-mixer-tests
        tests/mixer_channel_test.cpp
        src/audio/mixer_channel.cpp
        src/audio/meter.cpp
        src/audio/limiter.cpp
    )
    target_include_directories(liveplay-mixer-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME mixer COMMAND liveplay-mixer-tests)

    # Filter sections behind the HPF, the LPF and every EQ band. Header-only,
    # so the test needs nothing linked with it.
    add_executable(liveplay-biquad-tests
        tests/biquad_test.cpp
    )
    target_include_directories(liveplay-biquad-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME biquad COMMAND liveplay-biquad-tests)

    # Expander / gate and compressor / limiter. Header-only too.
    add_executable(liveplay-dynamics-tests
        tests/dynamics_test.cpp
    )
    target_include_directories(liveplay-dynamics-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME dynamics COMMAND liveplay-dynamics-tests)

    # Stereo image: the M/S width matrix, bass-mono, the balance law and the
    # correlation meter. Header-only.
    add_executable(liveplay-stereo-tests
        tests/stereo_width_test.cpp
    )
    target_include_directories(liveplay-stereo-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME stereo COMMAND liveplay-stereo-tests)

    # Clock-drift compensation: the resampler and its control loop. Header-only,
    # and the controller is driven by a simulated device rather than by audio —
    # a drift bug that takes twenty minutes to hear runs here in milliseconds.
    add_executable(liveplay-drift-tests
        tests/drift_test.cpp
    )
    target_include_directories(liveplay-drift-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME drift COMMAND liveplay-drift-tests)

    # Bus→bus processing order: Kahn's sort over strip→strip edges, cycle
    # edges dropped deterministically, Monitor excluded. The sort itself is a
    # header-only free function (compute_strip_order in engine.hpp) precisely
    # so it runs here without an audio device — but engine.hpp includes
    # miniaudio's header for the device types, hence the include-path link.
    add_executable(liveplay-topo-tests
        tests/topo_order_test.cpp
    )
    target_include_directories(liveplay-topo-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(liveplay-topo-tests
        PRIVATE liveplay_miniaudio Threads::Threads
    )
    add_test(NAME topo COMMAND liveplay-topo-tests)

    # Waveform downsampler (single-pass decode, per-channel traces).
    add_executable(liveplay-waveform-tests
        tests/waveform_test.cpp
        src/meta/waveform.cpp
        src/logger.cpp
    )
    target_include_directories(liveplay-waveform-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(liveplay-waveform-tests
        PRIVATE liveplay_miniaudio Threads::Threads
    )
    add_test(NAME waveform COMMAND liveplay-waveform-tests)
endif()

# ---------------------------------------------------------------------------
# Install rules — used by CI when staging the artifact directory.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS liveplay-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
