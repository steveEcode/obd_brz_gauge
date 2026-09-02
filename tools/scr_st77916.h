#ifndef _SCR_ST77916_H_
#define _SCR_ST77916_H_

#include "config.h"
#include <lvgl.h>
#include <ESP_IOExpander.h>
#include <ESP_Panel_Library.h>
#include <ui.h>
#include "driver/i2c.h"
#include "driver/spi_master.h"

static lv_color_t *disp_draw_buf;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_t *indev_touchpad;

static ESP_PanelBacklight *backlight = NULL;
static ESP_PanelLcd *lcd = NULL;
static ESP_PanelTouch *touch = NULL;
static ESP_PanelBus_I2C *touch_bus = NULL;
static ESP_PanelBus_QSPI *panel_bus = NULL;

/**
 * @brief 清理屏幕硬件资源
 * 
 * 在删除LVGL任务之前调用此函数，清理已初始化的硬件资源，
 * 以便后续可以重新初始化屏幕
 */
void scr_lvgl_cleanup()
{
    MY_INFO("Cleaning up screen hardware resources...\n");
    
    // 先删除触摸芯片对象
    if (touch != NULL)
    {
        delete touch;
        touch = NULL;
        MY_INFO("Touch driver cleaned up\n");
    }
    
    // 删除触摸总线对象（让它的析构函数处理I2C驱动卸载）
    if (touch_bus != NULL)
    {
        delete touch_bus;
        touch_bus = NULL;
        MY_INFO("Touch bus cleaned up\n");
    }
    
    // 删除LCD面板对象
    if (lcd != NULL)
    {
        delete lcd;
        lcd = NULL;
        MY_INFO("LCD panel cleaned up\n");
    }
    
    // 删除面板总线对象（让它的析构函数处理SPI总线释放）
    if (panel_bus != NULL)
    {
        // 添加短暂延迟，确保LCD面板完全释放SPI设备，避免SPI总线释放时断言失败
        vTaskDelay(20 / portTICK_PERIOD_MS);
        //delay(100);
        
        // 使用try-catch包裹，避免SPI总线释放失败导致崩溃
        try {
            delete panel_bus;
            panel_bus = NULL;
            MY_INFO("Panel bus cleaned up\n");
        } catch (...) {
            MY_INFO("Warning: Failed to clean up panel bus (SPI bus still in use)\n");
        }
    }
    
    // 删除背光对象
    if (backlight != NULL)
    {
        delete backlight;
        backlight = NULL;
        MY_INFO("Backlight cleaned up\n");
    }
    
    // 清理LVGL显示缓冲区
    if (disp_draw_buf != NULL)
    {
        free(disp_draw_buf);
        disp_draw_buf = NULL;
        MY_INFO("Display draw buffer freed\n");
    }
    
    // 清理LVGL
    lv_deinit();
    MY_INFO("LVGL deinitialized\n");
    
    MY_INFO("Screen hardware resources cleaned up\n");
}

/**
 * @brief 仅清理屏幕硬件资源（不清理LVGL）
 * 
 * 在需要保留LVGL状态的情况下调用此函数，仅清理硬件资源。
 * 用于从loop模式恢复到MJPEG模式时，避免lv_deinit()破坏内存管理。
 */
void scr_hardware_cleanup_only()
{
    MY_INFO("Cleaning up hardware resources only (keeping LVGL)...\n");
    
    // 先删除触摸芯片对象
    if (touch != NULL)
    {
        delete touch;
        touch = NULL;
        MY_INFO("Touch driver cleaned up\n");
    }
    
    // 删除触摸总线对象
    if (touch_bus != NULL)
    {
        delete touch_bus;
        touch_bus = NULL;
        MY_INFO("Touch bus cleaned up\n");
    }
    
    // 删除LCD面板对象
    if (lcd != NULL)
    {
        delete lcd;
        lcd = NULL;
        MY_INFO("LCD panel cleaned up\n");
    }
    
    // 删除面板总线对象
    if (panel_bus != NULL)
    {
        // 添加短暂延迟，确保LCD面板完全释放SPI设备，避免SPI总线释放时断言失败
        vTaskDelay(20 / portTICK_PERIOD_MS);
        //delay(100);
        
        // 使用try-catch包裹，避免SPI总线释放失败导致崩溃
        try {
            delete panel_bus;
            panel_bus = NULL;
            MY_INFO("Panel bus cleaned up\n");
        } catch (...) {
            // SPI总线释放失败，可能是因为还有设备绑定，记录警告但不崩溃
            MY_INFO("Warning: Failed to clean up panel bus (SPI bus still in use)\n");
            // 不设置panel_bus为NULL，下次清理时再尝试
        }
    }
    
    // 删除背光对象
    if (backlight != NULL)
    {
        delete backlight;
        backlight = NULL;
        MY_INFO("Backlight cleaned up\n");
    }
    
    // 清理LVGL显示缓冲区（但不清除LVGL本身）
    if (disp_draw_buf != NULL)
    {
        free(disp_draw_buf);
        disp_draw_buf = NULL;
        MY_INFO("Display draw buffer freed\n");
    }
    
    // 不调用 lv_deinit()，保持LVGL状态
    
    MY_INFO("Hardware resources cleaned up (LVGL kept)\n");
}



