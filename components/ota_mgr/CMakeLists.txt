idf_component_register(
    SRCS "ota_mg24.c"
         "mg24_bl_entry.c"
         "gbl_image.c"
         "gbl_crypto_psa.c"
         "ncp_unlock.c"
         "xmodem.c"
         "uart_bench.c"
         "mg24_image_record.c"
         "www_image.c"
    INCLUDE_DIRS "include"
    # uart_bridge.h is not exported by ezsp_bridge; the OTA path needs its
    # baud-switch and flush entry points, so pull the header in directly.
    PRIV_INCLUDE_DIRS "../ezsp_bridge" "${CMAKE_SOURCE_DIR}"
    REQUIRES ezsp_bridge watchdog
    PRIV_REQUIRES freertos esp_driver_uart esp_timer esp_event button mbedtls nvs_flash esp_partition
)

# The MG24 image signing public key is embedded from its PEM at build time.
# mg24/release/mg24_gbl_signing_key.pub is the only copy that is edited; the
# C array is regenerated whenever it (or the generator) changes, so the
# firmware can never carry a stale or hand-typed key.
#
# Everything below idf_component_register() is skipped while ESP-IDF expands
# component requirements in script mode, where add_custom_command is not
# allowed — which is exactly why it lives down here.
idf_build_get_property(_python PYTHON)
set(_mg24_pubkey_pem "${CMAKE_SOURCE_DIR}/mg24/release/mg24_gbl_signing_key.pub")
set(_mg24_pubkey_gen "${CMAKE_SOURCE_DIR}/tools/gen_mg24_pubkey.py")
set(_mg24_pubkey_c   "${CMAKE_CURRENT_BINARY_DIR}/mg24_gbl_pubkey.c")
add_custom_command(
    OUTPUT  "${_mg24_pubkey_c}"
    COMMAND "${_python}" "${_mg24_pubkey_gen}" "${_mg24_pubkey_pem}" "${_mg24_pubkey_c}"
    DEPENDS "${_mg24_pubkey_pem}" "${_mg24_pubkey_gen}"
    COMMENT "Embedding MG24 GBL signing public key from mg24/release/mg24_gbl_signing_key.pub"
    VERBATIM
)
set_property(SOURCE "${_mg24_pubkey_c}" PROPERTY GENERATED 1)
target_sources(${COMPONENT_LIB} PRIVATE "${_mg24_pubkey_c}")
