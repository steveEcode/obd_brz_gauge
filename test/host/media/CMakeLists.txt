# Host tests for the pure-C media pieces (no ESP-IDF build):
#   main/media/obv1_parser.c   OBV1 container header / index validation
#   main/media/delta_varint.c  legacy boot animation streaming decoder
#
#   cmake -S test/host/media -B build-host/media && cmake --build build-host/media
#   ctest --test-dir build-host/media --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(media_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# ---- Optional sanitisers ----
option(MH_SANITIZE "Build with -fsanitize=address,undefined when supported" ON)
if(MH_SANITIZE)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" MH_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(MH_HAVE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                            -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

set(MH_WARN -Wall -Wextra -Werror -Wshadow)

add_library(media_core STATIC
    "${REPO_ROOT}/main/media/obv1_parser.c"
    "${REPO_ROOT}/main/media/delta_varint.c")
target_include_directories(media_core PUBLIC "${REPO_ROOT}/main/media")
target_compile_options(media_core PRIVATE ${MH_WARN})

add_executable(test_media "${CMAKE_CURRENT_SOURCE_DIR}/test_media.c")
target_compile_options(test_media PRIVATE ${MH_WARN})
target_link_libraries(test_media PRIVATE media_core)

enable_testing()
# The repo root enables the bootmedia/slot_a legacy stream round trip.
add_test(NAME media COMMAND test_media "${REPO_ROOT}")

add_executable(test_boot_obv_validate test_boot_obv_validate.c
    "${REPO_ROOT}/main/media/boot_obv_validate.c")
target_include_directories(test_boot_obv_validate PRIVATE player_stubs "${REPO_ROOT}/main")
target_compile_options(test_boot_obv_validate PRIVATE ${MH_WARN} -UNDEBUG)
target_link_libraries(test_boot_obv_validate PRIVATE media_core)
add_test(NAME media_boot_obv_validate COMMAND test_boot_obv_validate)

# CRC regressions cover both independent modules with the previous algorithms.
add_library(crc_reference STATIC crc_reference.c)
target_compile_options(crc_reference PRIVATE ${MH_WARN})
add_executable(test_crc test_crc.c "${REPO_ROOT}/main/vehicle/vp_common.c")
target_include_directories(test_crc PRIVATE "${REPO_ROOT}/main/vehicle")
target_compile_options(test_crc PRIVATE ${MH_WARN})
target_link_libraries(test_crc PRIVATE media_core crc_reference)
add_test(NAME media_crc_portable COMMAND test_crc)

# Exercise the actual ESP_PLATFORM branch against ESP-IDF's official Linux
# compatibility implementation. No SDK is needed for the ordinary host suite.
if(DEFINED ENV{IDF_PATH})
    set(_mh_idf_default "$ENV{IDF_PATH}")
else()
    set(_mh_idf_default "$ENV{HOME}/.espressif/v5.5.3/esp-idf")
endif()
set(MH_ESP_ROM_DIR "${_mh_idf_default}/components/esp_rom" CACHE PATH
    "ESP-IDF esp_rom source directory (optional host compatibility tests)")
option(MH_ROM_CRC "Test ESP_PLATFORM CRC wrappers with the SDK Linux backend" ON)
option(MH_BENCHMARK "Build CRC microbenchmark against the SDK Linux backend" OFF)
if(MH_ROM_CRC AND EXISTS "${MH_ESP_ROM_DIR}/linux/esp_rom_crc.c" AND
   EXISTS "${MH_ESP_ROM_DIR}/include/esp_rom_crc.h")
    add_library(media_rom_compat STATIC "${MH_ESP_ROM_DIR}/linux/esp_rom_crc.c")
    target_include_directories(media_rom_compat PUBLIC "${MH_ESP_ROM_DIR}/include")
    add_library(media_rom_core STATIC
        "${REPO_ROOT}/main/media/obv1_parser.c"
        "${REPO_ROOT}/main/media/delta_varint.c"
        "${REPO_ROOT}/main/vehicle/vp_common.c")
    target_include_directories(media_rom_core PUBLIC
        "${REPO_ROOT}/main/media" "${REPO_ROOT}/main/vehicle")
    target_compile_definitions(media_rom_core PRIVATE ESP_PLATFORM)
    target_compile_options(media_rom_core PRIVATE ${MH_WARN})
    target_link_libraries(media_rom_core PUBLIC media_rom_compat)
    add_executable(test_crc_rom test_crc.c)
    target_compile_options(test_crc_rom PRIVATE ${MH_WARN})
    target_link_libraries(test_crc_rom PRIVATE media_rom_core crc_reference)
    add_test(NAME media_crc_rom COMMAND test_crc_rom)
    add_executable(test_media_rom test_media.c)
    target_compile_options(test_media_rom PRIVATE ${MH_WARN})
    target_link_libraries(test_media_rom PRIVATE media_rom_core)
    add_test(NAME media_rom COMMAND test_media_rom "${REPO_ROOT}")
    if(MH_BENCHMARK)
        add_executable(bench_crc bench_crc.c)
        target_compile_options(bench_crc PRIVATE ${MH_WARN})
        target_link_libraries(bench_crc PRIVATE media_rom_core crc_reference)
    endif()
