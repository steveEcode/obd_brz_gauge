# Host tests for the pure-C parts of the theme engine (main/theme_engine):
# pixel conversion, streaming JSON splitter, editor overlay number path,
# the sidecar reducer / pairing, video owner handoff and theme.bin layout parser. Optional real-LVGL tests;
# the default suite needs neither an ESP-IDF build nor LVGL.
#
#   cmake -S test/host/theme -B /tmp/theme_host && cmake --build /tmp/theme_host
#   ctest --test-dir /tmp/theme_host --output-on-failure
#
# cJSON is compiled from the ESP-IDF tree (not vendored): set IDF_PATH or
# -DCJSON_DIR=... if it is not at ~/.espressif/v5.5.3/esp-idf.
cmake_minimum_required(VERSION 3.16)
project(theme_engine_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(TE_SRC_DIR "${REPO_ROOT}/main/theme_engine")
set(RULES_SRC_DIR "${TE_SRC_DIR}/rules")

if(DEFINED ENV{IDF_PATH})
    set(_idf_default "$ENV{IDF_PATH}")
else()
    set(_idf_default "$ENV{HOME}/.espressif/v5.5.3/esp-idf")
endif()
set(CJSON_DIR "${_idf_default}/components/json/cJSON" CACHE PATH "Directory with cJSON.c/cJSON.h")
if(EXISTS "${CJSON_DIR}/cJSON.c")
    set(_TE_CJSON_LOCAL ON)
else()
    message(STATUS "cJSON not found at ${CJSON_DIR}; fetching via FetchContent")
    set(_TE_CJSON_LOCAL OFF)
    include(FetchContent)
    FetchContent_Declare(cjson_fc
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        v1.7.18
        GIT_SHALLOW    TRUE)
    set(ENABLE_CJSON_TEST  OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson_fc)
endif()

option(TE_SANITIZE "Build with -fsanitize=address,undefined when supported" ON)
if(TE_SANITIZE)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" TE_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(TE_HAVE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

if(_TE_CJSON_LOCAL)
    add_library(te_cjson STATIC "${CJSON_DIR}/cJSON.c")
    target_include_directories(te_cjson PUBLIC "${CJSON_DIR}")
    target_compile_options(te_cjson PRIVATE -w)
else()
    add_library(te_cjson INTERFACE)
    target_link_libraries(te_cjson INTERFACE cjson)
endif()

# Host assertions are part of the test contract, including Release builds.
set(TE_WARN -Wall -Wextra -Werror -UNDEBUG)

add_library(theme_core STATIC
    "${TE_SRC_DIR}/theme_pixconv.c"
    "${TE_SRC_DIR}/theme_jsplit.c"
    "${TE_SRC_DIR}/theme_overlay.c"
    "${TE_SRC_DIR}/theme_sidecar_core.c"
    "${TE_SRC_DIR}/theme_layout.c"
    "${RULES_SRC_DIR}/tr_parse.c"
    "${RULES_SRC_DIR}/tr_eval.c"
    "${RULES_SRC_DIR}/tr_format.c")
target_include_directories(theme_core PUBLIC "${TE_SRC_DIR}" "${RULES_SRC_DIR}")
# JS never fuses multiply-add; keep doubles bit-exact with the editor.
target_compile_options(theme_core PRIVATE ${TE_WARN} -Wshadow -Wconversion -Wno-sign-conversion -ffp-contract=off)
target_link_libraries(theme_core PUBLIC te_cjson)
find_library(MATH_LIB m)
if(MATH_LIB)
    target_link_libraries(theme_core PUBLIC ${MATH_LIB})
endif()

foreach(t test_pixconv test_jsplit test_overlay test_sidecar test_layout)
    add_executable(${t} ${t}.c th_util.c)
    target_compile_options(${t} PRIVATE ${TE_WARN} -ffp-contract=off)
    target_link_libraries(${t} PRIVATE theme_core)
endforeach()

add_executable(test_plan test_plan.c plan_reference.c th_util.c)
target_compile_options(test_plan PRIVATE ${TE_WARN})
target_link_libraries(test_plan PRIVATE theme_core)

enable_testing()
add_test(NAME theme_plan COMMAND test_plan)
add_test(NAME theme_pixconv COMMAND test_pixconv)
add_test(NAME theme_jsplit COMMAND test_jsplit)
add_test(NAME theme_overlay
         COMMAND test_overlay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/overlay_vectors.json")
add_test(NAME theme_sidecar COMMAND test_sidecar)
add_test(NAME theme_layout COMMAND test_layout)

# Optional page integration: compile the real page source and LVGL with only
# platform/signal/video I/O shims. The portable core suite stays SDK/LVGL-free.
add_executable(test_media_migration test_media_migration.c th_util.c
    "${REPO_ROOT}/main/app_obd_dsp/theme_mount.c"
    "${REPO_ROOT}/main/app_obd_dsp/inbox_import.c")
target_include_directories(test_media_migration PRIVATE page_stubs "${REPO_ROOT}/main")
target_compile_options(test_media_migration PRIVATE ${TE_WARN})
target_link_libraries(test_media_migration PRIVATE theme_core)
add_test(NAME theme_media_migration COMMAND test_media_migration)

add_executable(test_boot_media_invalidate test_boot_media_invalidate.c th_util.c
    "${REPO_ROOT}/main/app_obd_dsp/boot_media_mount.c"
    "${REPO_ROOT}/main/app_obd_dsp/flash_writer.c")
target_include_directories(test_boot_media_invalidate PRIVATE page_stubs "${REPO_ROOT}/main")
target_compile_options(test_boot_media_invalidate PRIVATE ${TE_WARN})
target_link_libraries(test_boot_media_invalidate PRIVATE theme_core)
add_test(NAME theme_boot_media_invalidate COMMAND test_boot_media_invalidate)

option(TE_PAGE_LVGL "Build real-LVGL page integration test" OFF)
option(TE_PAGE_BENCHMARK "Build stable-page microbenchmark" OFF)
if(TE_PAGE_BENCHMARK AND NOT TE_PAGE_LVGL)
    message(FATAL_ERROR "TE_PAGE_BENCHMARK requires TE_PAGE_LVGL=ON")
endif()
if(TE_PAGE_LVGL)
    set(TE_LVGL_DIR "${REPO_ROOT}/managed_components/lvgl__lvgl" CACHE PATH "LVGL 8 sources")
    if(NOT EXISTS "${TE_LVGL_DIR}/lvgl.h")
        message(FATAL_ERROR "LVGL 8 source missing; set TE_LVGL_DIR")
    endif()
    file(GLOB_RECURSE TE_LVGL_SOURCES CONFIGURE_DEPENDS "${TE_LVGL_DIR}/src/*.c")
    add_library(te_lvgl STATIC ${TE_LVGL_SOURCES})
    target_include_directories(te_lvgl SYSTEM PUBLIC "${TE_LVGL_DIR}")
    target_compile_definitions(te_lvgl PUBLIC LV_CONF_SKIP LV_MEM_CUSTOM=1)
    add_library(theme_page_support STATIC "${TE_SRC_DIR}/theme_img_cache.c"
        "${REPO_ROOT}/main/app_obd_dsp/tpms_theme_source.c")
    target_include_directories(theme_page_support PUBLIC page_stubs "${REPO_ROOT}/main")
    target_link_libraries(theme_page_support PUBLIC theme_core te_lvgl)
    target_compile_options(theme_page_support PRIVATE ${TE_WARN})
    add_executable(test_page test_page.c th_util.c "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c")
    target_link_libraries(test_page PRIVATE theme_page_support)
    target_compile_options(test_page PRIVATE ${TE_WARN} -ffp-contract=off)
    add_test(NAME theme_page COMMAND test_page)
    add_executable(test_video_elem test_video_elem.c th_util.c "${REPO_ROOT}/main/media/video_elem.c")
    target_include_directories(test_video_elem BEFORE PRIVATE video_elem_stubs page_stubs "${REPO_ROOT}/main" "${CJSON_DIR}")
    target_link_libraries(test_video_elem PRIVATE te_lvgl te_cjson)
    target_compile_options(test_video_elem PRIVATE ${TE_WARN})
    add_test(NAME theme_video_elem COMMAND test_video_elem)
    add_executable(test_sd_theme test_sd_theme.c th_util.c)
    target_link_libraries(test_sd_theme PRIVATE theme_page_support)
    target_compile_options(test_sd_theme PRIVATE ${TE_WARN} -ffp-contract=off)
    add_test(NAME theme_sd_loader_library COMMAND test_sd_theme)
    # Real acquisition and cache source; allocator callbacks trace every request.
    add_executable(test_acquire test_acquire.c th_util.c
        "${TE_SRC_DIR}/theme_img_cache.c" "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c"
        "${REPO_ROOT}/main/app_obd_dsp/tpms_theme_source.c")
    target_include_directories(test_acquire PRIVATE page_stubs "${REPO_ROOT}/main")
    target_compile_definitions(test_acquire PRIVATE TE_ACQUIRE_TRACE)
    target_link_libraries(test_acquire PRIVATE theme_core te_lvgl)
    target_compile_options(test_acquire PRIVATE ${TE_WARN} -ffp-contract=off)
    add_test(NAME theme_acquire COMMAND test_acquire)
    add_executable(test_navigation test_navigation.c)
    set_property(TARGET test_navigation PROPERTY C_STANDARD 11)
    target_include_directories(test_navigation BEFORE PRIVATE
        navigation_stubs page_stubs "${REPO_ROOT}/main")
    target_link_libraries(test_navigation PRIVATE te_lvgl)
    target_compile_options(test_navigation PRIVATE ${TE_WARN})
    add_test(NAME theme_navigation COMMAND test_navigation)
    if(TE_PAGE_BENCHMARK)
        add_executable(bench_page bench_page.c th_util.c)
        target_compile_options(bench_page PRIVATE ${TE_WARN} -ffp-contract=off)
        target_link_libraries(bench_page PRIVATE theme_page_support)
    endif()
endif()

# Image planning timing uses a separate Release build without sanitizers.
option(TE_PLAN_BENCHMARK "Build round-image planner benchmark" OFF)
if(TE_PLAN_BENCHMARK)
    add_executable(bench_plan bench_plan.c plan_reference.c)
    target_compile_options(bench_plan PRIVATE ${TE_WARN})
    target_link_libraries(bench_plan PRIVATE theme_core)
endif()

# Channel rounding compares the real private conversion and complete images
# against the original floorf path. Temporary source overrides are host-only.
set(TE_RESAMPLE_SOURCE "" CACHE FILEPATH "Temporary pixel source for negative controls")
add_library(resample_probe STATIC resample_probe.c)
target_link_libraries(resample_probe PUBLIC theme_core)
target_include_directories(resample_probe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(resample_probe PRIVATE ${TE_WARN} -ffp-contract=off)
if(TE_RESAMPLE_SOURCE)
    target_compile_definitions(resample_probe PRIVATE "RS_SOURCE=\"${TE_RESAMPLE_SOURCE}\"")
endif()
add_executable(test_resample test_resample.c th_util.c)
target_link_libraries(test_resample PRIVATE resample_probe)
target_compile_options(test_resample PRIVATE ${TE_WARN} -ffp-contract=off)
add_test(NAME theme_resample COMMAND test_resample)
option(TE_RESAMPLE_BENCHMARK "Build paired complete-image resample benchmark" OFF)
if(TE_RESAMPLE_BENCHMARK)
    add_executable(bench_resample bench_resample.c)
    target_link_libraries(bench_resample PRIVATE resample_probe)
    target_compile_options(bench_resample PRIVATE ${TE_WARN} -ffp-contract=off)
endif()

# Real USB installer and sidecar reducer with a temporary POSIX filesystem.
# SDK, SD bounce I/O, NVS, and LVGL are substituted; directory swaps are real.
add_executable(test_theme_bundle test_theme_bundle.c
    "${REPO_ROOT}/main/app_obd_dsp/theme_bundle.c")
target_include_directories(test_theme_bundle BEFORE PRIVATE bundle_stubs
    "${REPO_ROOT}/test/host/storage/stubs" "${REPO_ROOT}/main")
target_compile_definitions(test_theme_bundle PRIVATE _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)
target_compile_options(test_theme_bundle PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(test_theme_bundle PRIVATE theme_core)
add_test(NAME theme_bundle COMMAND test_theme_bundle)
