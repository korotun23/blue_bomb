#define USER_SETUP_LOADED
 
#include "../config/User_setup.h"
#include "lv_conf.h"
 
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
 
#include "display_driver.h"
 
#define HOR_RES    240
#define VER_RES    320
#define BUF_PIXELS (HOR_RES * 40)
 
// LVGL rendering alligned buffer 
// lv_display_set_buffers() is waiting for a size in bytes
alignas(64) static uint8_t s_lv_buffer[BUF_PIXELS * sizeof(uint16_t)];
 
static TFT_eSPI tft;

static uint16_t s_caldata[5] = { 301, 3370, 317, 3461, 7 }; // To store touch calibration data
 
// LVLG Tickers in MS
static uint32_t ticker(void)
{
    return millis();
}
 
static void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
 
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors(reinterpret_cast<uint16_t *>(px_map), w * h, true);
    tft.endWrite();
 
    lv_display_flush_ready(display);
}
 
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (tft.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
 
void display_init()
{
    // TFT
    tft.begin();
    tft.setRotation(2);
    tft.invertDisplay(true);
    tft.fillScreen(TFT_BLACK);
 
    // Touch screen calibration
    // uint16_t calibration[5];
    //tft.calibrateTouch(calibration, TFT_MAGENTA, TFT_BLACK, 15);
    // Serial.printf("Touch calibration: %u, %u, %u, %u, %u\n",
    //              (unsigned)calibration[0], (unsigned)calibration[1],
    //              (unsigned)calibration[2], (unsigned)calibration[3],
    //              (unsigned)calibration[4]);

    tft.setTouch(s_caldata);
    
    // LVGL
    lv_init();
    lv_tick_set_cb(ticker);
 
    lv_display_t *display = lv_display_create(HOR_RES, VER_RES);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_lv_buffer, nullptr, sizeof(s_lv_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, display_flush);
 
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, touch_read);
}
 
void display_update()
{
    lv_timer_handler();
}