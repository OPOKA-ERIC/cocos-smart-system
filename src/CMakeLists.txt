idf_component_register(SRCS "main.c" "rc522.c"
                       INCLUDE_DIRS "../include"
                       REQUIRES driver nvs_flash esp_netif esp_event lwip fatfs sdmmc esp_timer)
