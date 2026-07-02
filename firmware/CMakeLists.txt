idf_component_register(
    SRCS "main.c" "LCD_Display.c" 
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_client
        esp_driver_gpio
        esp_driver_i2c
        driver
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)

