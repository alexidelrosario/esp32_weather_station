/******************************************************************************
 * weather_station_main.c
 
 * Main application for an ESP32-based IoT weather station built using ESP-IDF.
 *
 * Top-level application for the ESP32 weather station. Connects to WiFi,
 * reads local temperature from the SHTC3 sensor, fetches outdoor weather
 * from wttr.in, displays both on the LCD, and reports to a local
 * server via HTTP GET (location) and POST (sensor data).
 ******************************************************************************/

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "lwip/ip4_addr.h"
#include "LCD_Display.h"

// User config
#define WIFI_SSID               "wifiname"
#define WIFI_PASS               "wifipassword"
#define SERVER_IP               "xx.xx.xx.xx" 
#define SERVER_PORT             "1234"

#define SERVER_LOCATION_URL     "http://" SERVER_IP ":" SERVER_PORT "/location"
#define SERVER_POST_URL         "http://" SERVER_IP ":" SERVER_PORT "/weather"

#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define MAXIMUM_RETRY           10

static const char *TAG = "weather_station";

static EventGroupHandle_t wifi_event_group;
static int retry_count = 0;

static char http_response[256];
static int http_response_len = 0;

static char outdoor_temp[64];
static char weather_url[256];

static bool http_get_to_buffer(const char *url, char *out, int out_size);

/******************************************************************************
 * Utility functions for strong cleanup, HTTP buffer management, and system support. 
 ******************************************************************************/
static void clear_http_response(void)
{
    memset(http_response, 0, sizeof(http_response));
    http_response_len = 0;
}

static void trim_trailing_whitespace(char *str)
{
    int len = strlen(str);

    while (len > 0 &&
           (str[len - 1] == '\r' ||
            str[len - 1] == '\n' ||
            str[len - 1] == ' '  ||
            str[len - 1] == '\t')) {
        str[len - 1] = '\0';
        len--;
    }
}

static const char *wifi_reason_to_string(uint8_t reason)
{
    switch (reason) {
        case 201:
            return "NO_AP_FOUND: ESP32 cannot find that WiFi network name";
        case 202:
            return "AUTH_FAIL: wrong password or auth problem";
        case 203:
            return "ASSOC_FAIL: router rejected association";
        case 204:
            return "HANDSHAKE_TIMEOUT: password/security negotiation failed";
        default:
            return "Unknown/other WiFi reason";
    }
}

/******************************************************************************
 * WiFi connection management handles network scanning, connection setup and reconnection
 ******************************************************************************/
static void scan_wifi_networks(void)
{
    ESP_LOGI(TAG, "Scanning for WiFi networks...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    ESP_LOGI(TAG, "Found %d WiFi networks", ap_count);

    if (ap_count == 0) {
        ESP_LOGW(TAG, "No WiFi networks found.");
        return;
    }

    wifi_ap_record_t ap_records[20];
    uint16_t max_records = 20;

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&max_records, ap_records));

    for (int i = 0; i < max_records; i++) {
        ESP_LOGI(TAG,
                 "SSID: '%s' | RSSI: %d | Channel: %d | Authmode: %d",
                 (char *)ap_records[i].ssid,
                 ap_records[i].rssi,
                 ap_records[i].primary,
                 ap_records[i].authmode);
    }
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started.");
    }

    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *) event_data;

        ESP_LOGW(TAG,
                 "Disconnected from WiFi. Reason code: %d (%s)",
                 event->reason,
                 wifi_reason_to_string(event->reason));

        if (retry_count < MAXIMUM_RETRY) {
            retry_count++;
            ESP_LOGW(TAG,
                     "Retrying WiFi connection... attempt %d/%d",
                     retry_count,
                     MAXIMUM_RETRY);

            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    }

    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;

        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));

        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    scan_wifi_networks();

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    strncpy((char *)wifi_config.sta.ssid,
            WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);

    strncpy((char *)wifi_config.sta.password,
            WIFI_PASS,
            sizeof(wifi_config.sta.password) - 1);

    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Connecting to WiFi SSID: '%s'", WIFI_SSID);

    retry_count = 0;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to WiFi.");
        return true;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi after retries.");
        return false;
    }

    ESP_LOGE(TAG, "Timed out waiting for WiFi connection.");
    return false;
}


/******************************************************************************
 * HTTP communication helpers used for HTTP requests, process server responses, 
 * and manage network communication buffers 
 ******************************************************************************/
 static bool get_server_location(char *location, int location_size)
{
    bool success = http_get_to_buffer(
        SERVER_LOCATION_URL,
        location,
        location_size);

    if (!success) {
        ESP_LOGE(TAG, "Failed to get server location.");
        return false;
    }

    ESP_LOGI(TAG, "Server location: %s", location);
    return true;
}

static esp_err_t http_get_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0) {
                int space_left = sizeof(http_response) - http_response_len - 1;

                if (space_left > 0) {
                    int copy_len = evt->data_len;

                    if (copy_len > space_left) {
                        copy_len = space_left;
                    }

                    memcpy(http_response + http_response_len,
                           evt->data,
                           copy_len);

                    http_response_len += copy_len;
                    http_response[http_response_len] = '\0';
                }
            }
            break;

        default:
            break;
    }

    return ESP_OK;
}


