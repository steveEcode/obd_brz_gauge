cmake_minimum_required(VERSION 3.16)

project(BRZGaugeSimulator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
)


find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

set(LV_CONF_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h
    CACHE STRING "LVGL configuration path" FORCE
)

set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/lvgl)

add_library(lv_drivers STATIC
    third_party/lv_drivers/sdl/sdl.c
    third_party/lv_drivers/sdl/sdl_common.c
)

target_include_directories(lv_drivers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lv_drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lv_drivers/sdl
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(lv_drivers PUBLIC
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
)

target_compile_options(lv_drivers PRIVATE
    ${SDL2_CFLAGS_OTHER}
)

target_link_directories(lv_drivers PUBLIC
    ${SDL2_LIBRARY_DIRS}
)

target_link_libraries(lv_drivers PUBLIC
    lvgl
    ${SDL2_LIBRARIES}
)

add_executable(BRZGaugeSimulator
    src/main.c
    ../platform/simulator/mock_ecu.c
    ../platform/simulator/keyboard.c
)

target_include_directories(BRZGaugeSimulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lv_drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/model
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/simulator
)

target_compile_definitions(BRZGaugeSimulator PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
    USE_SDL=1
    SDL_INCLUDE_PATH=<SDL2/SDL.h>
)

target_link_libraries(BRZGaugeSimulator PRIVATE
    lvgl
    lv_drivers
    ${SDL2_LIBRARIES}
    m
    pthread
)