#define USE_CUSTOM_INIT_CMD SCR_VERSION // 是否用自定义的初始化代码，由 device_model.h 中的 SCR_VERSION 宏控制

const esp_lcd_panel_vendor_init_cmd_t lcd_init_cmd_v1[] = {
    {0xF0, (uint8_t[]){0x08}, 1, 0},
    {0xF2, (uint8_t[]){0x08}, 1, 0},
    {0x9B, (uint8_t[]){0x51}, 1, 0},
    {0x86, (uint8_t[]){0x53}, 1, 0},
    {0xF2, (uint8_t[]){0x80}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x54}, 1, 0},
    {0xB1, (uint8_t[]){0x3F}, 1, 0},
    {0xB2, (uint8_t[]){0x2A}, 1, 0},
    {0xB4, (uint8_t[]){0x46}, 1, 0},
    {0xB5, (uint8_t[]){0x34}, 1, 0},
    {0xB6, (uint8_t[]){0xD5}, 1, 0},
    {0xB7, (uint8_t[]){0x30}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x51}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x51}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x69}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x3F}, 1, 0},
    {0xDE, (uint8_t[]){0x3F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
    {0xE1, (uint8_t[]){0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x08}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x00}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x82}, 1, 0},
    {0xEA, (uint8_t[]){0xDF}, 1, 0},
    {0xEB, (uint8_t[]){0x89}, 1, 0},
    {0xEC, (uint8_t[]){0x20}, 1, 0},
    {0xED, (uint8_t[]){0x14}, 1, 0},
    {0xEE, (uint8_t[]){0xFF}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0xFF}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x30}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x42}, 1, 0},
    {0x61, (uint8_t[]){0xE0}, 1, 0},
    {0x62, (uint8_t[]){0x40}, 1, 0},
    {0x63, (uint8_t[]){0x40}, 1, 0},
    {0x64, (uint8_t[]){0x02}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x40}, 1, 0},
    {0x67, (uint8_t[]){0x03}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x42}, 1, 0},
    {0x71, (uint8_t[]){0xE0}, 1, 0},
    {0x72, (uint8_t[]){0x40}, 1, 0},
    {0x73, (uint8_t[]){0x40}, 1, 0},
    {0x74, (uint8_t[]){0x02}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x40}, 1, 0},
    {0x77, (uint8_t[]){0x03}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x05}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xDD}, 1, 0},
    {0x85, (uint8_t[]){0x00}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x07}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xDF}, 1, 0},
    {0x8D, (uint8_t[]){0x00}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x09}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xE1}, 1, 0},
    {0x95, (uint8_t[]){0x00}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0B}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xE3}, 1, 0},
    {0x9D, (uint8_t[]){0x00}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x04}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xDC}, 1, 0},
    {0xA5, (uint8_t[]){0x00}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x06}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xDE}, 1, 0},
    {0xAD, (uint8_t[]){0x00}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x08}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xE0}, 1, 0},
    {0xB5, (uint8_t[]){0x00}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0A}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xE2}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x12}, 1, 0},
    {0xC1, (uint8_t[]){0xAA}, 1, 0},
    {0xC2, (uint8_t[]){0x65}, 1, 0},
    {0xC3, (uint8_t[]){0x74}, 1, 0},
    {0xC4, (uint8_t[]){0x47}, 1, 0},
    {0xC5, (uint8_t[]){0x56}, 1, 0},
    {0xC6, (uint8_t[]){0x00}, 1, 0},
    {0xC7, (uint8_t[]){0x88}, 1, 0},
    {0xC8, (uint8_t[]){0x99}, 1, 0},
    {0xC9, (uint8_t[]){0x33}, 1, 0},
    {0xD0, (uint8_t[]){0x21}, 1, 0},
    {0xD1, (uint8_t[]){0xAA}, 1, 0},
    {0xD2, (uint8_t[]){0x65}, 1, 0},
    {0xD3, (uint8_t[]){0x74}, 1, 0},
    {0xD4, (uint8_t[]){0x47}, 1, 0},
    {0xD5, (uint8_t[]){0x56}, 1, 0},
    {0xD6, (uint8_t[]){0x00}, 1, 0},
    {0xD7, (uint8_t[]){0x88}, 1, 0},
    {0xD8, (uint8_t[]){0x99}, 1, 0},
    {0xD9, (uint8_t[]){0x33}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xA0, (uint8_t[]){0x0B}, 1, 0},
    {0xA3, (uint8_t[]){0x2A}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x2B}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x2C}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x2D}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x2E}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x2F}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x30}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x31}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x32}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA3, (uint8_t[]){0x33}, 1, 0},
    {0xA5, (uint8_t[]){0xC3}, 1, 1},
    {0xA0, (uint8_t[]){0x09}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4, 0},
    {0x2B, (uint8_t[]){0x01, 0x68, 0x01, 0x68}, 4, 0},
    {0x4D, (uint8_t[]){0x00}, 1, 0},
    {0x4E, (uint8_t[]){0x00}, 1, 0},
    {0x4F, (uint8_t[]){0x00}, 1, 0},
    {0x4C, (uint8_t[]){0x01}, 1, 10},
    {0x4C, (uint8_t[]){0x00}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    //{0x3A, (uint8_t[]){0x55}, 1, 0}, // color=16
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
};