static bool http_get_to_buffer(const char *url, char *out, int out_size)
{
    clear_http_response();

    ESP_LOGI(TAG, "HTTP GET: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_get_event_handler,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client.");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP GET status code: %d", status_code);
    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed. Status code: %d", status_code);
        return false;
    }

    trim_trailing_whitespace(http_response);
    strncpy(out, http_response, out_size - 1);
    out[out_size - 1] = '\0';

    return true;
}

/******************************************************************************
 * Sensor and peripheral interface initializes and reads local hardware peripherals, 
 * including the SHTC3 environmental sensor and I2C LCD display
 ******************************************************************************/
static void init_my_temperature_sensor(void)
{
    lcd_shtc3_init();
    ESP_LOGI(TAG, "SHTC3 temperature sensor initialized.");
}

static float read_my_temperature_c(void)
{
    shtc3_reading_t reading;
    esp_err_t ret = read_temperature_humidity(&reading);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Temp/humidity read failed: %s", esp_err_to_name(ret));
        return -999.0f;
    }

    return reading.temperature_c;
}

/******************************************************************************
 * Weather API and server communication. Retrieves outdoor temperature from wttr.in
 * and transmits combined local and remote temperature data to the Raspberry Pi server.
 ******************************************************************************/
static bool get_outdoor_temperature(
    const char *location,
    char *outdoor_temp,
    int outdoor_temp_size)
{
    static char encoded_location[128];
    int j = 0;
    for (int i = 0; location[i] != '\0' && j < sizeof(encoded_location) - 1; i++) {
        if (location[i] == ' ') {
            encoded_location[j++] = '+';
        } else {
            encoded_location[j++] = location[i];
        }
    }
    encoded_location[j] = '\0';

    static char weather_url[256];
    snprintf(weather_url, sizeof(weather_url), "http://wttr.in/%s?format=%%t&m", encoded_location); 

    bool success = http_get_to_buffer(
        weather_url,
        outdoor_temp,
        outdoor_temp_size);

    if (!success) {
        ESP_LOGE(TAG, "Failed to get outdoor temperature from wttr.in.");
        return false;
    }

    ESP_LOGI(TAG, "Outdoor temperature from wttr.in: %s", outdoor_temp);
    return true;
}

static void post_results_to_server(
    const char *location,
    const char *outdoor_temp,
    float esp32_temp_c)
{
    char post_data[256];

    // Format sensor and weather data before transmitting to local server
    snprintf(post_data,
             sizeof(post_data),
             "{"
             "\"location\":\"%s\","
             "\"outdoor_temp_c\":\"%s\","
             "\"esp32_temp_c\":%.2f"
             "}",
             location,
             outdoor_temp,
             esp32_temp_c);

    ESP_LOGI(TAG, "HTTP POST: %s", SERVER_POST_URL);
    ESP_LOGI(TAG, "POST body: %s", post_data);

    esp_http_client_config_t config = {
        .url = SERVER_POST_URL,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for POST.");
        return;
    }

    ESP_ERROR_CHECK(esp_http_client_set_method(client, HTTP_METHOD_POST));
    ESP_ERROR_CHECK(esp_http_client_set_header(client,
                                               "Content-Type",
                                               "application/json"));
    ESP_ERROR_CHECK(esp_http_client_set_post_field(client,
                                                   post_data,
                                                   strlen(post_data)));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "POST complete. HTTP status code: %d", status_code);
    } else {
        ESP_LOGE(TAG, "POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}


/******************************************************************************
 * Main function
 ******************************************************************************/
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
    init_my_temperature_sensor();
    bool wifi_connected = wifi_init_sta();

    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi failed. Stopping.");
        return;
    }

    // Repeatedly request server location, gety wttr.in for outdoor temperature, 
    // read local SHTC3 temperature sensor, update LCD display, send both readings to server
    while (1) {
        char location[64]; 
        memset(location, 0, sizeof(location));
        memset(outdoor_temp, 0, sizeof(outdoor_temp));

        bool got_location = get_server_location(location, sizeof(location));
        ESP_LOGI(TAG, "got_location: %d, location: %s", got_location, location);

        if (!got_location) {
            ESP_LOGE(TAG, "Skipping this cycle because location request failed.");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        bool got_outdoor_temp = get_outdoor_temperature(
            location,
            outdoor_temp,
            sizeof(outdoor_temp));

        if (!got_outdoor_temp) {
            ESP_LOGE(TAG, "Skipping this cycle because wttr.in request failed.");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        float esp32_temp_c = read_my_temperature_c();

        shtc3_reading_t reading;
        esp_err_t read_ret = read_temperature_humidity(&reading);

        if (read_ret == ESP_OK) {
            char line1[17];
            char line2[17];
            float outdoor_temp_f = atof(outdoor_temp);

            snprintf(line1, sizeof(line1), "   In:  %.1fC", esp32_temp_c);
            snprintf(line2, sizeof(line2), "Out: %.1fC", outdoor_temp_f);

            lcd_clear();
            vTaskDelay(pdMS_TO_TICKS(5));

            lcd_set_cursor(0, 0);
            lcd_print(line1);

            lcd_set_cursor(0, 1);
            lcd_print(line2);
        }
        
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "Server location: %s", location);
        ESP_LOGI(TAG, "Outdoor temperature from wttr.in: %s", outdoor_temp);
        ESP_LOGI(TAG, "ESP32/SHTC3 sensor temperature: %.2f C", esp32_temp_c);
        ESP_LOGI(TAG, "====================================");

        post_results_to_server(location, outdoor_temp, esp32_temp_c);

        ESP_LOGI(TAG, "Next update in 30 seconds...");
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}