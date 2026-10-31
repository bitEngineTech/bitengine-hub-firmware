idf_component_register(
    SRCS "thread_br.c"
         "thread_br_channel.c"   # 信道共存纯逻辑，主机侧可单独编译（test_host/）
         "thread_radio_mgr.c"
         "srp_readvertise.c"
         "thread_br_diag.c"      # 诊断构建专用（CONFIG_BITENGINE_DIAG_THREAD_PENDING / _SRP_INJECT），出厂编出来是空的
         "zb_sniff.c"            # 诊断构建专用（CONFIG_BITENGINE_DIAG_ZB_SNIFF），出厂编出来是空的
         "srp_mdns_wrap.c"       # 诊断构建专用（CONFIG_BITENGINE_DIAG_SRP_INJECT），出厂编出来是空的
         "ot_cpu_guard.c"        # OT 任务过载时让出 CPU（下面的 --wrap=otPlatCryptoEcdsaVerify）
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}"   # psram_task.h
    REQUIRES config_mgr esp_event network_mgr led_ctrl
    PRIV_REQUIRES freertos ieee802154 openthread vfs mdns_svc mdns esp_netif esp_coex esp_timer
                  ezsp_bridge   # 建网时要读 NCP 实际信道，好躲开它（ncp_version.h）
                  esp_hal_ieee802154 soc   # ED 诊断计数器：hal/ieee802154_common_ll.h + soc/ieee802154_reg.h
)

# 诊断构建：给 SRP advertising proxy（预编译的 libopenthread_br.a）调的 mdns 写接口计时，
# 清单与 srp_mdns_wrap.c 里的 __wrap_ 一一对应（缺一个 = 链接时找不到 __wrap_X）。理由见那个文件的头注。
if(CONFIG_BITENGINE_DIAG_SRP_INJECT)
    foreach(fn mdns_delegate_hostname_add mdns_delegate_hostname_set_address mdns_delegate_hostname_remove
               mdns_service_add_for_host mdns_service_remove_for_host mdns_service_port_set_for_host
               mdns_service_txt_set_for_host mdns_service_subtype_update_multiple_items_for_host
               esp_ecdsa_transparent_verify_hash)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${fn}")
    endforeach()
endif()

# 出厂也要：每条 SRP 注册在 OT 任务里验一次签，截住它在过载时让出 CPU（ot_cpu_guard.c 头注）。
# ⚠️ 同一个符号只能 wrap 一次 —— 诊断构建的验签计时也在 ot_cpu_guard.c 里回调，别再加进上面的诊断清单。
if(CONFIG_OPENTHREAD_ENABLED)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=otPlatCryptoEcdsaVerify")
endif()
