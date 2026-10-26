idf_component_register(SRCS "main.c" "mqtt_bus.c" "rc522.c"
                       INCLUDE_DIRS "../include"
                       REQUIRES driver esp_wifi nvs_flash esp_netif esp_event lwip fatfs sdmmc)
