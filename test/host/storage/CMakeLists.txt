# Real storage_service.c, actual project API headers, deterministic SDK boundary.
cmake_minimum_required(VERSION 3.16)
project(storage_host_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(STORAGE_SOURCE "${REPO_ROOT}/main/app_obd_dsp/storage_service.c" CACHE FILEPATH
    "Storage implementation under test (override for negative controls)")
option(SH_SANITIZE "Use ASAN/UBSAN when supported" ON)
if(SH_SANITIZE)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" SH_HAVE_SANITIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(SH_HAVE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                            -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
enable_testing()
foreach(mode slot no_slot)
    add_executable(test_storage_${mode} test_storage_service.c "${STORAGE_SOURCE}")
    target_include_directories(test_storage_${mode} PRIVATE stubs "${REPO_ROOT}/main")
    target_compile_options(test_storage_${mode} PRIVATE -Wall -Wextra -Werror -Wshadow
                           -Wconversion -Wno-sign-conversion)
    if(mode STREQUAL "slot")
        target_compile_definitions(test_storage_${mode} PRIVATE STORAGE_TEST_HAS_SLOT=1)
    endif()
    add_test(NAME storage_${mode} COMMAND test_storage_${mode})
    set_tests_properties(storage_${mode} PROPERTIES TIMEOUT 30)

    add_executable(test_sd_${mode} test_sd_card.c "${REPO_ROOT}/main/bsp_obd_dsp/sd_card.c")
    target_include_directories(test_sd_${mode} PRIVATE sd_stubs stubs "${REPO_ROOT}/main")
    target_compile_options(test_sd_${mode} PRIVATE -Wall -Wextra -Werror -Wshadow)
    target_compile_definitions(test_sd_${mode} PRIVATE _POSIX_C_SOURCE=200809L)
    if(mode STREQUAL "slot")
        target_compile_definitions(test_sd_${mode} PRIVATE STORAGE_TEST_HAS_SLOT=1)
    endif()
    add_test(NAME sd_${mode} COMMAND test_sd_${mode})
    set_tests_properties(sd_${mode} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(test_vehicle_web_cors test_vehicle_web_cors.c)
target_include_directories(test_vehicle_web_cors PRIVATE "${REPO_ROOT}/main")
target_compile_options(test_vehicle_web_cors PRIVATE -Wall -Wextra -Werror)
add_test(NAME vehicle_web_cors COMMAND test_vehicle_web_cors)

# Exercise the actual USB receiver session against deterministic SDK/sink boundaries.
add_executable(test_serial_storage test_serial_storage.c)
target_include_directories(test_serial_storage PRIVATE serial_stubs sd_stubs stubs "${REPO_ROOT}/main")
target_compile_options(test_serial_storage PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME serial_storage COMMAND test_serial_storage)

# Old local sdkconfig may still have a USB primary/secondary console.
add_executable(test_serial_storage_usb_console test_serial_storage.c)
target_include_directories(test_serial_storage_usb_console PRIVATE serial_stubs sd_stubs stubs "${REPO_ROOT}/main")
target_compile_definitions(test_serial_storage_usb_console PRIVATE CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=1)
target_compile_options(test_serial_storage_usb_console PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME serial_storage_usb_console COMMAND test_serial_storage_usb_console)


# Exercise the real I2C wrapper against a synchronous fake IDF bus.
find_package(Threads REQUIRED)
add_executable(test_i2c_driver test_i2c_driver.c "${REPO_ROOT}/main/bsp_obd_dsp/i2c_driver/I2C_Driver.c")
target_include_directories(test_i2c_driver PRIVATE i2c_stubs stubs "${REPO_ROOT}/main")
target_compile_options(test_i2c_driver PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(test_i2c_driver PRIVATE Threads::Threads)
add_test(NAME i2c_driver COMMAND test_i2c_driver)

add_executable(test_ota_idle test_ota_idle.c)
target_include_directories(test_ota_idle PRIVATE stubs "${REPO_ROOT}/main")
target_compile_options(test_ota_idle PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME ota_idle COMMAND test_ota_idle)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(OTA_TIMEOUT_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/ota_timeout_task.inc")
add_custom_command(OUTPUT "${OTA_TIMEOUT_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_ota_timeout.py"
            "${REPO_ROOT}/main/app_obd_dsp/ota_wifi_server.c" "${OTA_TIMEOUT_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/app_obd_dsp/ota_wifi_server.c" extract_ota_timeout.py
    VERBATIM)
add_executable(test_ota_timeout test_ota_timeout.c "${OTA_TIMEOUT_EXCERPT}")
target_include_directories(test_ota_timeout PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" stubs "${REPO_ROOT}/main")
target_compile_options(test_ota_timeout PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME ota_timeout COMMAND test_ota_timeout)

add_executable(test_upload_sink test_upload_sink.c)
target_include_directories(test_upload_sink PRIVATE upload_stubs "${REPO_ROOT}/main")
target_compile_definitions(test_upload_sink PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(test_upload_sink PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME upload_sink COMMAND test_upload_sink)

set(SD_RENAME_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/sd_rename.inc")
add_custom_command(OUTPUT "${SD_RENAME_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_sd_rename.py"
            "${REPO_ROOT}/main/bsp_obd_dsp/sd_card.c" "${SD_RENAME_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/bsp_obd_dsp/sd_card.c" extract_sd_rename.py
    VERBATIM)
add_executable(test_sd_rename test_sd_rename.c "${SD_RENAME_EXCERPT}")
target_include_directories(test_sd_rename PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" upload_stubs)
target_compile_options(test_sd_rename PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME sd_rename COMMAND test_sd_rename)

set(OTA_SPACE_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/ota_space.inc")
add_custom_command(OUTPUT "${OTA_SPACE_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_ota_space.py"
            "${REPO_ROOT}/main/app_obd_dsp/ota_wifi_server.c" "${OTA_SPACE_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/app_obd_dsp/ota_wifi_server.c" extract_ota_space.py
    VERBATIM)
add_executable(test_ota_space test_ota_space.c "${OTA_SPACE_EXCERPT}")
target_include_directories(test_ota_space PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(test_ota_space PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME ota_space COMMAND test_ota_space)

set(HTTP_HEADERS_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/http_headers.inc")
add_custom_command(OUTPUT "${HTTP_HEADERS_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_http_headers.py"
            "${REPO_ROOT}/main/app_obd_dsp/ota_wifi_server.c"
            "${REPO_ROOT}/main/app_obd_dsp/api_v1_handlers.c" "${HTTP_HEADERS_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/app_obd_dsp/ota_wifi_server.c"
            "${REPO_ROOT}/main/app_obd_dsp/api_v1_handlers.c" extract_http_headers.py
    VERBATIM)
add_executable(test_http_headers test_http_headers.c "${HTTP_HEADERS_EXCERPT}")
target_include_directories(test_http_headers PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" sd_stubs stubs "${REPO_ROOT}/main")
target_compile_options(test_http_headers PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME http_headers COMMAND test_http_headers)

set(OTA_PAGE_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/ota_page.inc")
add_custom_command(OUTPUT "${OTA_PAGE_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_ota_page.py"
            "${REPO_ROOT}/main/export_path/screens/ui_ScreenPageOTAMode.c" "${OTA_PAGE_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/export_path/screens/ui_ScreenPageOTAMode.c" extract_ota_page.py
    VERBATIM)
add_executable(test_ota_page test_ota_page.c "${OTA_PAGE_EXCERPT}")
target_include_directories(test_ota_page PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" stubs "${REPO_ROOT}/main")
target_compile_options(test_ota_page PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME ota_page COMMAND test_ota_page)

add_executable(test_sd_io_guard test_sd_io_guard.c "${REPO_ROOT}/main/bsp_obd_dsp/sd_io_guard.c")
target_include_directories(test_sd_io_guard PRIVATE sd_stubs stubs "${REPO_ROOT}/main")
target_compile_options(test_sd_io_guard PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME sd_io_guard COMMAND test_sd_io_guard)

set(SD_FORMAT_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/sd_format.inc")
add_custom_command(OUTPUT "${SD_FORMAT_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_sd_format.py"
            "${REPO_ROOT}/main/app_obd_dsp/ota_wifi_server.c" "${SD_FORMAT_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/app_obd_dsp/ota_wifi_server.c" extract_sd_format.py VERBATIM)
add_executable(test_sd_format test_sd_format.c "${SD_FORMAT_EXCERPT}")
target_include_directories(test_sd_format PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" stubs "${REPO_ROOT}/main")
target_compile_options(test_sd_format PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME sd_format COMMAND test_sd_format)

set(SD_FORMAT_UI_EXCERPT "${CMAKE_CURRENT_BINARY_DIR}/sd_format_ui.inc")
add_custom_command(OUTPUT "${SD_FORMAT_UI_EXCERPT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_sd_format_ui.py"
            "${REPO_ROOT}/main/export_path/screens/ui_ScreenPageOTAMode.c" "${SD_FORMAT_UI_EXCERPT}"
    DEPENDS "${REPO_ROOT}/main/export_path/screens/ui_ScreenPageOTAMode.c" extract_sd_format_ui.py VERBATIM)
add_executable(test_sd_format_ui test_sd_format_ui.c "${SD_FORMAT_UI_EXCERPT}")
target_include_directories(test_sd_format_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" stubs "${REPO_ROOT}/main")
target_compile_options(test_sd_format_ui PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME sd_format_ui COMMAND test_sd_format_ui)
