cmake_minimum_required(VERSION 3.16)
project(signal_store_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

option(SS_SANITIZE "Build with address/undefined sanitizers when supported" ON)
if(SS_SANITIZE)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" SS_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(SS_HAVE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

add_executable(test_signal_store
    test_signal_store.c
    "${REPO_ROOT}/main/app_obd_dsp/signal_store.c"
    "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c"
    "${REPO_ROOT}/main/app_obd_dsp/obd_data_cache.c")
target_include_directories(test_signal_store PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${REPO_ROOT}/main")
target_compile_options(test_signal_store PRIVATE -Wall -Wextra -Werror)

if(NOT MSVC)
    target_link_libraries(test_signal_store PRIVATE m)
endif()

enable_testing()
add_test(NAME signal_store COMMAND test_signal_store)

# Interleave a writer immediately before the reader actually acquires its lock.
add_executable(test_signal_clock_race test_signal_clock_race.c
    "${REPO_ROOT}/main/app_obd_dsp/signal_store.c"
    "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c")
target_include_directories(test_signal_clock_race PRIVATE race_stubs stubs "${REPO_ROOT}/main")
target_compile_options(test_signal_clock_race PRIVATE -Wall -Wextra -Werror -UNDEBUG)
if(NOT MSVC)
    target_link_libraries(test_signal_clock_race PRIVATE m)
endif()
add_test(NAME signal_clock_race COMMAND test_signal_clock_race)

# On-device performance aggregation is pure C; keep it in the application-data job.
add_executable(test_perf_hist
    test_perf_hist.c
    "${REPO_ROOT}/main/app_obd_dsp/perf_hist.c")
target_include_directories(test_perf_hist PRIVATE "${REPO_ROOT}/main")
target_compile_options(test_perf_hist PRIVATE -Wall -Wextra -Werror)
add_test(NAME perf_hist COMMAND test_perf_hist)

# Compare actual delivered-unit formulas against the original implementation.
add_library(delivery_check STATIC delivery_reference.c delivery_candidate.c
    "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c")
target_include_directories(delivery_check PUBLIC "${REPO_ROOT}/main")
target_compile_options(delivery_check PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
if(NOT MSVC)
    target_link_libraries(delivery_check PUBLIC m)
endif()
add_executable(test_delivery test_delivery.c
    "${REPO_ROOT}/main/app_obd_dsp/signal_store.c")
target_include_directories(test_delivery PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
target_compile_options(test_delivery PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
target_link_libraries(test_delivery PRIVATE delivery_check)
add_test(NAME signal_delivery COMMAND test_delivery)

option(SS_DELIVERY_BENCHMARK "Build delivered-unit conversion microbenchmark" OFF)
if(SS_DELIVERY_BENCHMARK)
    add_executable(bench_delivery bench_delivery.c)
    target_compile_options(bench_delivery PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
    target_link_libraries(bench_delivery PRIVATE delivery_check)
endif()

# Sparse UI snapshots must exactly preserve the existing facade semantics.
add_executable(test_snapshot_mask
    test_snapshot_mask.c
    "${REPO_ROOT}/main/app_obd_dsp/signal_store.c"
    "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c"
    "${REPO_ROOT}/main/app_obd_dsp/obd_data_cache.c")
target_include_directories(test_snapshot_mask PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main")
target_compile_options(test_snapshot_mask PRIVATE -Wall -Wextra -Werror)
add_test(NAME snapshot_mask COMMAND test_snapshot_mask)

# Numeric lookup on real store metadata, compared with the frozen old scan.
add_executable(test_signal_lookup test_lookup.c lookup_probe.c
    "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c")
target_include_directories(test_signal_lookup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main")
target_compile_options(test_signal_lookup PRIVATE -Wall -Wextra -Werror)
if(NOT MSVC)
    target_link_libraries(test_signal_lookup PRIVATE m)
endif()
add_test(NAME signal_lookup COMMAND test_signal_lookup)

option(SS_LOOKUP_BENCHMARK "Build signal ID lookup and ESP-NOW apply benchmark" OFF)
if(SS_LOOKUP_BENCHMARK)
    add_executable(bench_signal_lookup bench_lookup.c lookup_probe.c
        "${REPO_ROOT}/main/app_obd_dsp/signal_registry.c"
        "${REPO_ROOT}/main/bsp_obd_dsp/espnow_v6.c")
    target_include_directories(bench_signal_lookup PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main")
    target_compile_options(bench_signal_lookup PRIVATE -Wall -Wextra -Werror)
    if(NOT MSVC)
        target_link_libraries(bench_signal_lookup PRIVATE m)
    endif()
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME signal_lookup_generator COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lookup_generator.py" "${CMAKE_C_COMPILER}")
    add_test(NAME boot_page COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_boot_page.py" "${CMAKE_C_COMPILER}")
endif()

# Check actual label-update behavior, including signed pressure and placeholders.
if(Python3_Interpreter_FOUND)
    set(DISPLAY_UPDATE_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/display_update.inc")
    add_custom_command(OUTPUT "${DISPLAY_UPDATE_EXCERPT}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_display_update.py"
                "${REPO_ROOT}/main/export_path/ui_disp_item.c" "${DISPLAY_UPDATE_EXCERPT}"
        DEPENDS "${REPO_ROOT}/main/export_path/ui_disp_item.c" extract_display_update.py
        VERBATIM)
    add_executable(test_display_response test_display_response.c "${DISPLAY_UPDATE_EXCERPT}")
    target_include_directories(test_display_response PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" stubs "${REPO_ROOT}/main")
    target_compile_options(test_display_response PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME display_response COMMAND test_display_response)
endif()

# Sweep is elapsed-time presentation, independent of timer callback frequency.
add_executable(test_sweep_clock test_sweep_clock.c
    "${REPO_ROOT}/main/export_path/ui_sweep.c")
target_include_directories(test_sweep_clock PRIVATE "${REPO_ROOT}/main")
target_compile_options(test_sweep_clock PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME sweep_clock COMMAND test_sweep_clock)

if(Python3_Interpreter_FOUND)
    set(SWEEP_RUNTIME_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/sweep_runtime.inc")
    add_custom_command(OUTPUT "${SWEEP_RUNTIME_EXCERPT}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_sweep_runtime.py"
                "${REPO_ROOT}/main/export_path/ui_ext.c" "${SWEEP_RUNTIME_EXCERPT}"
        DEPENDS "${REPO_ROOT}/main/export_path/ui_ext.c"
                "${REPO_ROOT}/main/export_path/ui.c"
                "${REPO_ROOT}/main/export_path/ui_ext.h" extract_sweep_runtime.py
        VERBATIM)
    add_executable(test_sweep_runtime test_sweep_runtime.c "${SWEEP_RUNTIME_EXCERPT}"
        "${REPO_ROOT}/main/export_path/ui_sweep.c")
    set_property(TARGET test_sweep_runtime PROPERTY C_STANDARD 11)
    target_include_directories(test_sweep_runtime PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}" "${REPO_ROOT}/main")
    target_compile_options(test_sweep_runtime PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME sweep_runtime COMMAND test_sweep_runtime)
endif()
