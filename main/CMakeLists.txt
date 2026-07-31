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

# 自动生成 bootmedia SPIFFS 镜像, idf.py flash 时一起烧录
spiffs_create_partition_image(bootmedia ${CMAKE_CURRENT_SOURCE_DIR}/../bootmedia/slot_a FLASH_IN_PROJECT)

