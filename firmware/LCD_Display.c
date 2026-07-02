/******************************************************************************
 * LCD_Display.c
 *
 * Driver for the 16x2 I2C LCD, RGB backlight controller, and SHTC3
 * temperature/humidity sensor used by the ESP32 weather station.
 *
 * All three devices share the same I2C bus. This file handles I2C bus setup,
 * device registration, LCD command/data writes, cursor control, backlight color
 * control, and SHTC3 temperature/humidity measurements.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_err.h"

#include "LCD_Display.h"

static const i2c_port_num_t i2c_port = 0;
static const gpio_num_t i2c_sda_pin = GPIO_NUM_7;
static const gpio_num_t i2c_scl_pin = GPIO_NUM_8;
static const uint8_t i2c_glitch_ignore_cnt = 7;
static const uint32_t i2c_scl_speed = 100000;

static const uint8_t shtc3_wake_cmd[2] = {0x35, 0x17};
static const uint8_t shtc3_measure_cmd[2] = {0x78, 0x66};
static const uint8_t shtc3_sleep_cmd[2] = {0xB0, 0x98};

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t shtc3_dev;
static i2c_master_dev_handle_t lcd_dev;
static i2c_master_dev_handle_t rgb_dev;

/******************************************************************************
 * LCD and RGB helper functions. 
 * 
 * Sends commands to the LCD controller and writes color values to the RGB
 * backlight and controller over I2C
 ******************************************************************************/
static void lcd_send_cmd(uint8_t cmd)
{
    uint8_t buf[2] = {0x80, cmd};
    i2c_master_transmit(lcd_dev, buf, 2, 100);
}

static void lcd_send_data(uint8_t data)
{
    uint8_t buf[2] = {0x40, data};
    i2c_master_transmit(lcd_dev, buf, 2, 100);
}

static void rgb_set_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_master_transmit(rgb_dev, buf, 2, 100);
}

/******************************************************************************
 * Configures the 16x2 LCD display mode, clears the display, and inits the RGB backlight
 ******************************************************************************/
static void lcd_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t show_function = 0x08 | 0x00 | 0x00;
    show_function |= 0x08;

    lcd_send_cmd(0x20 | show_function);
    vTaskDelay(pdMS_TO_TICKS(5));

    lcd_send_cmd(0x20 | show_function);
    vTaskDelay(pdMS_TO_TICKS(5));

    lcd_send_cmd(0x20 | show_function);

    // Display on, cursor off, blink off
    lcd_send_cmd(0x08 | 0x04);

    // Clear display
    lcd_send_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));

    // Entry mode: left-to-right, no shift
    lcd_send_cmd(0x04 | 0x02);

    // Default backlight: white
    rgb_set_reg(0x01, 0xFF);
    rgb_set_reg(0x02, 0xFF);
    rgb_set_reg(0x03, 0xFF);
}

/******************************************************************************
 * Set up I2C bus, LCD, RGB backlight, temp/humidity sensor
 ******************************************************************************/
void lcd_shtc3_init(void)
{
    esp_err_t ret;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = i2c_port,
        .sda_io_num = i2c_sda_pin,
        .scl_io_num = i2c_scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = i2c_glitch_ignore_cnt,
        .flags.enable_internal_pullup = true,
    };

    ret = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (ret != ESP_OK) {
        printf("i2c bus init failed\n");
        abort();
    }

    i2c_device_config_t lcd_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LCD_ADDR,
        .scl_speed_hz = i2c_scl_speed,
    };

    ret = i2c_master_bus_add_device(i2c_bus, &lcd_dev_config, &lcd_dev);
    if (ret != ESP_OK) {
        printf("lcd device add failed\n");
        abort();
    }

    i2c_device_config_t rgb_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RGB_ADDR,
        .scl_speed_hz = i2c_scl_speed,
    };

    ret = i2c_master_bus_add_device(i2c_bus, &rgb_dev_config, &rgb_dev);
    if (ret != ESP_OK) {
        printf("rgb device add failed\n");
        abort();
    }

    i2c_device_config_t shtc3_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = i2c_scl_speed,
    };

    ret = i2c_master_bus_add_device(i2c_bus, &shtc3_dev_config, &shtc3_dev);
    if (ret != ESP_OK) {
        printf("shtc3 device add failed\n");
        abort();
    }

    lcd_init();
    lcd_set_rgb(255, 50, 50);  // pink backlight
}

/******************************************************************************
 * LCD functions used by the main application to set backlight, move the 
 * cursor, clear the display, and print text
 ******************************************************************************/
void lcd_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    rgb_set_reg(0x01, r);
    rgb_set_reg(0x02, g);
    rgb_set_reg(0x03, b);
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    uint8_t addr = (row == 0) ? (col | 0x80) : (col | 0xC0);
    uint8_t buf[2] = {0x80, addr};
    i2c_master_transmit(lcd_dev, buf, 2, 100);
}

void lcd_clear(void)
{
    lcd_send_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));
}

void lcd_print(const char *str)
{
    while (*str) {
        lcd_send_data((uint8_t)*str);
        vTaskDelay(pdMS_TO_TICKS(1));
        str++;
    }
}

/******************************************************************************
 * Wakes the SHTC3 sensor, starts a temperature and humidity measurement, 
 * read the raw data and convert the units, and puts sensor to sleep 
 ******************************************************************************/
esp_err_t read_temperature_humidity(shtc3_reading_t *reading)
{
    esp_err_t ret;
    uint8_t data[6];

    ret = i2c_master_transmit(shtc3_dev, shtc3_wake_cmd, 2, -1);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(25));

    ret = i2c_master_transmit(shtc3_dev, shtc3_measure_cmd, 2, -1);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));

    ret = i2c_master_receive(shtc3_dev, data, 6, -1);
    if (ret != ESP_OK) return ret;

    uint16_t temperature_raw = (data[0] << 8) | data[1];
    uint16_t raw_humidity = (data[3] << 8) | data[4];

    reading->temperature_c = -45.0f + 175.0f * ((float)temperature_raw / 65536.0f);
    reading->temperature_f = reading->temperature_c * (9.0f / 5.0f) + 32.0f;
    reading->humidity = 100.0f * ((float)raw_humidity / 65536.0f);

    i2c_master_transmit(shtc3_dev, shtc3_sleep_cmd, 2, -1);

    return ESP_OK;
}