const esp_lcd_panel_vendor_init_cmd_t lcd_init_cmd_v2[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0}
};
#define TFT_SPI_FREQ_HZ (50 * 1000 * 1000)

static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    ESP_PanelLcd *lcd = (ESP_PanelLcd *)disp->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;
#ifndef USE_LV_SWAP_COLOR
    // 计算绘制区域的大小
    int size = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);
    // 交换字节序
    for (int i = 0; i < size; i++)
    {
        color_p[i].full = __builtin_bswap16(color_p[i].full);
    }
#endif
    lcd->drawBitmap(offsetx1, offsety1, offsetx2 - offsetx1 + 1, offsety2 - offsety1 + 1, (const uint8_t *)color_p);
}

IRAM_ATTR bool onRefreshFinishCallback(void *user_data)
{
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_data;
    lv_disp_flush_ready(drv);
    return false;
}

void setRotation(uint8_t rot)
{
    if (rot > DEV_ROTATE_270)
        return;
    if (lcd == NULL || touch == NULL)
        return;

    cfg.rotate = rot;
    switch (cfg.rotate)
    {
    case DEV_ROTATE_90: // 顺时针90度
        lcd->swapXY(true);
        lcd->mirrorX(true);
        lcd->mirrorY(false);
        touch->swapXY(true);
        touch->mirrorX(true);
        touch->mirrorY(false);
        break;
    case DEV_ROTATE_180:
        lcd->swapXY(false);
        lcd->mirrorX(true);
        lcd->mirrorY(true);
        touch->swapXY(false);
        touch->mirrorX(true);
        touch->mirrorY(true);
        break;
    case DEV_ROTATE_270:
        lcd->swapXY(true);
        lcd->mirrorX(false);
        lcd->mirrorY(true);
        touch->swapXY(true);
        touch->mirrorX(false);
        touch->mirrorY(true);
        break;
    default:
        lcd->swapXY(false);
        lcd->mirrorX(false);
        lcd->mirrorY(false);
        touch->swapXY(false);
        touch->mirrorX(false);
        touch->mirrorY(false);
        break;
    }
}

void screen_switch(bool on)
{
    if (NULL == backlight)
        return;
    if (on)
        backlight->on();
    else
        backlight->off();
}

// 输入值为0-100
void set_brightness(uint8_t bri)
{
    if (NULL == backlight)
        return;
    backlight->setBrightness(bri);
}


