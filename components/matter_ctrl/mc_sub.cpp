/* mc_sub —— Matter 设备的订阅：在线与读数（docs/15 设计 9.5；M7 第五步）。
 *
 * 每台访谈完的设备一条订阅（一个 ReadClient），路径按访谈结果挑（mc_sub_core.c）；全在 CHIP 任务里。
 *  - 建：SendAutoResubscribeRequest(对端) —— CASE 由 ReadClient 自己经 CASESessionManager 建；同一时刻最多 MC_SUB_SETUP_MAX 条在建
 *    （交换上下文 8、CASE 客户端 4：给配网与访谈留着；控制器起来后照节点表立即开订）。
 *  - 断：CHIP 的存活计时（最大间隔 + 余量）或会话失效 → OnResubscriptionNeeded：按 9.5 退避 5 → 30 → 120 s 重订（自己算，不用
 *    CHIP 的斐波那契）；超时断的要重建 CASE。设备拒了路径数（规范只保证 3 条）→ 结束这条、收窄到前 3 条重建。
 *  - 离线：订阅没建好、且超过 2 × 最大间隔没听到它（mc_sub_judge）；建好即在线。在线 / 离线、读数翻转经钩子告诉 hub_core。
 *
 * H1（connectedhomeip 随 esp_matter 1.6.0~1，src/app/ReadClient.{h,cpp}，2026-10-24 核过）：
 *  - SendAutoResubscribeRequest(ScopedNodeId, ReadPrepareParams&&)：参数整个移进 ReadClient；同步失败时它自己调 StopResubscription →
 *    OnDeallocatePaths（路径数组在那里还给我们）；成功后终止时 Close → OnResubscriptionNeeded（返回 CHIP_NO_ERROR = 已安排重订）
 *    或 OnError + OnDeallocatePaths + OnDone。OnDone 是 Close 的最后一句：在 OnDone 里删 ReadClient 与回调对象是安全的（esp-matter
 *    的 client_deleter 也这么做）。
 *  - ~ReadClient：assertChipStackLockedByCurrentThread（只能在 CHIP 任务里删）、StopResubscription（→ OnDeallocatePaths，回调对象
 *    这时还得活着）、从 IM 引擎摘掉；**不调 OnDone**。所以先删 ReadClient、再删回调对象。
 *  - ScheduleResubscription(ms, NullOptional, 要不要重建 CASE)：只能在 Idle 状态调（OnResubscriptionNeeded 里就是）。
 *    DefaultResubscribePolicy 对 LIT ICD 的 INACTIVE_TIMEOUT 返回错误让它睡着等 check-in —— 我们不做 ICD 注册（9.1），照常重订。
 *  - OnSubscriptionEstablished 每次（含重订）都来；GetReportingIntervals(min, max) 拿设备答应的间隔。
 *  - 内存：chip::Platform::New / MemoryCalloc 在本配置下走 PSRAM（CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL）。
 *  - 报告值：标量直接是 TLV 的 Boolean / UnsignedInteger / SignedInteger / Null；CumulativeEnergyImported 是结构体
 *    （EnergyMeasurementStruct，上下文标签 0 = energy，int64 mWh），可为 null。 */
#include "mc_priv.h"

#include <inttypes.h>
#include <new>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/MessageDef/StatusIB.h>
#include <esp_matter_controller_client.h>
#include <lib/support/CHIPMem.h>

#include "mc_sub_core.h"

static const char *TAG = "mc_sub";

#define MC_SUB_TICK_MS    5000
#define MC_SUB_SETUP_MAX  4                /* 128 台时开机全部订上约要 1 min（每条在建几百 ms 到几 s）；留 4 个 CASE 客户端给配网 / 访谈 / 控制 */

using esp_matter::controller::matter_controller_client;
using chip::app::ReadClient;

class sub_cb;

typedef struct {
    bool       used;
    bool       want;                       /* 访谈完了、该订着 */
    bool       narrow;                     /* 设备拒过路径数：只订前 MC_SUB_NARROW 条 */
    uint64_t   node;
    mc_path_t  paths[MC_SUB_PATHS_MAX];
    int        n_paths;
    ReadClient *rc;
    sub_cb     *cb;
    int64_t    since_ms;                   /* 这次开机开始订它的时刻（从没听到过它时，离线判定从这里算） */
    int64_t    next_ms;                    /* ReadClient 没了（建失败 / 结束）之后，什么时候再建 */
    uint32_t   fails;                      /* 连着建失败 / 结束的次数（退避用；订阅建好清零） */
    uint32_t   retries;                    /* OnResubscriptionNeeded 连着几次（退避用；建好清零） */
    mc_live_t  live;
} sub_slot_t;

