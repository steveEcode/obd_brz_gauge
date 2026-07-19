option(
    SIMULATOR_ENABLE_GITHUB_UI_PROBE
    "Compile the existing GitHub UI using simulator compatibility headers"
    OFF
)

if(SIMULATOR_ENABLE_GITHUB_UI_PROBE)
    include(
        "${CMAKE_CURRENT_LIST_DIR}/github_ui_sources.cmake"
    )

    set(
        SIMULATOR_UI_COMPAT_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/simulator/ui_compat/probe"
    )

    target_sources(
        BRZGaugeSimulator
        PRIVATE

        ${GITHUB_UI_ALL_SOURCES}

        "${SIMULATOR_UI_COMPAT_ROOT}/src/ui_compat_probe.c"
        "${SIMULATOR_UI_COMPAT_ROOT}/src/obd_data_sim.c"
        "${SIMULATOR_UI_COMPAT_ROOT}/src/ble_sim.c"
        "${SIMULATOR_UI_COMPAT_ROOT}/src/espnow_sim.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../main/app_obd_dsp/vehicle_profiles.c"
    )

    target_include_directories(
        BRZGaugeSimulator
        PRIVATE

        "${SIMULATOR_UI_COMPAT_ROOT}/include"

        "${CMAKE_CURRENT_LIST_DIR}/../../main"

        "${GITHUB_UI_ROOT}"
        "${GITHUB_UI_ROOT}/screens"
        "${GITHUB_UI_ROOT}/components"
        "${GITHUB_UI_ROOT}/fonts"
        "${GITHUB_UI_ROOT}/images"
    )

    target_compile_definitions(
        BRZGaugeSimulator
        PRIVATE

        SIMULATOR_BUILD=1
        SIMULATOR_USE_GITHUB_UI=1
    )
endif()
