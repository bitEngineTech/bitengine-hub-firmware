/* mc_iv —— Matter 设备的访谈（docs/15 设计 9.4 图末段"读 Basic Information、Descriptor → 映射能力"；M7 第四步）。
 *
 * 一次一台、在 CHIP 任务里：CASE（commissioner 的 GetConnectedDevice）→ 一个读请求：
 *   根节点的 Basic Information（VendorName / VendorID / ProductName / ProductID / NodeLabel）、
 *   全部端点的 Descriptor（DeviceTypeList / ServerList）、全部端点 PowerSource 的 FeatureMap（BAT 位 = 电池供电）。
 * 列表可能分块到：外面套 CHIP 的 BufferedReadCallback 拼回整条再给我们（同 esp-matter 的 subscribe_command）。
 * 读完（OnDone 且没出过错）置 DONE、经钩子落盘（hubdb mctrl/iv<节点号>）、通知 hub_core；失败按 30 s → 2 min → 10 min 退避重试，
 * 每次开机最多 MC_IV_TRIES 次（之后要等下次开机 / 重新配网）。整个一次访谈有 MC_IV_TIMEOUT_MS 的兜底计时器（CASE 连不上、
 * 睡眠设备迟迟不醒时 CHIP 自己也会报错，这只是防它不报）。
 *
 * H1（connectedhomeip 随 esp_matter 1.6.0~1）：
 *  - AttributePathParams(ClusterId, AttributeId) = 通配端点；读请求的路径在 SendRequest 里就编进报文（esp_matter read::send_request），
 *    数组不必活到 OnDone。ReadClient 由 esp-matter 的 client_deleter 在 OnDone 时删；我们的回调对象是静态的。
 *  - BufferedReadCallback 把列表拼好再调 OnAttributeData（TLV 读写头指在值上）；DeviceTypeList 的元素是结构体，
 *    上下文标签 0 = deviceType（u32）、1 = revision；ServerList 的元素是簇 ID（u32）。字符串是 UTF8String（GetDataPtr + GetLength）。
 *  - 簇 / 属性 ID：BasicInformation 0x0028（1 VendorName、2 VendorID、3 ProductName、4 ProductID、5 NodeLabel）、Descriptor 0x001D
 *    （0 DeviceTypeList、1 ServerList）、PowerSource 0x002F、FeatureMap 0xFFFC（PowerSource 的 BAT = bit 1）、IcdManagement 0x0046。 */
#include "mc_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <new>

#include <app/BufferedReadCallback.h>
#include <app/ReadClient.h>
#include <esp_matter_client.h>
#include <esp_matter_controller_client.h>

#include "mc_iv_core.h"

static const char *TAG = "mc_iv";

#define MC_IV_TRIES       4
#define MC_IV_TIMEOUT_MS  120000          /* 睡眠设备（ICD）要等它轮询，比常电的慢得多 */
#define MC_IV_TICK_MS     5000

using esp_matter::controller::matter_controller_client;

/* 每台一格（PSRAM：CONFIG_BITENGINE_MATTER_MAX_NODES × 约 400 B）。只在 CHIP 任务里写；别的任务经 s_lock 拷 */
typedef struct {
    bool     used;
    bool     queued;
    bool     added;                       /* 这次访谈是配网刚完成时排的：做完了推"加好了"（EV_ADDED）。开机补做的、
                                             记录读不回来重做的都不算 —— 否则升级 / hubdb 读失败一次，Muse 就收到一遍"新设备" */
    uint8_t  tries;
    int64_t  next_ms;                     /* 下次可以试的时刻（退避） */
    mc_iv_t  iv;
} iv_slot_t;

static iv_slot_t *s_iv;
static bool s_reading;                    /* 读请求在途（send_request 成功 → OnDone）：旧的没 OnDone 之前不开下一台 ——
                                             回调对象是静态共用的，两个 ReadClient 同时往一个 BufferedReadCallback 里灌会串 */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_timer_on;

