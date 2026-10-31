idf_component_register(
    SRCS "matter_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES ezsp_bridge esp_event
    PRIV_REQUIRES freertos
)
