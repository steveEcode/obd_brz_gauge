SET(SOURCES screens/ui_ScreenPageMain.c
    screens/ui_ScreenPageGear.c
    screens/ui_ScreenPageRpm.c
    screens/ui_ScreenPageSpeed.c
    screens/ui_ScreenPageEasterEgg.c
    screens/ui_ScreenPageODBProtocal.c
    screens/ui_ScreenPageInfo.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_pngmainback_png.c
    images/ui_img_pngblackear_png.c
    fonts/ui_font_FontBabyGearNumSize48.c
    fonts/ui_font_FontBabySize108.c
    fonts/ui_font_FontBabySize156.c
    fonts/ui_font_FontBabySize180.c
    fonts/ui_font_FontBabySize200.c
    fonts/ui_font_FontBabySize56.c
    fonts/ui_font_FontTaikongSize108.c
    fonts/ui_font_FontTaikongSize128.c
    fonts/ui_font_FontTaikongSize32.c
    fonts/ui_font_FontTaikongSize40.c
    fonts/ui_font_FontTaikongSize48.c
    fonts/ui_font_FontTaikongSize56.c
    fonts/ui_font_FontTaikongSize64.c
    fonts/ui_font_FontTaikongSize72.c
    fonts/ui_font_FontTypoderSize16.c
    fonts/ui_font_FontTypoderSize20.c
    fonts/ui_font_FontTypoderSize24.c
    fonts/ui_font_FontTypoderSize28.c
    fonts/ui_font_FontTypoderSize32.c
    fonts/ui_font_FontTypoderSize36.c
    fonts/ui_font_FontTypoderSize40.c
    fonts/ui_font_FontTypoderSize44.c)

add_library(ui ${SOURCES})
