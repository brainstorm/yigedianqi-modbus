FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                                    ${CMAKE_SOURCE_DIR}/include
                       PRIV_REQUIRES ws2812_led app_insights app_reset app_wifi mdns esp_http_client esp-tls esp_netif nvs_flash esp_rainmaker rmaker_common esp_insights)