/* 正在访谈的那一台（CHIP 任务里） */
static struct {
    int      slot;                        /* -1 = 空闲 */
    bool     err;
    int64_t  t0_ms;
    uint32_t gen;
    mc_iv_t  work;                        /* 读的过程中往这里填，读完整再拷进 s_iv */
} s_cur = { -1, false, 0, 0, {} };

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int slot_of(uint64_t node)
{
    for (int i = 0; s_iv && i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++)
        if (s_iv[i].used && s_iv[i].iv.node_id == node) return i;
    return -1;
}

static int slot_new(uint64_t node)
{
    int i = slot_of(node);
    if (i >= 0) return i;
    for (i = 0; s_iv && i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++) {
        if (s_iv[i].used) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);           /* 别的任务经 matter_ctrl_node_iv 在读 */
        memset(&s_iv[i], 0, sizeof(s_iv[i]));
        s_iv[i].iv.node_id = node;
        s_iv[i].used = true;
        xSemaphoreGive(s_lock);
        return i;
    }
    return -1;
}

static esp_err_t iv_persist(const mc_iv_t *iv)
{
    const matter_ctrl_hooks_t *h = mc_hooks();
    if (!h || !h->iv_save) return ESP_ERR_INVALID_STATE;
    uint8_t *buf = (uint8_t *)malloc(MC_IV_BLOB_MAX);
    if (!buf) return ESP_ERR_NO_MEM;
    const int n = mc_iv_encode(iv, buf, MC_IV_BLOB_MAX);
    const esp_err_t e = n > 0 ? h->iv_save(iv->node_id, MC_IV_VER, buf, (size_t)n) : ESP_ERR_INVALID_SIZE;
    free(buf);
    return e;
}

/* ---------------------------------------------------------------- 读回来的值 */

static void take_str(chip::TLV::TLVReader *d, char dst[MC_IV_STR_MAX + 1])
{
    const uint8_t *p = nullptr;
    if (d->GetType() == chip::TLV::kTLVType_UTF8String && d->GetDataPtr(p) == CHIP_NO_ERROR)
        mc_iv_set_str(dst, (const char *)p, d->GetLength());
}

static void take_u16(chip::TLV::TLVReader *d, uint16_t *out)
{
    uint16_t v;
    if (d->Get(v) == CHIP_NO_ERROR) *out = v;
}

class iv_cb : public chip::app::ReadClient::Callback {
public:
    void OnAttributeData(const chip::app::ConcreteDataAttributePath &path, chip::TLV::TLVReader *data,
                         const chip::app::StatusIB &status) override
    {
        if (s_cur.slot < 0 || !data || status.mStatus != chip::Protocols::InteractionModel::Status::Success) return;
        mc_iv_t *iv = &s_cur.work;
        const uint32_t cl = path.mClusterId, at = path.mAttributeId;
        if (cl == 0x0028 && path.mEndpointId == 0) {
            if (at == 1) take_str(data, iv->vendor);
            else if (at == 2) take_u16(data, &iv->vid);
            else if (at == 3) take_str(data, iv->product);
            else if (at == 4) take_u16(data, &iv->pid);
            else if (at == 5) take_str(data, iv->label);
            return;
        }
        if (cl == 0x002F && at == 0xFFFC) {
            uint32_t fm;
            if (data->Get(fm) == CHIP_NO_ERROR && (fm & 0x2u)) iv->flags |= MC_IV_F_BATTERY;
            return;
        }
        if (cl != 0x001D || data->GetType() != chip::TLV::kTLVType_Array) return;
        mc_iv_ep_t *e = mc_iv_ep(iv, path.mEndpointId);
        if (!e) return;
        chip::TLV::TLVType outer;
        if (data->EnterContainer(outer) != CHIP_NO_ERROR) return;
        while (data->Next() == CHIP_NO_ERROR) {
            if (at == 1) {                                    /* ServerList：簇 ID */
                uint32_t id;
                if (data->Get(id) == CHIP_NO_ERROR) {
                    mc_iv_add_cl(iv, e, id);
                    if (path.mEndpointId == 0 && id == 0x0046) iv->flags |= MC_IV_F_ICD;
                }
            } else if (at == 0 && data->GetType() == chip::TLV::kTLVType_Structure) {   /* DeviceTypeList：{0: deviceType, 1: revision} */
                chip::TLV::TLVType st;
                if (data->EnterContainer(st) != CHIP_NO_ERROR) break;
                while (data->Next() == CHIP_NO_ERROR) {
                    uint32_t dt;
                    if (data->GetTag() == chip::TLV::ContextTag(0) && data->Get(dt) == CHIP_NO_ERROR) mc_iv_add_dt(iv, e, dt);
                }
                (void)data->ExitContainer(st);
            }
        }
        (void)data->ExitContainer(outer);
    }
    void OnError(CHIP_ERROR err) override
    {
        s_cur.err = true;
        ESP_LOGW(TAG, "访谈节点 %" PRIu64 " 读出错：%s", s_cur.slot >= 0 ? s_iv[s_cur.slot].iv.node_id : 0, err.AsString());
    }
    void OnDeallocatePaths(chip::app::ReadPrepareParams &&) override {}
    void OnDone(chip::app::ReadClient *) override;
};

