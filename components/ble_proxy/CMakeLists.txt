idf_component_register(
    SRCS "ble_proxy.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt_client esp_event
    PRIV_REQUIRES bt freertos esp_timer
)
