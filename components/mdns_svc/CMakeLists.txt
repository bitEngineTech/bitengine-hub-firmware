idf_component_register(
    SRCS "mdns_svc.c" "mdns_load.c"
    INCLUDE_DIRS "include"
    REQUIRES config_mgr esp_event
    PRIV_REQUIRES mdns app_update lwip
)
