idf_component_register(
    SRCS "spectrum_scan.c"
         "beacon_scan.c"
         "channel_recommend.c"
         "spectrum_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES thread_br webhook esp_event config_mgr
    PRIV_REQUIRES ieee802154 esp_timer freertos
)
