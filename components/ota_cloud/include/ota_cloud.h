/* ota_cloud — 云端固件更新（第一步：查更新 + 用户点一下装）
 *
 * 形态刻意做小：设备去一个固定 URL 拉一份 JSON manifest，比版本号，新的话
 * 让用户点一下装。**不做**自动推送、不做灰度、不做设备身份。
 * 理由见 docs/guide/云端OTA设计.md 第一节 —— 卖硬件不需要车队管理系统，
 * 而"查更新+装"这件事不需要设备身份（固件本身有签名，谁下都一样）。
 *
 * ⚠️ 签名验证不在本组件里，也不需要在这里做：
 * esp_https_ota() 内部走 esp_ota_end()，验签在那里发生
 * （esp_https_ota.c:924 → esp_ota_end → CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT）。
 * 本组件一行验签代码都没有是**正确的**，别照着"没看到验签"去补。
 * 见 docs/guide/固件签名与密钥保管.md。
 *
 * ⚠️ manifest 是**不可信输入**。它来自网络，即使 HTTPS 也只证明"连对了服务器"，
 * 不证明服务器没被攻破。所以：URL 必须是 https、所有字符串定长截断、
 * 版本必须严格大于当前（防降级 —— 签名防不住"推一个旧的但签名合法的固件"）。
 */
#pragma once
#include <stddef.h>

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单一状态机。UP_TO_DATE / UPDATE_AVAILABLE 是 check 的两种成功结局，
   分开而不是用一个 bool，是为了让 /api/v1/ota/check 能把"查过了，已是最新"
   和"还没查过"区分开 —— 前端要据此决定显不显示提示条。 */
typedef enum {
    OTA_CLOUD_IDLE = 0,          /* 从没查过 */
    OTA_CLOUD_CHECKING,          /* 正在拉 manifest */
    OTA_CLOUD_UP_TO_DATE,        /* 查过了，当前已是最新 */
    OTA_CLOUD_UPDATE_AVAILABLE,  /* 查过了，有新版本，latest 有效 */
    OTA_CLOUD_DOWNLOADING,       /* 正在下载安装 */
    OTA_CLOUD_SUCCESS,           /* 装好了，等重启 */
    OTA_CLOUD_FAILED,            /* 失败，last_error() 有原因 */
} ota_cloud_state_t;

/* manifest 解析结果。所有字符串都是定长且已截断，可直接进日志；
   进 JSON 回包前仍需转义（url/notes 来自网络）。 */
typedef struct {
    char     version[48];   /* 如 "v0.6.0-3-gabcdef0" */
    char     url[256];      /* 固件下载地址，已校验必须 https:// 开头 */
    char     notes[160];    /* 一行更新说明，可空 */
    uint32_t size;          /* 字节数，0 = manifest 未提供 */
    bool     valid;         /* 上面几项是否可用 */

    /* 前端分区（www.bin）。manifest 可不带；带了就与 app 成对下发（2026-09-10）。
       前端没有签名机制，靠 HTTPS + sha256；版本串与 /www/version.json 的 version 逐字比较。 */
    bool     has_www;
    char     www_version[40];
    char     www_url[256];  /* 已校验必须 https:// 开头 */
    char     www_sha256[65];/* 64 位十六进制 + NUL，已校验形状 */
    uint32_t www_size;      /* 必须等于 www 分区大小，install 前再查一次 */

    /* Zigbee 芯片（MG24 NCP）固件 .gbl。可缺；带了就四项必须合法。**单独安装，不与 app 捆绑**
       （刷写要断 Zigbee 约 15-30 秒，时机由用户挑）。HTTPS + sha256 保完整性；真实性由
       ota_mgr 在 C5 侧验签（T1.2，云端来源必须带签名）。 */
    bool     has_ncp;
    char     ncp_version[40];   /* 指纹与构建串都缺时与 ncp_version_format() 逐字比较，如 "8.2.2.0 build 532" */
    char     ncp_url[256];      /* 已校验必须 https:// 开头 */
    char     ncp_sha256[65];    /* 与 mg24_image_record 里本网关刷过的镜像指纹比较 */
    char     ncp_build[96];     /* XNCP 构建串（可选，T9.19 ⑦）：没有指纹时与 NCP 报的 build_string 比；老清单没有 = "" */
    uint32_t ncp_size;          /* [MG24_OTA_MIN_SIZE, MG24_OTA_MAX_SIZE] */

    /* check() 算好的决定，install() 用此刻的运行版本重算一次再执行 */
    bool     app_newer;     /* manifest 的 app 严格新于当前 → 装 app */
    bool     www_differs;   /* 带 www 且版本串与运行中不同（且 app 不比当前旧）→ 换前端 */
    bool     ncp_differs;   /* 带 ncp 且与本机不同 → 提示可更新 Zigbee 芯片（判据见 version_cmp.h） */
} ota_cloud_manifest_t;