static iv_cb s_cb;
/* BufferedReadCallback 每次读之前原地重建（审计）：它的 OnError 只清已缓冲的列表、不清"正在拼的路径"，读到一半出错 / 超时，
   下一台的第一个回调会先收到上一台那条路径的空列表（凭空多一个空端点）。重建时上一个读请求一定已经 OnDone、ReadClient 已被
   esp-matter 的 client_deleter 删掉（s_reading 保证），没人再引用旧对象。 */
alignas(chip::app::BufferedReadCallback) static uint8_t s_buf_mem[sizeof(chip::app::BufferedReadCallback)];
static chip::app::BufferedReadCallback *s_buf_cb;

/* 收尾：成功拷进表、落盘、通知；失败退避（CHIP 任务里） */
static void iv_end(bool ok)
{
    const int i = s_cur.slot;
    if (i < 0) return;
    s_cur.slot = -1;
    s_cur.gen++;
    iv_slot_t *sl = &s_iv[i];
    if (ok && !mc_iv_complete(&s_cur.work)) {
        /* 读请求本身成功、可要紧的路径回了错误状态（ACL 被别的管理员改了、UnsupportedAccess……）：OnAttributeData 把这些丢了，
           读回来是空的。别当成"访谈完了"落盘 —— 那样它永远 kind:unknown、再也不重访（审计） */
        ESP_LOGW(TAG, "访谈节点 %" PRIu64 " 读回来没有根端点的簇表：按失败算", sl->iv.node_id);
        ok = false;
    }
    if (!ok) {
        sl->tries++;
        static const int64_t k_backoff_ms[] = { 30000, 120000, 600000 };
        sl->next_ms = now_ms() + k_backoff_ms[sl->tries - 1 < 3 ? sl->tries - 1 : 2];
        sl->queued = sl->tries < MC_IV_TRIES;
        ESP_LOGW(TAG, "访谈节点 %" PRIu64 " 没成（第 %u 次）%s", sl->iv.node_id, sl->tries, sl->queued ? "，过会儿再试" : "，这次开机不再试");
        return;
    }
    const bool added = sl->added;
    s_cur.work.flags |= MC_IV_F_DONE;
    s_cur.work.node_id = sl->iv.node_id;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sl->iv = s_cur.work;
    sl->queued = false;
    sl->added = false;
    sl->tries = 0;
    xSemaphoreGive(s_lock);
    const esp_err_t pe = iv_persist(&sl->iv);
    if (pe != ESP_OK) ESP_LOGE(TAG, "访谈结果没落盘（%s）：下次开机再访谈一遍", esp_err_to_name(pe));
    ESP_LOGI(TAG, "访谈节点 %" PRIu64 " 完成：%s / %s，%u 个端点", sl->iv.node_id, sl->iv.vendor, sl->iv.product, sl->iv.n_ep);
    const matter_ctrl_hooks_t *h = mc_hooks();
    mc_sub_want(sl->iv.node_id, &sl->iv);               /* 按这次的簇订（路径没变就不动订阅） */
    if (h && h->event) h->event(sl->iv.node_id, added ? MATTER_CTRL_EV_ADDED : MATTER_CTRL_EV_INTERVIEWED, 0);
}