static sub_slot_t *s_sub;                  /* PSRAM，CONFIG_BITENGINE_MATTER_MAX_NODES 格；只在 CHIP 任务里写，别的任务经 s_lock 拷 live */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_timer_on;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void notify(const sub_slot_t *s, int ev, unsigned bits)
{
    const matter_ctrl_hooks_t *h = mc_hooks();
    if (h && h->event) h->event(s->node, ev, bits);
}

/* online 变了才说：未知 → 在线不说（开机头一次连上），其余都说 */
static void set_online(sub_slot_t *s, int8_t on)
{
    const int8_t was = s->live.online;
    if (on == was) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s->live.online = on;
    xSemaphoreGive(s_lock);
    if (on == 0) notify(s, MATTER_CTRL_EV_OFFLINE, 0);
    else if (on == 1 && was == 0) notify(s, MATTER_CTRL_EV_ONLINE, 0);
}

static void set_sub(sub_slot_t *s, uint8_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s->live.sub = st;
    xSemaphoreGive(s_lock);
}

/* ReadClient 没了（建失败 / 整条结束）之后什么时候再建：9.5 的第二、三档（30 → 120 s；第一档 5 s 是就地重订），±20 % 抖动 */
static void retry_later(sub_slot_t *s)
{
    s->fails++;
    s->next_ms = now_ms() + mc_sub_jitter(mc_sub_backoff_ms(s->fails + 1), esp_random());
}

/* ---------------------------------------------------------------- 一条订阅的回调（每台一个对象：OnAttributeData 不带 ReadClient） */

class sub_cb final : public ReadClient::Callback {
public:
    explicit sub_cb(int slot) : m_slot(slot) {}

    void OnAttributeData(const chip::app::ConcreteDataAttributePath &path, chip::TLV::TLVReader *data,
                         const chip::app::StatusIB &status) override
    {
        sub_slot_t *s = &s_sub[m_slot];
        const int64_t now = now_ms();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s->live.last_ms = now;                              /* 报了就是听到了它，哪怕这一项是错误状态 */
        xSemaphoreGive(s_lock);
        if (!data || status.mStatus != chip::Protocols::InteractionModel::Status::Success) return;   /* 例：电量订在了不带 BAT 的 PowerSource 上 */
        uint8_t field = MC_FIELD_NONE;
        for (int i = 0; i < s->n_paths; i++) {
            const mc_path_t *p = &s->paths[i];
            if (p->ep == path.mEndpointId && p->cl == path.mClusterId && p->at == path.mAttributeId) {
                field = p->field;
                break;
            }
        }
        if (field == MC_FIELD_NONE) return;
        mc_val_t v = { MC_V_NULL, 0, 0 };
        if (!decode(data, &v)) return;
        unsigned sev = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool ok = mc_sub_apply(&s->live.st, field, &v, now, &sev);
        xSemaphoreGive(s_lock);
        if (!ok) ESP_LOGW(TAG, "节点 %" PRIu64 " 的 %u/0x%04" PRIX32 "/0x%04" PRIX32 " 报了认不出的值，丢掉", s->node,
                          (unsigned)path.mEndpointId, path.mClusterId, path.mAttributeId);
        if (sev) notify(s, MATTER_CTRL_EV_STATE, sev);
    }

    /* 每一份报告都会来（含读数没变时设备按最大间隔发的空保活报告 —— 那种不走 OnAttributeData）：听到了它（审计 ❌：
       不记这个，读数不变的设备 last_ms 停在订阅建好那一刻，订阅一抖就按"一小时没听到"误报离线） */
    void NotifySubscriptionStillActive(const ReadClient &) override
    {
        sub_slot_t *s = &s_sub[m_slot];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s->live.last_ms = now_ms();
        xSemaphoreGive(s_lock);
    }

