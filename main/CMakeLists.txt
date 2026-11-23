# ---- UI theme table codegen ------------------------------------------------
# themes/registry.txt + themes/*/<id>/theme.toml -> export_path/ui_theme_generated.c
# Runs BEFORE idf_component_register because SRC_DIRS is globbed at register time.
# Adding a theme requires editing registry.txt, and registry.txt is watched
# below, so a new theme always triggers a reconfigure and regeneration.
set(THEMES_DIR "${CMAKE_CURRENT_LIST_DIR}/../themes")
file(GLOB THEME_MANIFESTS "${THEMES_DIR}/*/*/theme.toml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${THEMES_DIR}/registry.txt" ${THEME_MANIFESTS})

# idf.py sets `python` to the IDF virtualenv interpreter; fall back for plain
# cmake / IDE reconfigures. gen_themes.py has no third-party dependencies.
if(NOT DEFINED python)
    set(python "python3")
endif()

execute_process(
    COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/gen_themes.py"
    RESULT_VARIABLE THEME_GEN_RESULT
    OUTPUT_VARIABLE THEME_GEN_STDOUT
    ERROR_VARIABLE  THEME_GEN_STDERR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT THEME_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Theme generation failed.\n${THEME_GEN_STDOUT}\n${THEME_GEN_STDERR}\n"
            "See themes/README.md.")
endif()
if(THEME_GEN_STDERR)
    message(WARNING "${THEME_GEN_STDERR}")
endif()
message(STATUS "${THEME_GEN_STDOUT}")

# Build metadata for app-side compatibility checks and on-device manifests.
set(OBD_GAUGE_GIT_HASH "unknown")
set(OBD_GAUGE_GIT_BRANCH "unknown")
set(OBD_GAUGE_GIT_COUNT 0)

execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/.." rev-parse --short=12 HEAD
    RESULT_VARIABLE OBD_GAUGE_GIT_HASH_RESULT
    OUTPUT_VARIABLE OBD_GAUGE_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT OBD_GAUGE_GIT_HASH_RESULT EQUAL 0 OR OBD_GAUGE_GIT_HASH STREQUAL "")
    set(OBD_GAUGE_GIT_HASH "unknown")
endif()

execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/.." rev-parse --abbrev-ref HEAD
    RESULT_VARIABLE OBD_GAUGE_GIT_BRANCH_RESULT
    OUTPUT_VARIABLE OBD_GAUGE_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT OBD_GAUGE_GIT_BRANCH_RESULT EQUAL 0 OR OBD_GAUGE_GIT_BRANCH STREQUAL "")
    set(OBD_GAUGE_GIT_BRANCH "unknown")
endif()

execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/.." rev-list --count HEAD
    RESULT_VARIABLE OBD_GAUGE_GIT_COUNT_RESULT
    OUTPUT_VARIABLE OBD_GAUGE_GIT_COUNT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT OBD_GAUGE_GIT_COUNT_RESULT EQUAL 0 OR OBD_GAUGE_GIT_COUNT STREQUAL "")
    set(OBD_GAUGE_GIT_COUNT 0)
endif()

string(REGEX REPLACE "[^A-Za-z0-9._-]" "_" OBD_GAUGE_GIT_BRANCH_SAFE "${OBD_GAUGE_GIT_BRANCH}")
set(OBD_GAUGE_BUILD_TAG "${OBD_GAUGE_GIT_BRANCH_SAFE}-${OBD_GAUGE_GIT_COUNT}-${OBD_GAUGE_GIT_HASH}")

idf_component_register(SRC_DIRS
                    "."
                    "export_path/fonts"
                    "export_path/images"
                    "export_path/theme_assets"
                    "export_path/screens"
                    "export_path"
                    "app_obd_dsp"  # includes boot-lifetime LVGL runtime / frame wakeups
                    "bsp_obd_dsp"
                    "bsp_obd_dsp/lcd_driver"
                    "bsp_obd_dsp/lcd_driver/esp_lcd_st77916"
                    "bsp_obd_dsp/exio"
                    "bsp_obd_dsp/i2c_driver"
                    "bsp_obd_dsp/touch_driver"
                    "theme_engine"
                    "theme_engine/rules"
                    "vehicle"
                    "media"
                    INCLUDE_DIRS
                    "."
                    REQUIRES
                    esp_http_server
                    esp_rom
                    esp_driver_usb_serial_jtag
                    esp_wifi
                    bt
                    mbedtls
                    nvs_flash
                    app_update
                    json
                    sdmmc
                    esp_driver_sdmmc
                    fatfs
                    espcoredump
                    spi_flash
                    joltwallet__littlefs)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OBD_GAUGE_GIT_HASH="${OBD_GAUGE_GIT_HASH}"
    OBD_GAUGE_GIT_BRANCH="${OBD_GAUGE_GIT_BRANCH_SAFE}"
    OBD_GAUGE_GIT_COUNT=${OBD_GAUGE_GIT_COUNT}
    OBD_GAUGE_BUILD_TAG="${OBD_GAUGE_BUILD_TAG}")

# ---- Device web page (SoftAP /) ---------------------------------------------
# Single-file vanilla JS page, gzip-embedded; the build fails above 48 KB.
set(DEVICE_WEB_SRC "${CMAKE_CURRENT_LIST_DIR}/app_obd_dsp/device_web_page.html")
set(DEVICE_WEB_GZ  "${CMAKE_CURRENT_BINARY_DIR}/device_web_page.html.gz")
add_custom_command(OUTPUT "${DEVICE_WEB_GZ}"
    COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/gzip_asset.py"
            --max 49152 "${DEVICE_WEB_SRC}" "${DEVICE_WEB_GZ}"
    DEPENDS "${DEVICE_WEB_SRC}" "${CMAKE_CURRENT_LIST_DIR}/../tools/gzip_asset.py"
    VERBATIM)
add_custom_target(device_web_page_gz DEPENDS "${DEVICE_WEB_GZ}")
add_dependencies(${COMPONENT_LIB} device_web_page_gz)
target_add_binary_data(${COMPONENT_LIB} "${DEVICE_WEB_GZ}" BINARY)

# ---- Raw bootmedia image -----------------------------------------------------
# build/bootmedia.bin = [boot_block.txt padded with 0xFF to 4 KB][boot_block.bin],
# SD legacy format: copy build/bootmedia.bin to /OBD/BOOT/bootmedia.raw.
# There is no Flash media target.
set(BOOTMEDIA_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../bootmedia/slot_a")
set(BOOTMEDIA_RAW "${CMAKE_BINARY_DIR}/bootmedia.bin")
add_custom_command(OUTPUT "${BOOTMEDIA_RAW}"
    COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/make_bootmedia_raw.py"
            --src "${BOOTMEDIA_SRC_DIR}" --out "${BOOTMEDIA_RAW}"
    DEPENDS "${BOOTMEDIA_SRC_DIR}/boot_block.txt" "${BOOTMEDIA_SRC_DIR}/boot_block.bin"
            "${CMAKE_CURRENT_LIST_DIR}/../tools/make_bootmedia_raw.py"
    VERBATIM)
add_custom_target(bootmedia_raw ALL DEPENDS "${BOOTMEDIA_RAW}")

# Intercept SDK sector IO (including FatFs metadata), not just application fread.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=sdmmc_read_sectors" "-Wl,--wrap=sdmmc_write_sectors")
