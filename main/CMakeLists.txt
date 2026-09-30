idf_component_register(
    SRCS "main.c" "app_config.c" "proto.c" "uart_bridge.c" "net_bridge.c" "console_cmd.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event nvs_flash lwip console esp_timer esp_app_format
)
