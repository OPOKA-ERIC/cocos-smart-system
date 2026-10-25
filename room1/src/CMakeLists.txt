FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/include
                       REQUIRES driver gpio esp_wifi esp_event mqtt json nvs_flash)