static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    ESP_PanelTouch *tp = (ESP_PanelTouch *)indev_drv->user_data;
    ESP_PanelTouchPoint point;

    int read_touch_result = tp->readPoints(&point, 1);
    if (read_touch_result > 0)
    {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_indev_t *indev_init(ESP_PanelTouch *tp)
{
    ESP_PANEL_CHECK_FALSE_RET(tp != nullptr, nullptr, "Invalid touch device");
    ESP_PANEL_CHECK_FALSE_RET(tp->getHandle() != nullptr, nullptr, "Touch device is not initialized");
    static lv_indev_drv_t indev_drv_tp;
    lv_indev_drv_init(&indev_drv_tp);
    indev_drv_tp.type = LV_INDEV_TYPE_POINTER;
    indev_drv_tp.read_cb = touchpad_read;
    indev_drv_tp.user_data = (void *)tp;
    return lv_indev_drv_register(&indev_drv_tp);
}

void scr_lvgl_init()
{
    MY_INFO("scr_lvgl_init() starting...\n");

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
    MY_INFO("LED timer configured\n");

    ledc_channel_config_t ledc_channel = {
        .gpio_num = (TFT_BLK),
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0};

    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    MY_INFO("LED channel configured\n");

    backlight = new ESP_PanelBacklight(ledc_timer, ledc_channel);
    backlight->begin();
    backlight->off();
    MY_INFO("Backlight initialized\n");

    touch_bus = new ESP_PanelBus_I2C(TOUCH_PIN_NUM_I2C_SCL, TOUCH_PIN_NUM_I2C_SDA, ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG());
    touch_bus->configI2cFreqHz(400000);
    touch_bus->begin();
    MY_INFO("Touch I2C bus initialized\n");

    touch = new ESP_PanelTouch_CST816S(touch_bus, SCREEN_RES_HOR, SCREEN_RES_VER, TOUCH_PIN_NUM_RST, TOUCH_PIN_NUM_INT);

    touch->init();
    touch->begin();
    MY_INFO("Touch panel initialized\n");

    panel_bus = new ESP_PanelBus_QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
    panel_bus->configQspiFreqHz(TFT_SPI_FREQ_HZ);
    panel_bus->begin();
    MY_INFO("Panel QSPI bus initialized\n");

    lcd = new ESP_PanelLcd_ST77916(panel_bus, 16, TFT_RST);
    // 注意，初始化代码的设置必须在INIT之前
#if USE_CUSTOM_INIT_CMD == 1
    lcd->configVendorCommands(lcd_init_cmd_v1, sizeof(lcd_init_cmd_v1) / sizeof(lcd_init_cmd_v1[0]));
#elif USE_CUSTOM_INIT_CMD == 2
    lcd->configVendorCommands(lcd_init_cmd_v2, sizeof(lcd_init_cmd_v2) / sizeof(lcd_init_cmd_v2[0]));
#endif

    lcd->init();
    lcd->reset();
    lcd->begin();
    MY_INFO("LCD panel initialized\n");

    lcd->invertColor(true);
    setRotation(cfg.rotate);
    lcd->displayOn();
    MY_INFO("LCD display on\n");

    size_t lv_cache_rows = 90;

    disp_draw_buf = (lv_color_t *)heap_caps_malloc(lv_cache_rows * SCREEN_RES_HOR * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    memset(disp_draw_buf, 0, lv_cache_rows * SCREEN_RES_HOR * 2);
    MY_INFO("Display draw buffer allocated\n");

    for (int i = 0; i < SCREEN_RES_VER / lv_cache_rows; i++)
    {
        lcd->drawBitmap(0, lv_cache_rows * i, SCREEN_RES_HOR, lv_cache_rows, (const uint8_t *)disp_draw_buf); // 这里不能用PSRAM刷，只能用SRAM
    }
    MY_INFO("Screen cleared\n");

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, SCREEN_RES_HOR * lv_cache_rows);
    MY_INFO("LVGL initialized\n");

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_RES_HOR;
    disp_drv.ver_res = SCREEN_RES_VER;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = (void *)lcd;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    MY_INFO("LVGL display driver registered\n");

    if (lcd->getBus()->getType() != ESP_PANEL_BUS_TYPE_RGB)
    {
        lcd->attachRefreshFinishCallback(onRefreshFinishCallback, (void *)disp->driver);
    }

    indev_touchpad = indev_init(touch);
    MY_INFO("LVGL touch input initialized\n");
    
    vTaskDelay(100);
    screen_switch(true);
    set_brightness(cfg.brightness);
    MY_INFO("scr_lvgl_init() completed!\n");
}




#endif