/* NCP 那条路的小状态机，与上面的 7 态独立：下载 → 交给 ota_mg24（进度在 /ota/zigbee/status）。 */
typedef enum {
    OTA_CLOUD_NCP_IDLE = 0,
    OTA_CLOUD_NCP_DOWNLOADING,   /* 正在把 .gbl 下到 PSRAM 并核 sha256 */
    OTA_CLOUD_NCP_FLASHING,      /* 已交给 ota_mg24，ota_mg24_busy() 为假后自动回 IDLE */
    OTA_CLOUD_NCP_FAILED,        /* ncp_last_error() 有原因 */
} ota_cloud_ncp_state_t;

/** 初始化。只建互斥量，不联网。 */
esp_err_t ota_cloud_init(void);

/** 拉 manifest 并与当前固件比较。**同步阻塞**，最长约
 *  CONFIG_BITENGINE_OTA_CLOUD_TIMEOUT_MS。调用方通常是 httpd 任务，
 *  期间整个 Web 界面会卡住 —— 这是刻意的取舍：用户主动点"检查更新"时
 *  等一下可以接受，换成异步要多一套轮询状态机，不值得。
 *
 *  @return ESP_OK 查询成功（结果看 get_state()）；
 *          ESP_ERR_INVALID_STATE 正在下载中；
 *          ESP_ERR_NOT_SUPPORTED 未配置 manifest URL；
 *          其他 = 网络或解析失败，last_error() 有细节 */
esp_err_t ota_cloud_check(void);

/** 安装 get_latest() 指向的那个版本。**立即返回**，实际下载在后台任务里，
 *  调用方轮询 get_state()/progress()。装完不自动重启，由调用方决定时机。
 *
 *  @return ESP_OK 已受理；ESP_ERR_INVALID_STATE 没有可装的版本或已有 OTA 在跑 */
esp_err_t ota_cloud_install(void);

ota_cloud_state_t ota_cloud_get_state(void);

/** 最近一次 check 的结果。永不返回 NULL；未查过时 ->valid 为 false。 */
const ota_cloud_manifest_t *ota_cloud_get_latest(void);

/** 下载中返回 0-100（按当前阶段计）；未知或非下载态返回 -1。 */
int ota_cloud_progress(void);

/** 当前阶段："www"（下载前端）/ "app"（下载安装固件）/ "write"（写前端分区）/ ""。
 *  只指向静态字符串，可直接进回包。 */
const char *ota_cloud_phase(void);

/** webui 挂好 SPIFFS 后把运行中的前端版本告诉本组件（/www/version.json 的 version）。
 *  不调用或传空 = 未知，此时 manifest 里任何 www 版本都算"不同"。 */
void ota_cloud_set_running_www_version(const char *v);

/** 是否有云 OTA 正在进行（含 NCP 下载中）。其余 OTA 入口（www/esp32/zigbee）必须先查这个，
 *  否则两路同时写 flash。与 ota_mg24_busy() 同类。 */
bool ota_cloud_busy(void);

/* 防串刷（docs/15 设计 2.7、架构 5.8 R-9）：整个 app 镜像（或至少开头 112 字节）已在内存里时，
 * 比对它的工程名与正在运行的固件。一致返回 NULL；否则返回给 REST 用的短标识
 * （"profile_mismatch" / "profile_unknown" / "image_invalid"）并打一条 E 级日志。
 * 网页上传路径在 esp_ota_begin() 之前调；云端路径在 ota_cloud.c 里用镜像描述自己比。 */
const char *ota_cloud_profile_check_image(const void *img, size_t len);

/** 安装 get_latest() 里的 Zigbee 芯片固件：下到 PSRAM、核 sha256、交给 ota_mg24（与 Web 上传同一条
 *  XMODEM 路径）。**立即返回**；先轮询 ncp_state()，离开 DOWNLOADING 后改轮询 ota_mg24_get_status()。
 *  @param force 指纹相同也刷（产线 / 售后重刷用）
 *  @return ESP_OK 已受理；ESP_ERR_INVALID_STATE 没有可装的 NCP 固件、指纹相同且未 force、或已有 OTA 在跑 */
esp_err_t ota_cloud_install_ncp(bool force);
ota_cloud_ncp_state_t ota_cloud_ncp_state(void);
/** 最近一次 NCP 下载失败原因，短字符串；永不返回 NULL。 */
const char *ota_cloud_ncp_last_error(void);

/** 最近一次失败原因，短字符串常量或定长缓冲；永不返回 NULL。 */
const char *ota_cloud_last_error(void);

/** 版本号比较：>0 表示 a 比 b 新，0 相等，<0 更旧。
 *  解析 "v<major>.<minor>.<patch>-<commits>-g<sha>"（git describe 格式），
 *  按 (major, minor, patch, commits) 四元组排序。缺失的段按 0 处理。
 *  单独导出是为了能在主机侧测（test_host/），这类纯函数最容易写错。 */
int ota_cloud_version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
