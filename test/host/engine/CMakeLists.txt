# Host tests for the vehicle profile engine (main/vehicle/vp_engine.c), no ESP-IDF build.
#
#   cmake -S test/host/engine -B build-host/engine && cmake --build build-host/engine
#   ctest --test-dir build-host/engine --output-on-failure
#
# Uses the vehicle core sources and the shared test helpers of test/host/vehicle.
# cJSON is taken from the ESP-IDF tree when available (set IDF_PATH or -DCJSON_DIR=...),
# otherwise fetched automatically via FetchContent (requires network on first configure).
cmake_minimum_required(VERSION 3.16)
project(engine_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(VEHICLE_SRC_DIR "${REPO_ROOT}/main/vehicle")
set(VEHICLE_TEST_DIR "${REPO_ROOT}/test/host/vehicle")

# ---- cJSON: prefer IDF tree, fall back to FetchContent ----
if(DEFINED ENV{IDF_PATH})
    set(_idf_default "$ENV{IDF_PATH}")
else()
    set(_idf_default "$ENV{HOME}/.espressif/v5.5.3/esp-idf")
endif()
set(CJSON_DIR "${_idf_default}/components/json/cJSON" CACHE PATH "Directory with cJSON.c/cJSON.h")

if(EXISTS "${CJSON_DIR}/cJSON.c")
    add_library(eng_cjson STATIC "${CJSON_DIR}/cJSON.c")
    target_include_directories(eng_cjson PUBLIC "${CJSON_DIR}")
    target_compile_options(eng_cjson PRIVATE -w)
else()
    message(STATUS "cJSON not found at ${CJSON_DIR}; fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(cjson_fc
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        v1.7.18
        GIT_SHALLOW    TRUE)
    set(ENABLE_CJSON_TEST  OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson_fc)
    add_library(eng_cjson INTERFACE)
    target_link_libraries(eng_cjson INTERFACE cjson)
endif()

# ---- Optional sanitisers ----
option(EH_SANITIZE "Build with -fsanitize=address,undefined when supported" ON)
if(EH_SANITIZE)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" EH_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(EH_HAVE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                            -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

set(EH_WARN -Wall -Wextra -Werror -Wshadow)

# ---- Vehicle core + engine (pure C99) ----
add_library(engine_core STATIC
    "${VEHICLE_SRC_DIR}/vp_common.c"
    "${VEHICLE_SRC_DIR}/vp_compile.c"
    "${VEHICLE_SRC_DIR}/vp_defaults.c"
    "${VEHICLE_SRC_DIR}/vp_builtin_generic.c"
    "${VEHICLE_SRC_DIR}/vp_rpn.c"
    "${VEHICLE_SRC_DIR}/vp_rpn_json.c"
    "${VEHICLE_SRC_DIR}/vp_validate.c"
    "${VEHICLE_SRC_DIR}/vp_engine.c"
    "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c")
target_include_directories(engine_core PUBLIC "${VEHICLE_SRC_DIR}" "${REPO_ROOT}/main")
target_compile_options(engine_core PRIVATE ${EH_WARN})
target_link_libraries(engine_core PUBLIC eng_cjson)
find_library(MATH_LIB m)
if(MATH_LIB)
    target_link_libraries(engine_core PUBLIC ${MATH_LIB})
endif()

add_library(engine_vt STATIC "${VEHICLE_TEST_DIR}/vt_support.c")
target_include_directories(engine_vt PUBLIC "${VEHICLE_TEST_DIR}")
target_compile_options(engine_vt PRIVATE ${EH_WARN})
target_link_libraries(engine_vt PUBLIC engine_core eng_cjson)

add_executable(test_engine "${CMAKE_CURRENT_SOURCE_DIR}/test_engine.c")
target_compile_options(test_engine PRIVATE ${EH_WARN})
target_compile_definitions(test_engine PRIVATE
    "ET_CATALOG_DIR=\"${REPO_ROOT}/vehicles/catalog\""
    "ET_GOLDEN_DIR=\"${VEHICLE_TEST_DIR}/fixtures/golden\"")
target_link_libraries(test_engine PRIVATE engine_vt)

enable_testing()
add_test(NAME engine COMMAND test_engine)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ELM_WORKER_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/elm_worker.inc")
add_custom_command(OUTPUT "${ELM_WORKER_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_elm_worker.py"
            "${REPO_ROOT}/main/bsp_obd_dsp/elm327_ble_client.c" "${ELM_WORKER_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/bsp_obd_dsp/elm327_ble_client.c" extract_elm_worker.py
    VERBATIM)
add_executable(test_elm_wakeup test_elm_wakeup.c "${ELM_WORKER_EXCERPT}")
target_include_directories(test_elm_wakeup PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(test_elm_wakeup PRIVATE ${EH_WARN} -UNDEBUG)
add_test(NAME elm_wakeup COMMAND test_elm_wakeup)

set(ELM_NOTIFY_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/elm_notify.inc")
add_custom_command(OUTPUT "${ELM_NOTIFY_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_elm_notify.py"
            "${REPO_ROOT}/main/bsp_obd_dsp/elm327_ble_client.c" "${ELM_NOTIFY_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/bsp_obd_dsp/elm327_ble_client.c" extract_elm_notify.py
    VERBATIM)
add_executable(test_elm_notify test_elm_notify.c "${ELM_NOTIFY_EXCERPT}")
target_include_directories(test_elm_notify PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(test_elm_notify PRIVATE ${EH_WARN} -UNDEBUG)
add_test(NAME elm_notify COMMAND test_elm_notify)

# Selection-only timing: opt in with a separate Release build, sanitizers off.
option(EH_CAN_BENCHMARK "Build CAN rule-selection microbenchmark" OFF)
if(EH_CAN_BENCHMARK)
    add_executable(bench_can_index bench_can_index.c can_index_selection.c)
    target_compile_options(bench_can_index PRIVATE ${EH_WARN})
    target_link_libraries(bench_can_index PRIVATE engine_core)
endif()

option(EH_REQUEST_BENCHMARK "Build request dispatch-index microbenchmark" OFF)
if(EH_REQUEST_BENCHMARK)
    add_executable(bench_request_index bench_request_index.c request_index_selection.c)
    target_compile_options(bench_request_index PRIVATE ${EH_WARN})
endif()

# Private planner replay compiles production source directly, plus an independent
# pre-mask reference. No hooks or benchmark code are linked into the firmware.
add_library(poll_plan_probe STATIC poll_plan_probe.c)
target_compile_options(poll_plan_probe PRIVATE ${EH_WARN})
target_link_libraries(poll_plan_probe PUBLIC engine_core)
add_executable(test_poll_plan test_poll_plan.c)
target_compile_options(test_poll_plan PRIVATE ${EH_WARN})
target_link_libraries(test_poll_plan PRIVATE poll_plan_probe)
add_test(NAME engine_poll_plan COMMAND test_poll_plan)

option(EH_POLL_BENCHMARK "Build request planning deduplication benchmark" OFF)
if(EH_POLL_BENCHMARK)
    add_executable(bench_poll_plan bench_poll_plan.c)
    target_compile_options(bench_poll_plan PRIVATE ${EH_WARN})
    target_link_libraries(bench_poll_plan PRIVATE poll_plan_probe)
endif()

# Real private queue replay against independent snprintf-based reference.
add_library(queue_copy_probe STATIC queue_copy_probe.c)
target_compile_options(queue_copy_probe PRIVATE ${EH_WARN})
target_link_libraries(queue_copy_probe PUBLIC engine_core)
set(EH_QUEUE_SOURCE "" CACHE FILEPATH "Host-only engine override for queue/state baselines and negative controls")
if(EH_QUEUE_SOURCE)
    target_compile_definitions(queue_copy_probe PRIVATE "QC_ENGINE_SOURCE=\"${EH_QUEUE_SOURCE}\"")
endif()
add_executable(test_queue_copy test_queue_copy.c)
target_compile_options(test_queue_copy PRIVATE ${EH_WARN})
target_link_libraries(test_queue_copy PRIVATE queue_copy_probe)
add_test(NAME engine_queue_copy COMMAND test_queue_copy)

option(EH_QUEUE_BENCHMARK "Build command queue copy microbenchmark" OFF)
if(EH_QUEUE_BENCHMARK)
    add_executable(bench_queue_copy bench_queue_copy.c)
    target_compile_options(bench_queue_copy PRIVATE ${EH_WARN})
    target_link_libraries(bench_queue_copy PRIVATE queue_copy_probe)
endif()

# Fixed-prefix state commands share the real-engine/private queue probe.
add_executable(test_state_format test_state_format.c)
target_compile_options(test_state_format PRIVATE ${EH_WARN})
target_link_libraries(test_state_format PRIVATE queue_copy_probe)
add_test(NAME engine_state_format COMMAND test_state_format)

# Optional whole-source baseline in a separate translation unit. Symbol aliases
# let both actual implementations run in one process, on the same probe state.
# The saved source must have the same engine/probe layout as production.
set(EH_STATE_BASELINE_SOURCE "" CACHE FILEPATH "Saved pre-formatting engine source for paired host timing")
option(EH_STATE_BENCHMARK "Build adapter state-command formatting microbenchmark" OFF)
if(EH_STATE_BENCHMARK)
    add_executable(bench_state_format bench_state_format.c)
    target_compile_options(bench_state_format PRIVATE ${EH_WARN})
    target_link_libraries(bench_state_format PRIVATE queue_copy_probe)
    if(EH_STATE_BASELINE_SOURCE)
        add_library(state_baseline_probe STATIC queue_copy_probe.c)
        target_compile_options(state_baseline_probe PRIVATE ${EH_WARN})
        target_link_libraries(state_baseline_probe PUBLIC engine_core)
        target_compile_definitions(state_baseline_probe PRIVATE
            "QC_ENGINE_SOURCE=\"${EH_STATE_BASELINE_SOURCE}\"")
        foreach(fn create destroy link_up link_down on_rx run get_stats gear_from_ratio)
            target_compile_definitions(state_baseline_probe PRIVATE
                "vp_engine_${fn}=baseline_vp_engine_${fn}")
        endforeach()
        foreach(fn create destroy push pop count format copy state state_format state_copy)
            target_compile_definitions(state_baseline_probe PRIVATE "qc_${fn}=qc_baseline_${fn}")
        endforeach()
        target_compile_definitions(bench_state_format PRIVATE EH_STATE_BASELINE)
        target_link_libraries(bench_state_format PRIVATE state_baseline_probe)
    endif()
endif()

# RX replay compares whole private state and real monitor callback traces.
add_library(rx_append_probe STATIC rx_append_probe.c)
target_compile_options(rx_append_probe PRIVATE ${EH_WARN})
target_link_libraries(rx_append_probe PUBLIC engine_core)
set(EH_RX_SOURCE "" CACHE FILEPATH "Host-only RX source override for experiments/negative controls")
if(EH_RX_SOURCE)
    target_compile_definitions(rx_append_probe PRIVATE "RA_ENGINE_SOURCE=\"${EH_RX_SOURCE}\"")
endif()
add_executable(test_rx_append test_rx_append.c)
target_compile_options(test_rx_append PRIVATE ${EH_WARN})
target_link_libraries(test_rx_append PRIVATE rx_append_probe)
add_test(NAME engine_rx_append COMMAND test_rx_append)

option(EH_RX_BENCHMARK "Build paired receive append microbenchmark" OFF)
set(EH_RX_BASELINE_SOURCE "" CACHE FILEPATH "Saved pre-append whole engine source for paired host timing")
if(EH_RX_BENCHMARK)
    if(NOT EH_RX_BASELINE_SOURCE)
        message(FATAL_ERROR "EH_RX_BENCHMARK requires EH_RX_BASELINE_SOURCE")
    endif()
    add_library(rx_baseline_probe STATIC rx_append_probe.c)
    target_compile_options(rx_baseline_probe PRIVATE ${EH_WARN})
    target_link_libraries(rx_baseline_probe PUBLIC engine_core)
    target_compile_definitions(rx_baseline_probe PRIVATE
        "RA_ENGINE_SOURCE=\"${EH_RX_BASELINE_SOURCE}\"")
    foreach(fn create destroy link_up link_down on_rx run get_stats gear_from_ratio)
        target_compile_definitions(rx_baseline_probe PRIVATE "vp_engine_${fn}=rx_baseline_vp_engine_${fn}")
    endforeach()
    foreach(fn create destroy reset compare observe feed engine_size)
        target_compile_definitions(rx_baseline_probe PRIVATE "ra_${fn}=ra_baseline_${fn}")
    endforeach()
    add_executable(bench_rx_append bench_rx_append.c)
    target_compile_options(bench_rx_append PRIVATE ${EH_WARN})
    target_link_libraries(bench_rx_append PRIVATE rx_append_probe rx_baseline_probe)
endif()

# Real sample writeback and full Binary32 identity-rounding oracle.
add_library(write_rounding_probe STATIC write_rounding_probe.c)
target_compile_options(write_rounding_probe PRIVATE ${EH_WARN})
target_link_libraries(write_rounding_probe PUBLIC engine_core)
set(EH_WRITE_SOURCE "" CACHE FILEPATH "Host-only sample-write source override for negative controls")
if(EH_WRITE_SOURCE)
    target_compile_definitions(write_rounding_probe PRIVATE "WR_ENGINE_SOURCE=\"${EH_WRITE_SOURCE}\"")
endif()
add_executable(test_write_rounding test_write_rounding.c)
target_compile_options(test_write_rounding PRIVATE ${EH_WARN})
target_link_libraries(test_write_rounding PRIVATE write_rounding_probe)
add_test(NAME engine_write_rounding COMMAND test_write_rounding)
option(EH_WRITE_BENCHMARK "Build identity-rounding-only microbenchmark" OFF)
if(EH_WRITE_BENCHMARK)
    add_executable(bench_write_rounding bench_write_rounding.c)
    target_compile_options(bench_write_rounding PRIVATE ${EH_WARN})
    target_link_libraries(bench_write_rounding PRIVATE write_rounding_probe)
endif()

add_test(NAME engine_skyhub COMMAND test_engine --skyhub)

# Protocol regressions can run independently of historical request fixtures.
add_test(NAME engine_protocol COMMAND test_engine --protocol)
