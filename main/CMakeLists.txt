idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES  nvs_flash driver lwip esp_event esp_netif wifi_manager mqtt_app esp_wifi mqtt mdns esp_adc
)
