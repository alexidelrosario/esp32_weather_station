/******************************************************************************
 * LCD_Display.h
 *
 * Driver for the 16x2 I2C LCD, RGB backlight controller, and SHTC3
 * temperature/humidity sensor used by the ESP32 weather station.
 *
 * All three devices share the same I2C bus. This file handles I2C bus setup,
 * device registration, LCD command/data writes, cursor control, backlight color
 * control, and SHTC3 temperature/humidity measurements.
 ******************************************************************************/

#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c.h"

#define I2C_PORT_NUM        0
#define I2C_SDA_PIN         7
#define I2C_SCL_PIN         8
#define I2C_SCL_SPEED       100000
#define I2C_GLITCH_IGNORE   7 // any pulse shorter than 7 clk cycle is noise/ignored

#define SHTC3_ADDR          0x70
#define LCD_ADDR            0x3E
#define RGB_ADDR            0x2D

typedef struct {
    float temperature_c;
    float temperature_f;
    float humidity;
} shtc3_reading_t;

void lcd_shtc3_init(void);

esp_err_t read_temperature_humidity(shtc3_reading_t *reading);

void lcd_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void lcd_set_cursor(uint8_t col, uint8_t row);
void lcd_clear(void);
void lcd_print(const char *str);

#endif