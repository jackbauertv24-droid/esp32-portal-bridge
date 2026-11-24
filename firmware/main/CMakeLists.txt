file(GLOB APP_SRCS "*.c")
file(GLOB CORE_SRCS "../core/*.c")
idf_component_register(
  SRCS ${APP_SRCS} ${CORE_SRCS}
  EMBED_TXTFILES "www/index.html"
  INCLUDE_DIRS "." "../core"
  PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client esp-tls mqtt json
                spiffs app_update esp_driver_i2c esp_driver_uart esp_driver_gpio mbedtls esp_timer lwip
                espcoredump esp_partition fatfs sdmmc esp_driver_sdmmc esp_driver_tsens esp_psram spi_flash)
# snprintf into fixed buffers is used deliberately for bounded copies.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
# Keep the lwIP route hook even though only lwIP references it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u pb_route_hook" "-u pb_input_hook")