void iv_cb::OnDone(chip::app::ReadClient *)
{
    s_reading = false;
    iv_end(!s_cur.err);                                  /* 已经超时收过尾：iv_end 看 slot < 0 直接返回 */
}

static void on_connected(void *, chip::Messaging::ExchangeManager &em, const chip::SessionHandle &sh)
{
    if (s_cur.slot < 0) return;
    chip::OperationalDeviceProxy proxy(&em, sh);
    chip::app::AttributePathParams paths[] = {
        chip::app::AttributePathParams(0, 0x0028, 1), chip::app::AttributePathParams(0, 0x0028, 2),
        chip::app::AttributePathParams(0, 0x0028, 3), chip::app::AttributePathParams(0, 0x0028, 4),
        chip::app::AttributePathParams(0, 0x0028, 5),
        /* 两参数的是"所有端点"（通配端点）；类型要写明，否则 (EndpointId, ClusterId) 与 (ClusterId, AttributeId) 两个构造有歧义 */
        chip::app::AttributePathParams(chip::ClusterId(0x001D), chip::AttributeId(0)),
        chip::app::AttributePathParams(chip::ClusterId(0x001D), chip::AttributeId(1)),
        chip::app::AttributePathParams(chip::ClusterId(0x002F), chip::AttributeId(0xFFFC)),
    };
    if (s_buf_cb) s_buf_cb->~BufferedReadCallback();
    s_buf_cb = new (s_buf_mem) chip::app::BufferedReadCallback(s_cb);
    const esp_err_t e = esp_matter::client::interaction::read::send_request(&proxy, paths, sizeof(paths) / sizeof(paths[0]), nullptr, 0,
                                                                           *s_buf_cb);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "访谈的读请求没发出去：%s", esp_err_to_name(e));
        iv_end(false);
        return;
    }
    s_reading = true;
}

static void on_conn_fail(void *, const chip::ScopedNodeId &peer, CHIP_ERROR err)
{
    if (s_cur.slot < 0) return;
    ESP_LOGW(TAG, "访谈节点 %" PRIu64 " 连不上：%s", peer.GetNodeId(), err.AsString());
    iv_end(false);
}

static chip::Callback::Callback<chip::OnDeviceConnected> s_on_conn(on_connected, nullptr);
static chip::Callback::Callback<chip::OnDeviceConnectionFailure> s_on_fail(on_conn_fail, nullptr);

/* CHIP 任务里每 MC_IV_TICK_MS 看一次：空闲就挑下一台到了时候的；正在做的超时了就算失败 */
static void arm_tick(void);

static void iv_tick(chip::System::Layer *, void *)
{
    if (s_cur.slot >= 0 && now_ms() - s_cur.t0_ms >= MC_IV_TIMEOUT_MS) {
        ESP_LOGW(TAG, "访谈节点 %" PRIu64 " 超过 %d s 没结论", s_iv[s_cur.slot].iv.node_id, MC_IV_TIMEOUT_MS / 1000);
        iv_end(false);                               /* 迟到的回调看 s_cur.slot < 0，不再认 */
    }
    if (s_cur.slot < 0 && !s_reading && mc_running()) {
        const int64_t now = now_ms();
        for (int i = 0; i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++) {
            iv_slot_t *sl = &s_iv[i];
            if (!sl->used || !sl->queued || now < sl->next_ms) continue;
            memset(&s_cur.work, 0, sizeof(s_cur.work));
            s_cur.slot = i;
            s_cur.err = false;
            s_cur.t0_ms = now;
            ESP_LOGI(TAG, "访谈节点 %" PRIu64 "（第 %u 次）", sl->iv.node_id, sl->tries + 1);
            const CHIP_ERROR e = matter_controller_client::get_instance().get_commissioner()->GetConnectedDevice(sl->iv.node_id, &s_on_conn,
                                                                                                               &s_on_fail);
            if (e != CHIP_NO_ERROR) {
                ESP_LOGW(TAG, "访谈节点 %" PRIu64 " 没发起：%s", sl->iv.node_id, e.AsString());
                iv_end(false);
            }
            break;
        }
    }
    arm_tick();
}