    void OnSubscriptionEstablished(chip::SubscriptionId) override
    {
        sub_slot_t *s = &s_sub[m_slot];
        uint16_t mn = 0, mx = 0;
        ReadClient *rc = s->rc;
        if (!rc || rc->GetReportingIntervals(mn, mx) != CHIP_NO_ERROR) mx = MC_SUB_MAX_IV_S;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s->live.sub = MC_SUB_UP;
        s->live.max_iv_s = mx ? mx : MC_SUB_MAX_IV_S;
        s->live.last_ms = now_ms();
        xSemaphoreGive(s_lock);
        s->fails = 0;
        s->retries = 0;
        ESP_LOGI(TAG, "节点 %" PRIu64 " 订阅建好：间隔 [%u, %u] s、%d 条路径", s->node, mn, mx, s->n_paths);
        set_online(s, 1);
    }

    CHIP_ERROR OnResubscriptionNeeded(ReadClient *rc, CHIP_ERROR cause) override
    {
        sub_slot_t *s = &s_sub[m_slot];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s->live.sub = MC_SUB_WAIT;
        s->live.resubs++;
        xSemaphoreGive(s_lock);
        const chip::app::StatusIB st(cause);
        using chip::Protocols::InteractionModel::Status;
        if (!s->narrow && s->n_paths > MC_SUB_NARROW &&
            (st.mStatus == Status::PathsExhausted || st.mStatus == Status::ResourceExhausted || st.mStatus == Status::InvalidAction)) {
            ESP_LOGW(TAG, "节点 %" PRIu64 " 不接受 %d 条路径（%s）：收窄到 %d 条重建", s->node, s->n_paths, cause.AsString(), MC_SUB_NARROW);
            s->narrow = true;
            return cause;                                   /* → OnError、OnDeallocatePaths、OnDone：删掉、tick 里按收窄的重建 */
        }
        if (s->retries++ == 0) {
            /* 刚断（订阅建好之后头一次）：5 s 后就地重订一次（9.5 第一档），超时断的重建 CASE */
            const uint32_t ms = mc_sub_jitter(mc_sub_backoff_ms(1), esp_random());
            ESP_LOGW(TAG, "节点 %" PRIu64 " 订阅断了（%s）：%" PRIu32 " ms 后重订", s->node, cause.AsString(), ms);
            return rc->ScheduleResubscription(ms, chip::NullOptional, cause == CHIP_ERROR_TIMEOUT);
        }
        /* 又没成：结束这条，交给 sub_tick 按 30 → 120 s 重建 —— 那里受"同一时刻最多 MC_SUB_SETUP_MAX 条在建"约束（审计：就地重订
           不受约束，一批离线设备会按同一节奏一起占 CASE 客户端，配网 / 访谈的 CASE 拿到 NO_MEMORY） */
        ESP_LOGW(TAG, "节点 %" PRIu64 " 重订没成（%s）：稍后重建", s->node, cause.AsString());
        return cause;
    }

    void OnError(CHIP_ERROR err) override
    {
        ESP_LOGW(TAG, "节点 %" PRIu64 " 订阅结束：%s", s_sub[m_slot].node, err.AsString());
    }

    void OnDeallocatePaths(chip::app::ReadPrepareParams &&p) override
    {
        chip::Platform::MemoryFree(p.mpAttributePathParamsList);
        p.mpAttributePathParamsList = nullptr;
        p.mAttributePathParamsListSize = 0;
    }

    void OnDone(ReadClient *rc) override
    {
        /* 订阅彻底结束（我们拒了重订 / 出了错）：删 ReadClient 与本对象（Close 的最后一句，之后谁也不碰它们），过会儿重建 */
        sub_slot_t *s = &s_sub[m_slot];
        if (s->rc == rc) {
            s->rc = nullptr;
            s->cb = nullptr;
            retry_later(s);
            set_sub(s, MC_SUB_NONE);
        }
        chip::Platform::Delete(rc);
        chip::Platform::Delete(this);
    }

private:
    static bool decode(chip::TLV::TLVReader *d, mc_val_t *v)
    {
        switch (d->GetType()) {
        case chip::TLV::kTLVType_Null:
            v->t = MC_V_NULL;
            return true;
        case chip::TLV::kTLVType_Boolean: {
            bool b;
            if (d->Get(b) != CHIP_NO_ERROR) return false;
            v->t = MC_V_BOOL;
            v->u = b ? 1 : 0;
            return true;
        }
        case chip::TLV::kTLVType_UnsignedInteger:
            v->t = MC_V_UINT;
            return d->Get(v->u) == CHIP_NO_ERROR;
        case chip::TLV::kTLVType_SignedInteger:
            v->t = MC_V_INT;
            return d->Get(v->i) == CHIP_NO_ERROR;
        case chip::TLV::kTLVType_Structure: {              /* EnergyMeasurementStruct：取上下文标签 0（energy） */
            chip::TLV::TLVType outer;
            if (d->EnterContainer(outer) != CHIP_NO_ERROR) return false;
            bool got = false;
            while (!got && d->Next() == CHIP_NO_ERROR) {
                if (d->GetTag() != chip::TLV::ContextTag(0)) continue;
                if (d->GetType() == chip::TLV::kTLVType_SignedInteger) {
                    v->t = MC_V_INT;
                    got = d->Get(v->i) == CHIP_NO_ERROR;
                } else if (d->GetType() == chip::TLV::kTLVType_UnsignedInteger) {
                    v->t = MC_V_UINT;
                    got = d->Get(v->u) == CHIP_NO_ERROR;
                } else {
                    break;
                }
            }
            (void)d->ExitContainer(outer);
            return got;
        }
        default:
            return false;
        }
    }

