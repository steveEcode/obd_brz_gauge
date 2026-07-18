set(
    GITHUB_UI_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../main/export_path"
)

file(
    GLOB
    GITHUB_UI_SCREEN_SOURCES
    CONFIGURE_DEPENDS
    "${GITHUB_UI_ROOT}/screens/*.c"
)

file(
    GLOB
    GITHUB_UI_FONT_SOURCES
    CONFIGURE_DEPENDS
    "${GITHUB_UI_ROOT}/fonts/*.c"
)

file(
    GLOB
    GITHUB_UI_IMAGE_SOURCES
    CONFIGURE_DEPENDS
    "${GITHUB_UI_ROOT}/images/*.c"
)

set(
    GITHUB_UI_CORE_SOURCES

    "${GITHUB_UI_ROOT}/ui.c"
    "${GITHUB_UI_ROOT}/ui_helpers.c"
    "${GITHUB_UI_ROOT}/components/ui_comp_hook.c"
)

set(
    GITHUB_UI_ALL_SOURCES

    ${GITHUB_UI_CORE_SOURCES}
    ${GITHUB_UI_SCREEN_SOURCES}
    ${GITHUB_UI_FONT_SOURCES}
    ${GITHUB_UI_IMAGE_SOURCES}
)

list(
    SORT
    GITHUB_UI_ALL_SOURCES
)