elseif(MH_BENCHMARK)
    message(FATAL_ERROR "MH_BENCHMARK needs MH_ROM_CRC=ON and MH_ESP_ROM_DIR with SDK Linux CRC sources")
else()
    message(STATUS "SDK Linux CRC compatibility tests skipped (disabled or SDK unavailable)")
endif()

# Test the production SPSC head reader with lock-boundary interleavings and
# a real concurrent producer; only FreeRTOS critical sections are substituted.
find_package(Threads REQUIRED)
add_executable(test_video_ring_peek test_video_ring_peek.c)
target_include_directories(test_video_ring_peek PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/media")
target_compile_definitions(test_video_ring_peek PRIVATE _DEFAULT_SOURCE)
target_compile_options(test_video_ring_peek PRIVATE ${MH_WARN})
target_link_libraries(test_video_ring_peek PRIVATE Threads::Threads)
add_test(NAME media_video_ring_peek COMMAND test_video_ring_peek)

# White-box execution of the real player/session/ring/wait control paths. Only
# SDK/RTOS/LVGL/LCD/JPEG boundaries are substituted; no hardware timing claims.
set(MH_PLAYER_SOURCE "${REPO_ROOT}/main/media/video_player.c" CACHE FILEPATH
    "Player implementation included by control tests (negative-control override)")
set(MH_RUNTIME_SOURCE "${REPO_ROOT}/main/app_obd_dsp/lvgl_runtime.c" CACHE FILEPATH
    "LVGL runtime implementation included by control tests (negative-control override)")
foreach(t test_video_player_wait test_video_player_stats)
    add_executable(${t} test_video_player_wait.c)
    target_include_directories(${t} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/player_stubs" "${REPO_ROOT}/main")
    target_compile_definitions(${t} PRIVATE
        _POSIX_C_SOURCE=200809L "VP_TEST_SOURCE=\"${MH_PLAYER_SOURCE}\""
        "VP_RUNTIME_SOURCE=\"${MH_RUNTIME_SOURCE}\"")
    target_compile_options(${t} PRIVATE ${MH_WARN})
    target_link_libraries(${t} PRIVATE media_core)
endforeach()
target_compile_definitions(test_video_player_stats PRIVATE CONFIG_OBD_PERF_STATS=1)
add_test(NAME media_video_player_wait COMMAND test_video_player_wait)
add_test(NAME media_video_player_stats COMMAND test_video_player_stats)
set_tests_properties(media_video_player_wait media_video_player_stats PROPERTIES TIMEOUT 30)

# Real LCD/LVGL completion bridge. A pthread-backed binary semaphore forces
# deterministic blocked/woken interleavings in normal and statistics builds.
find_package(Threads REQUIRED)
foreach(flush_stats 0 1)
    add_executable(test_lvgl_flush_sync_${flush_stats} test_lvgl_flush_sync.c
        "${REPO_ROOT}/main/app_obd_dsp/lvgl_flush_sync.c")
    target_include_directories(test_lvgl_flush_sync_${flush_stats} PRIVATE
        flush_stubs "${REPO_ROOT}/main")
    target_compile_definitions(test_lvgl_flush_sync_${flush_stats} PRIVATE
        CONFIG_OBD_PERF_STATS=${flush_stats})
    target_compile_options(test_lvgl_flush_sync_${flush_stats} PRIVATE ${MH_WARN} -UNDEBUG)
    target_link_libraries(test_lvgl_flush_sync_${flush_stats} PRIVATE Threads::Threads)
    add_test(NAME media_lvgl_flush_sync_${flush_stats} COMMAND test_lvgl_flush_sync_${flush_stats})
    set_tests_properties(media_lvgl_flush_sync_${flush_stats} PROPERTIES TIMEOUT 15)
endforeach()