/* CHIP 的定时器池是静态的（CONFIG_NUM_TIMERS 32）：挂不上要说出来，下次有访谈要排时再挂（同 mc_sub.cpp，审计） */
static void arm_tick(void)
{
    const CHIP_ERROR e = chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(MC_IV_TICK_MS), iv_tick, nullptr);
    s_timer_on = e == CHIP_NO_ERROR;
    if (!s_timer_on) ESP_LOGE(TAG, "访谈的轮询计时器挂不上（%s）：下次有访谈要排时再挂", e.AsString());
}

/* ---------------------------------------------------------------- 对内（mc_priv.h） */

esp_err_t mc_iv_boot_load(const uint64_t *nodes, int n)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (!s_iv) {
        s_iv = (iv_slot_t *)heap_caps_calloc(CONFIG_BITENGINE_MATTER_MAX_NODES, sizeof(iv_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_iv) return ESP_ERR_NO_MEM;
    }
    const matter_ctrl_hooks_t *h = mc_hooks();
    uint8_t *buf = (uint8_t *)malloc(MC_IV_BLOB_MAX);
    if (!buf) return ESP_ERR_NO_MEM;
    for (int k = 0; k < n; k++) {
        const int i = slot_new(nodes[k]);
        if (i < 0) break;
        size_t len = MC_IV_BLOB_MAX;
        mc_iv_t iv;
        const bool got = h && h->iv_load && h->iv_load(nodes[k], MC_IV_VER, buf, &len) == MATTER_CTRL_LOAD_OK && mc_iv_decode(buf, len, &iv) &&
                         iv.node_id == nodes[k];
        xSemaphoreTake(s_lock, portMAX_DELAY);               /* slot_new 已经置了 used：httpd 这会儿可能在拷（审计） */
        if (got) s_iv[i].iv = iv;
        s_iv[i].queued = !(s_iv[i].iv.flags & MC_IV_F_DONE);       /* 没访谈完（或读不回来）：开机再访谈 */
        xSemaphoreGive(s_lock);
    }
    free(buf);
    return ESP_OK;
}

void mc_iv_on_setup(void)
{
    if (s_timer_on || !s_iv) return;
    for (int i = 0; i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++)       /* 开机读回、已经访谈完的：照表立即开订（9.5） */
        if (s_iv[i].used && (s_iv[i].iv.flags & MC_IV_F_DONE)) mc_sub_want(s_iv[i].iv.node_id, &s_iv[i].iv);
    arm_tick();
}

void mc_iv_request(uint64_t node, bool added)
{
    if (!s_iv) return;
    const int i = slot_new(node);
    if (i < 0) return;
    s_iv[i].added = added;
    s_iv[i].queued = true;
    if (!s_timer_on) arm_tick();                         /* 计时器上次没挂上：借这次补挂 */
    s_iv[i].tries = 0;
    s_iv[i].next_ms = 0;
}

void mc_iv_forget(uint64_t node)
{
    mc_sub_forget(node);
    const matter_ctrl_hooks_t *h = mc_hooks();
    if (h && h->iv_erase) h->iv_erase(node);              /* 表里没有这一格（表没分配上 / 满了）也删存的那条 */
    const int i = slot_of(node);
    if (i < 0) return;
    if (s_cur.slot == i) iv_end(false);                   /* 在途的读请求照样等它 OnDone（s_reading），迟到的结果看 slot < 0 不认 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_iv[i], 0, sizeof(s_iv[i]));
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- 对外 */

bool matter_ctrl_node_iv(uint64_t node, struct mc_iv_s *out)
{
    if (!out || !s_lock || !s_iv) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int i = slot_of(node);
    if (i >= 0) *out = s_iv[i].iv;
    xSemaphoreGive(s_lock);
    return i >= 0;
}

bool matter_ctrl_node_iv_busy(uint64_t node)
{
    const int cur = s_cur.slot;                          /* 别的任务里读：CHIP 任务随时可能把它改成 -1，只读一次（审计） */
    return s_iv && cur >= 0 && cur < CONFIG_BITENGINE_MATTER_MAX_NODES && s_iv[cur].iv.node_id == node;
}
