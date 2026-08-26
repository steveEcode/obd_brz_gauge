// Theme-provided gauge page. Entered by swiping down from the Gear page,
// only when the active theme partition declares a "main_gauge" theme page
// (checked by the caller via theme_has_page() before this is ever invoked).
// The actual widget tree comes entirely from theme_create_page(); this file
// just wires it into the existing screen/navigation conventions.

#include "../ui.h"
#include "theme_engine/theme_interface.h"

void ui_ScreenPageThemeGauge_screen_init(void)
{
    ui_ScreenPageThemeGauge = theme_create_page("main_gauge");
    if (!ui_ScreenPageThemeGauge) {
        // Theme unloaded or page missing after all — fall back to a blank
        // screen rather than leaving the pointer NULL (which would make
        // _ui_screen_change() call this init function again on every swipe).
        ui_ScreenPageThemeGauge = lv_obj_create(NULL);
        lv_obj_clear_flag(ui_ScreenPageThemeGauge, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_add_event_cb(ui_ScreenPageThemeGauge, ui_event_theme_gauge_background, LV_EVENT_GESTURE, NULL);
}