    int m_slot;
};

/* ---------------------------------------------------------------- 建 / 拆 */

static void teardown(sub_slot_t *s)
{
    if (s->rc) chip::Platform::Delete(s->rc);              /* → StopResubscription → cb 的 OnDeallocatePaths 还路径；不调 OnDone */
    if (s->cb) chip::Platform::Delete(s->cb);
    s->rc = nullptr;
    s->cb = nullptr;
    set_sub(s, MC_SUB_NONE);
}

static bool create(int i)
{
    sub_slot_t *s = &s_sub[i];
    const int n = s->narrow && s->n_paths > MC_SUB_NARROW ? MC_SUB_NARROW : s->n_paths;
    auto *paths = static_cast<chip::app::AttributePathParams *>(chip::Platform::MemoryCalloc((size_t)n, sizeof(chip::app::AttributePathParams)));
    sub_cb *cb = paths ? chip::Platform::New<sub_cb>(i) : nullptr;
    ReadClient *rc = cb ? chip::Platform::New<ReadClient>(chip::app::InteractionModelEngine::GetInstance(),
                                                          chip::app::InteractionModelEngine::GetInstance()->GetExchangeManager(), *cb,
                                                          ReadClient::InteractionType::Subscribe)
                        : nullptr;
    if (!rc) {
        if (cb) chip::Platform::Delete(cb);
        chip::Platform::MemoryFree(paths);
        ESP_LOGE(TAG, "节点 %" PRIu64 " 的订阅没有内存", s->node);
        return false;
    }
    for (int k = 0; k < n; k++) {
        const mc_path_t *p = &s->paths[k];
        new (&paths[k]) chip::app::AttributePathParams(chip::EndpointId(p->ep), chip::ClusterId(p->cl), chip::AttributeId(p->at));
    }
    chip::app::ReadPrepareParams params;
    params.mpAttributePathParamsList = paths;
    params.mAttributePathParamsListSize = (size_t)n;
    params.mMinIntervalFloorSeconds = MC_SUB_MIN_IV_S;
    params.mMaxIntervalCeilingSeconds = MC_SUB_MAX_IV_S;
    params.mKeepSubscriptions = false;                       /* 设备那边我们上次开机留下的旧订阅一并清掉 */
    s->rc = rc;                                              /* 回调（含同步失败时的 OnDeallocatePaths）可能马上要找它 */
    s->cb = cb;
    set_sub(s, MC_SUB_SETUP);                                /* 先置：CASE 池满时失败回调在发起里同步回来、会改成 WAIT，别再盖掉（审计） */
    const chip::ScopedNodeId peer(s->node, matter_controller_client::get_instance().get_commissioner()->GetFabricIndex());
    const CHIP_ERROR e = rc->SendAutoResubscribeRequest(peer, std::move(params));
    if (e != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "节点 %" PRIu64 " 的订阅没发起：%s", s->node, e.AsString());
        teardown(s);                                         /* 路径已在 OnDeallocatePaths 里还了；状态回 NONE */
        return false;
    }
    return true;
}

static void arm_tick(void);

