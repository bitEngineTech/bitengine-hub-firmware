/* muse_hold —— 安全模式里"退出并先关掉 Muse"的一次性标记（docs/15 设计附录 E33，2026-10-23 用户定 B）。见 include/muse_hold.h。
 *
 * 为什么不直接写 hubdb 的 muse.en：安全模式下 hub_store 不起（设计 2.6 #8），也不该为这件事起它 —— 崩溃的根源
 * 可能就在存储那一侧。标记写在主 nvs（安全模式下 config_mgr 照常在用），只是一条"下次开机请关掉 Muse"的留言；
 * Muse 开关的正本仍只有 hubdb 的 muse.en：下次正常开机由 muse_hold_apply() 经 muse_cfg 写进去，写成了才清标记。
 *
 * 命名空间 muse_hold 不在 config_mgr 的 OWNED_NS 里：恢复出厂留下它也无害 —— 恢复出厂清掉了整个 hubdb muse，
 * muse.en 本来就是关；下次开机 apply 再写一次"关"并清掉标记。
 */
#include "muse_hold.h"

#include "esp_log.h"
#include "hub_store.h"
#include "muse_cfg.h"
#include "nvs.h"

static const char *TAG = "muse_hold";

#define HOLD_NS  "muse_hold"
#define HOLD_KEY "off"
#define HOLD_FLUSH_MS 2000    /* hub_store 平时几毫秒；开机这一步等得起 */

esp_err_t muse_hold_request(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(HOLD_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打不开 nvs（%s），没记下\"关掉 Muse\"", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, HOLD_KEY, 1);
    if (err == ESP_OK) err = nvs_commit(h);    /* 调用方紧接着重启：必须当场落盘，不走 config_mgr 的防抖 */
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写不进 nvs（%s），没记下\"关掉 Muse\"", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "已记下：下次开机先关掉 Muse（安全模式退出时用户要求）");
    return ESP_OK;
}

bool muse_hold_pending(void)
{
    nvs_handle_t h;
    if (nvs_open(HOLD_NS, NVS_READONLY, &h) != ESP_OK) return false;   /* 命名空间不存在 = 从没记过 */
    uint8_t v = 0;
    const bool on = nvs_get_u8(h, HOLD_KEY, &v) == ESP_OK && v != 0;
    nvs_close(h);
    return on;
}

esp_err_t muse_hold_apply(void)
{
    /* 先只读地看：READWRITE 打开不存在的命名空间会把它建出来，等于每次开机都往 flash 写一笔 */
    nvs_handle_t h;
    esp_err_t err = nvs_open(HOLD_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;                    /* 从没记过 */
    if (err != ESP_OK) {
        /* 读不出有没有留言：宁可这次不起 Muse，也不冒"用户要关、我们照开"的险 */
        ESP_LOGE(TAG, "打不开 nvs（%s），不知道有没有\"关掉 Muse\"的留言 —— 这次开机不起 Muse", esp_err_to_name(err));
        return err;
    }
    uint8_t v = 0;
    err = nvs_get_u8(h, HOLD_KEY, &v);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND || (err == ESP_OK && v == 0)) return ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读不出留言（%s）—— 这次开机不起 Muse", esp_err_to_name(err));
        return err;
    }

    /* 有留言：先把正本（hubdb muse.en）写成关并落盘（hub_store_put 只是排队、2 s 防抖），落盘了才清留言；
       写不成留言保留，下次开机再试 */
    err = muse_cfg_set_enabled(false);
    if (err == ESP_OK) err = hub_store_flush(HOLD_FLUSH_MS);
    /* flush 回 OK 不等于这一条写进去了（排队时被拒只打日志，审计 ⚠️）：读回确认已是关，才算数 */
    bool en = true, paired = false;
    if (err == ESP_OK) err = muse_cfg_load_flags(&en, &paired);
    if (err == ESP_OK && en) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "关不掉 Muse（%s）—— 留言保留，这次开机不起 Muse", esp_err_to_name(err));
        return err;
    }
    err = nvs_open(HOLD_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, HOLD_KEY);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* Muse 已经关了（正本落了盘），留言清不掉只是下次开机再写一次"关"—— 仍然安全 */
        ESP_LOGW(TAG, "Muse 已关，但留言清不掉（%s），下次开机会再关一次", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "按安全模式退出时的要求关掉了 Muse —— 查明原因后在 Muse 页重新打开");
    return ESP_OK;
}

void muse_hold_clear(void)
{
    if (!muse_hold_pending()) return;                    /* 没有就别 READWRITE 打开（会把命名空间建出来） */
    nvs_handle_t h;
    esp_err_t err = nvs_open(HOLD_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, HOLD_KEY);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) ESP_LOGW(TAG, "用户在 Muse 页打开了 Muse，作废安全模式退出时留的\"关掉 Muse\"");
    else ESP_LOGE(TAG, "作废留言失败（%s）—— 下次开机 Muse 仍会被关掉", esp_err_to_name(err));
}
