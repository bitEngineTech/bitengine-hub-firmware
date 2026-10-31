#pragma once
/* muse_link —— Muse 直连会话（docs/15_muse_direct_design_v1.md 4.1、4.2、4.6；只在 BITENGINE_ENABLE_MUSE_LINK 下编）。
 *
 * 一个常驻任务 muse_sess（PSRAM 栈 12 KB，优先级 4）独自驱动会话状态机（muse_link_core）：取 VM（vm_api）、刷新令牌、
 * 跑会话（muse_session：TLS + WS + Noise）、注册、接调用、发心跳。别的任务只经下面的接口投事件、读状态快照，
 * 不阻塞（S2）；网络与内存事件由本组件自己订阅。凭据落盘一律经 hub_store（muse_cfg），本任务不碰 flash（PSRAM 栈规则 1）。
 *
 * 失败只影响 Muse 自己（N7）：不重启网关、不阻塞事件循环、不占 httpd。 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "muse_link_core.h"

typedef struct {
    muse_state_t state;
    uint32_t     state_since_s;    /* 进入当前状态时的开机秒数 */
    char         last_error[96];   /* 原因串（mlc_reason_str）；注册被拒时后面跟服务端给的原因（已净化） */
    char         node_id[24];
    char         display_name[33];
    char         vm_name[48];      /* 当前连着的 VM 的名字（已净化） */
    uint32_t     invokes_total, invokes_denied, invokes_failed;
    uint32_t     registrations;    /* link.register 成功次数：test_api 用它证明"改了设置真的重新注册了"（14.5） */
    uint8_t      reg_backends;     /* 最近一次注册用的后端就绪位（hub_registry.h 的 HUB_BACKEND_*；test_api 证明 zhub 起来后命令表真的带上了 hub.device.update） */
    bool         enabled, paired, sdk_token_set;
    bool         heartbeat;        /* 服务端回了 status=registered，会发 24 h 心跳 */
    uint32_t     stack_min_free;   /* 会话任务栈用得最深时还剩的字节数（上板确认 12 KB 够不够） */
    uint32_t     pushes_sent;      /* 送达的推送（4.9；test_api 用它证明 notify 关掉后不推，14.5） */
    uint32_t     pushes_dropped;   /* 队列满 / 重复 / 过期 / 被服务端拒 / 管线入口溢出 */
    uint32_t     pushes_queued;    /* 眼下排着的 */
} muse_status_t;

/* 读 hubdb、算身份、订阅网络 / 内存事件。在 hub_store 起来之后、安全模式出口之后调。 */
esp_err_t muse_link_init(void);
/* 建会话任务（PSRAM 栈；分配失败返回 ESP_ERR_NO_MEM，不退回内部内存）。 */
esp_err_t muse_link_start(void);
/* 状态快照（只读缓存，httpd 里直接调）。 */
esp_err_t muse_link_get_status(muse_status_t *out);

/* 下面这些不等会话任务，真正的动作在会话任务里做；没 init 或任务没起来（start 失败）返回 ESP_ERR_INVALID_STATE。
   set_enabled / unpair_local 投事件，队列满返回 ESP_ERR_TIMEOUT（开关有 10 s 一次的 resync 兜底）；reregister / config_changed
   走标志、丢不掉。config_changed 还会顺手读两次存储刷新状态快照（最长约 1 s，httpd 里调可以接受）。 */
esp_err_t muse_link_set_enabled(bool en);   /* 调用方已经 muse_cfg_set_enabled() 落了盘 */
esp_err_t muse_link_unpair_local(void);     /* 本地解绑（4.10）：断会话、删 acc_tok / ref_tok / vm_id */
esp_err_t muse_link_reregister(void);       /* 权限位或显示名变了，命令表 / 注册参数要重发（4.7） */
esp_err_t muse_link_config_changed(void);   /* SDK token / API 地址变了：会话任务重新喂给 vm_api */
/* 网页上的"发一条测试推送"（10.1）：在线时排一条 GATEWAY 测试消息（同所有推送：不合并、不去重、不限配额 —— 每点一次发一条）。
   main_chat：true 推进主会话（真推送也一律进主会话，muse_link.c 的 MUSE_PUSH_MAIN_CHAT）、false 推进侧边会话（chat_sid，诊断用 ——
   2026-10-23 上板对照：侧边会话在 App 里找不到）。
   任务没起来 ESP_ERR_INVALID_STATE；不在线 ESP_ERR_NOT_ALLOWED。 */
esp_err_t muse_link_test_push(bool main_chat);
