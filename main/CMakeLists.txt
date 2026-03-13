file(GLOB_RECURSE SOURCES_C *.c)
file(GLOB_RECURSE SOURCES_CPP *.cpp)

idf_component_register(SRC_DIRS
                    "." 
                    "export_path/components"
                    "export_path/fonts"
                    "export_path/images"
                    "export_path/screens"
                    "export_path"
                    "app_obd_dsp"
                    "bsp_obd_dsp"
                    "bsp_obd_dsp/lcd_driver"
                    "bsp_obd_dsp/lcd_driver/esp_lcd_st77916"
                    "bsp_obd_dsp/exio"
                    "bsp_obd_dsp/i2c_driver"
                    "bsp_obd_dsp/touch_driver"
                    INCLUDE_DIRS
                    ".")

