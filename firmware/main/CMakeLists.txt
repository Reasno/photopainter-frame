idf_component_register(
    SRCS "main.c" "board.c" "epd.c" "pmu.c" "net.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_wifi esp_netif esp_event esp_http_client nvs_flash esp_timer
)