static void sub_tick(chip::System::Layer *, void *)
{
    const int64_t now = now_ms();
    int setting_up = 0;
    for (int i = 0; i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++)
        if (s_sub[i].used && s_sub[i].live.sub == MC_SUB_SETUP) setting_up++;
    for (int i = 0; i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++) {
        sub_slot_t *s = &s_sub[i];
        if (!s->used || !s->want) continue;
        if (!s->rc && now >= s->next_ms && setting_up < MC_SUB_SETUP_MAX && mc_running()) {
            if (!s->since_ms) s->since_ms = now;
            if (create(i)) setting_up++;
            else retry_later(s);
        }
        const int64_t ref = s->live.last_ms ? s->live.last_ms : s->since_ms;
        if (ref) set_online(s, mc_sub_judge(s->live.online, s->live.sub == MC_SUB_UP, ref, s->live.max_iv_s, now));
    }
    arm_tick();
}

/* CHIP 的定时器池是静态的（CONFIG_NUM_TIMERS 32、没开 POOL_USE_HEAP），每条订阅还常驻一个：挂不上要说出来，下次有设备要订时再挂（审计） */
static void arm_tick(void)
{
    const CHIP_ERROR e = chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(MC_SUB_TICK_MS), sub_tick, nullptr);
    s_timer_on = e == CHIP_NO_ERROR;
    if (!s_timer_on) ESP_LOGE(TAG, "订阅的轮询计时器挂不上（%s）：CHIP 定时器池满了？下次有设备要订时再挂", e.AsString());
}

/* ---------------------------------------------------------------- 对内（mc_priv.h） */

static int slot_of(uint64_t node)
{
    for (int i = 0; s_sub && i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++)
        if (s_sub[i].used && s_sub[i].node == node) return i;
    return -1;
}

static bool ensure_table(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (!s_sub) s_sub = (sub_slot_t *)heap_caps_calloc(CONFIG_BITENGINE_MATTER_MAX_NODES, sizeof(sub_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_sub != nullptr;
}

void mc_sub_want(uint64_t node, const mc_iv_t *iv)
{
    if (!iv || !(iv->flags & MC_IV_F_DONE) || !ensure_table()) return;
    mc_path_t paths[MC_SUB_PATHS_MAX];
    const int n = mc_sub_paths(iv, paths, MC_SUB_PATHS_MAX);
    int i = slot_of(node);
    if (!s_timer_on && mc_running()) arm_tick();           /* 计时器上次没挂上：借这次补挂 */
    if (i >= 0) {
        sub_slot_t *s = &s_sub[i];
        if (s->n_paths == n && mc_sub_paths_eq(s->paths, paths, n)) {
            s->want = true;
            return;                                         /* 重新访谈、路径没变：订阅照旧 */
        }
        teardown(s);                                        /* 路径变了：拆了按新的重建（读数保留） */
        memcpy(s->paths, paths, sizeof(paths));
        s->n_paths = n;
        s->narrow = false;
        s->want = true;
        s->next_ms = 0;
        return;
    }
    for (i = 0; i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++) {
        if (s_sub[i].used) continue;
        sub_slot_t *s = &s_sub[i];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->node = node;
        s->live.online = -1;
        s->live.max_iv_s = MC_SUB_MAX_IV_S;
        xSemaphoreGive(s_lock);
        memcpy(s->paths, paths, sizeof(paths));
        s->n_paths = n;
        s->want = true;
        return;
    }
    ESP_LOGE(TAG, "订阅表满了（%d）：节点 %" PRIu64 " 不订", CONFIG_BITENGINE_MATTER_MAX_NODES, node);
}

void mc_sub_forget(uint64_t node)
{
    const int i = slot_of(node);
    if (i < 0) return;
    teardown(&s_sub[i]);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_sub[i], 0, sizeof(s_sub[i]));
    xSemaphoreGive(s_lock);
}

void mc_sub_on_setup(void)
{
    if (s_timer_on || !ensure_table()) return;
    arm_tick();
}

void mc_sub_status(uint8_t *subs, uint8_t *up)
{
    uint8_t a = 0, b = 0;
    if (s_sub && s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++) {
            if (!s_sub[i].used || !s_sub[i].want) continue;
            a++;
            if (s_sub[i].live.sub == MC_SUB_UP) b++;
        }
        xSemaphoreGive(s_lock);
    }
    if (subs) *subs = a;
    if (up) *up = b;
}

/* ---------------------------------------------------------------- 对外 */

bool matter_ctrl_node_live(uint64_t node, mc_live_t *out)
{
    if (!out || !s_sub || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < CONFIG_BITENGINE_MATTER_MAX_NODES; i++) {
        if (!s_sub[i].used || s_sub[i].node != node) continue;
        *out = s_sub[i].live;
        found = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return found;
}
