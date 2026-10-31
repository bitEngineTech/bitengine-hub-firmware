/* mc_ctl —— Matter 设备的控制：开 / 关 / 切换 / 亮度（docs/15 设计 9.5「命令」；M7 第六步）。
 *
 * matter_ctrl_control 在调用方任务里同步等结论（hub.device.control 的处理函数；同 Zigbee 的 zhub_control）：
 *   ① 调用方：查访谈结果挑端点、记下发之前的读数，把这一件交给 CHIP 任务（ScheduleWork），等 s_done；
 *   ② CHIP 任务：GetConnectedDevice（CASE，已有会话就直接用）→ InvokeCommandRequest；回应 / 失败 / MC_CTL_DEADLINE_MS 兜底都走 ctl_finish；
 *   ③ 调用方：invoke 成功后再等最多 3 s，订阅报回目标值 = confirmed，否则 delivered（9.5）；发之前就已经是目标、订阅又建好着
 *      （设备不会因为没变化而报）也算 confirmed。
 * 同一时刻只做一件（s_mtx，拿不到回 busy）；一件的状态在 s_op（s_mux 护着），按 gen 认领 —— 超时之后迟到的回调认不上。
 *
 * H1（connectedhomeip 随 esp_matter 1.6.0~1，2026-10-24 核过）：
 *  - controller/InvokeInteraction.h：InvokeCommandRequest(ExchangeManager*, SessionHandle, EndpointId, 请求, onSuccess, onError)
 *    —— 两个回调是 std::function，恰好来一个；TypedCommandCallback 由它自己 new / delete。On / Off / Toggle 与
 *    LevelControl::MoveToLevelWithOnOff 的回应类型都是 DataModel::NullObjectType，都不要求定时交互（门锁要，随后）。
 *  - MoveToLevelWithOnOff：level（u8，1–254）、transitionTime（可空：空 = 设备自己的默认过渡）、optionsMask / optionsOverride（0）。
 *  - onError 的 CHIP_ERROR：IsIMStatus() 时是设备回的状态（StatusIB(err).mStatus）；否则是超时 / 会话断了 —— 可能已经执行。
 *  - DeviceCommissioner::GetConnectedDevice：同一个 Callback 对象再次登记会先从旧的队列里摘掉（CallbackDeque::Enqueue → Cancel），
 *    所以一次只挂一个连接请求，超时那件迟到的连接结果不会落到下一件上；回调里再按会话的对端节点号核一遍。 */
#include "mc_priv.h"

#include <inttypes.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <app-common/zap-generated/cluster-objects.h>
#include <controller/InvokeInteraction.h>
#include <esp_matter_controller_client.h>
#include <platform/CHIPDeviceLayer.h>

#include "matter_ctrl_iv.h"
#include "matter_ctrl_live.h"
#include "mc_sub_core.h"

static const char *TAG = "mc_ctl";

#define MC_CTL_DEADLINE_MS  9000           /* 连接 + 回应的兜底；调用方最多再多等 1.5 s（CHIP 任务排队），再加 3 s 等报告：整件 ≤ 约 13.5 s，在命令的 15 s 之内 */
#define MC_CTL_POLL_MS      100

using esp_matter::controller::matter_controller_client;
namespace Clusters = chip::app::Clusters;

static SemaphoreHandle_t s_mtx;
static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_done;           /* 静态：调用方等超时走了之后，迟到的 give 不会落到已经释放的栈上（下一件开始前先清掉） */
static StaticSemaphore_t s_done_buf;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
    uint32_t gen;
    bool     pending;
    bool     sent;                         /* InvokeCommandRequest 已经调了（之后超时 = 可能执行了；之前超时 = 一定没执行，审计） */
    uint64_t node;
    uint16_t ep;
    int      act;                          /* 实际发的：亮度 0 % 发的是 OFF */
    uint8_t  level;                        /* CurrentLevel 1–254 */
    uint8_t  err;                          /* MATTER_CTRL_CTL_E_* */
    uint8_t  im;
} s_op;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void ctl_deadline(chip::System::Layer *, void *app);

/* 一件的结论（CHIP 任务里）：认 gen，只认一次 */
static void ctl_finish(uint32_t gen, uint8_t err, uint8_t im)
{
    bool give = false;
    portENTER_CRITICAL(&s_mux);
    if (s_op.pending && s_op.gen == gen) {
        s_op.pending = false;
        s_op.err = err;
        s_op.im = im;
        give = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!give) return;
    chip::DeviceLayer::SystemLayer().CancelTimer(ctl_deadline, (void *)(uintptr_t)gen);
    xSemaphoreGive(s_done);
}

static void ctl_deadline(chip::System::Layer *, void *app)
{
    const uint32_t gen = (uint32_t)(uintptr_t)app;
    portENTER_CRITICAL(&s_mux);
    const bool sent = s_op.gen == gen && s_op.sent;
    portEXIT_CRITICAL(&s_mux);
    /* 连接还没建好就到点（设备拔了电，走 Thread 的 CASE 重传很容易超过 9 s）：没发，别说"可能执行了" */
    ctl_finish(gen, sent ? MATTER_CTRL_CTL_E_NO_ANSWER : MATTER_CTRL_CTL_E_UNREACHABLE, 0);
}

static bool cur_op(uint32_t *gen, uint64_t *node, uint16_t *ep, int *act, uint8_t *level)
{
    portENTER_CRITICAL(&s_mux);
    const bool p = s_op.pending;
    *gen = s_op.gen;
    *node = s_op.node;
    *ep = s_op.ep;
    *act = s_op.act;
    *level = s_op.level;
    portEXIT_CRITICAL(&s_mux);
    return p;
}

template <typename T>
static CHIP_ERROR invoke(chip::Messaging::ExchangeManager &em, const chip::SessionHandle &sh, uint16_t ep, const T &req, uint32_t gen)
{
    auto ok = [gen](const chip::app::ConcreteCommandPath &, const chip::app::StatusIB &, const typename T::ResponseType &) {
        ctl_finish(gen, MATTER_CTRL_CTL_E_OK, 0);
    };
    auto bad = [gen](CHIP_ERROR e) {
        if (e.IsIMStatus()) {
            const chip::app::StatusIB st(e);
            ctl_finish(gen, MATTER_CTRL_CTL_E_REFUSED, (uint8_t)st.mStatus);
        } else {
            ESP_LOGW(TAG, "命令没等到回应：%s", e.AsString());
            ctl_finish(gen, MATTER_CTRL_CTL_E_NO_ANSWER, 0);
        }
    };
    return chip::Controller::InvokeCommandRequest(&em, sh, chip::EndpointId(ep), req, ok, bad);
}

static void on_connected(void *, chip::Messaging::ExchangeManager &em, const chip::SessionHandle &sh)
{
    uint32_t gen;
    uint64_t node;
    uint16_t ep;
    int act;
    uint8_t level;
    if (!cur_op(&gen, &node, &ep, &act, &level)) return;          /* 已经超时收过尾 */
    if (sh->GetPeer().GetNodeId() != node) return;                 /* 不是这一件的（见文件头 H1） */
    portENTER_CRITICAL(&s_mux);
    const bool mine = s_op.pending && s_op.gen == gen;
    if (mine) s_op.sent = true;                                     /* 先记再发：发起之后的超时都按"可能执行了"报 */
    portEXIT_CRITICAL(&s_mux);
    if (!mine) return;
    CHIP_ERROR e;
    switch (act) {
    case MATTER_CTRL_ACT_ON:     e = invoke(em, sh, ep, Clusters::OnOff::Commands::On::Type(), gen); break;
    case MATTER_CTRL_ACT_OFF:    e = invoke(em, sh, ep, Clusters::OnOff::Commands::Off::Type(), gen); break;
    case MATTER_CTRL_ACT_TOGGLE: e = invoke(em, sh, ep, Clusters::OnOff::Commands::Toggle::Type(), gen); break;
    case MATTER_CTRL_ACT_LEVEL: {
        Clusters::LevelControl::Commands::MoveToLevelWithOnOff::Type req;
        req.level = level;
        req.transitionTime.SetNull();                              /* 设备自己的默认过渡 */
        e = invoke(em, sh, ep, req, gen);
        break;
    }
    default:
        e = CHIP_ERROR_INVALID_ARGUMENT;
        break;
    }
    if (e != CHIP_NO_ERROR) {                                      /* 没发出去：一定没执行 */
        ESP_LOGW(TAG, "节点 %" PRIu64 " 的命令没发出去：%s", node, e.AsString());
        ctl_finish(gen, MATTER_CTRL_CTL_E_UNREACHABLE, 0);
    }
}

static void on_conn_fail(void *, const chip::ScopedNodeId &peer, CHIP_ERROR err)
{
    uint32_t gen;
    uint64_t node;
    uint16_t ep;
    int act;
    uint8_t level;
    if (!cur_op(&gen, &node, &ep, &act, &level) || peer.GetNodeId() != node) return;
    ESP_LOGW(TAG, "节点 %" PRIu64 " 连不上：%s", node, err.AsString());
    ctl_finish(gen, MATTER_CTRL_CTL_E_UNREACHABLE, 0);
}

static chip::Callback::Callback<chip::OnDeviceConnected> s_on_conn(on_connected, nullptr);
static chip::Callback::Callback<chip::OnDeviceConnectionFailure> s_on_fail(on_conn_fail, nullptr);

static void ctl_start(intptr_t arg)
{
    const uint32_t gen = (uint32_t)arg;
    uint32_t g;
    uint64_t node;
    uint16_t ep;
    int act;
    uint8_t level;
    if (!cur_op(&g, &node, &ep, &act, &level) || g != gen) return;   /* 调用方已经等不及走了 */
    if (chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(MC_CTL_DEADLINE_MS), ctl_deadline,
                                                    (void *)(uintptr_t)gen) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "控制的兜底计时器挂不上（CHIP 定时器池满了？）：这件不做");
        ctl_finish(gen, MATTER_CTRL_CTL_E_UNREACHABLE, 0);
        return;
    }
    const CHIP_ERROR e = matter_controller_client::get_instance().get_commissioner()->GetConnectedDevice(node, &s_on_conn, &s_on_fail);
    if (e != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "节点 %" PRIu64 " 没发起连接：%s", node, e.AsString());
        ctl_finish(gen, MATTER_CTRL_CTL_E_UNREACHABLE, 0);
    }
}

/* ---------------------------------------------------------------- 对内 / 对外 */

void mc_ctl_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    if (!s_done) s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
}

esp_err_t matter_ctrl_control(uint64_t node, int act, uint8_t level_pct, matter_ctrl_ctl_t *out)
{
    if (!out || act < MATTER_CTRL_ACT_ON || act > MATTER_CTRL_ACT_LEVEL || (act == MATTER_CTRL_ACT_LEVEL && level_pct > 100)) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!mc_running() || !s_mtx || !s_done) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, 0) != pdTRUE) return ESP_ERR_TIMEOUT;   /* 另一条控制正在做：回 busy，别排队 */
    out->result = MATTER_CTRL_CTL_FAILED;
    mc_iv_t iv;
    if (!matter_ctrl_node_iv(node, &iv) || !(iv.flags & MC_IV_F_DONE)) {
        out->err = MATTER_CTRL_CTL_E_NO_NODE;
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }
    int sact = act;
    int ep;
    if (act == MATTER_CTRL_ACT_LEVEL && level_pct == 0) {
        ep = mc_sub_first_ep(&iv, 0x0006);                         /* 0 % = 关：要有开关簇（同 Zigbee 的 ZH_CTL_E_NO_OFF） */
        sact = MATTER_CTRL_ACT_OFF;
    } else {
        ep = mc_sub_first_ep(&iv, act == MATTER_CTRL_ACT_LEVEL ? 0x0008 : 0x0006);
    }
    if (ep < 0) {
        out->err = MATTER_CTRL_CTL_E_NO_CLUSTER;
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }
    mc_live_t before;
    const bool known = matter_ctrl_node_live(node, &before);
    const bool on_known = known && (before.st.have & (1u << MC_ST_ONOFF));
    const int8_t want_on = act == MATTER_CTRL_ACT_ON ? 1 : act == MATTER_CTRL_ACT_OFF ? 0
                           : act == MATTER_CTRL_ACT_TOGGLE ? (on_known ? (int8_t)!before.st.onoff : (int8_t)-1)
                                                            : (int8_t)(level_pct ? 1 : 0);
    const bool already = known && before.sub == MC_SUB_UP && mc_ctl_already(&before.st, act, level_pct);

    (void)xSemaphoreTake(s_done, 0);                               /* 上一件超时之后迟到的 give */
    uint32_t gen;
    portENTER_CRITICAL(&s_mux);
    gen = ++s_op.gen;
    s_op.pending = true;
    s_op.sent = false;
    s_op.node = node;
    s_op.ep = (uint16_t)ep;
    s_op.act = sact;
    s_op.level = mc_ctl_level(level_pct);
    s_op.err = MATTER_CTRL_CTL_E_NO_ANSWER;
    s_op.im = 0;
    portEXIT_CRITICAL(&s_mux);
    const int64_t t0 = now_ms();
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ctl_start, (intptr_t)gen) != CHIP_NO_ERROR) {
        portENTER_CRITICAL(&s_mux);
        s_op.pending = false;
        portEXIT_CRITICAL(&s_mux);
        out->err = MATTER_CTRL_CTL_E_UNREACHABLE;
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }
    /* 等结论。醒来先核对是不是这一件的（审计：上一件被认领后、give 之前被抢占，那个迟到的 give 可能落到这一件上）；
       等到点还没收尾（CHIP 任务堵住了）就放弃这件 —— 之后的连接结果、回应、兜底都认不上，也不会再补发 */
    const int64_t until = t0 + MC_CTL_DEADLINE_MS + 1500;
    uint8_t err = MATTER_CTRL_CTL_E_NO_ANSWER, im = 0;
    for (;;) {
        const int64_t left = until - now_ms();
        const bool got = left > 0 && xSemaphoreTake(s_done, pdMS_TO_TICKS(left)) == pdTRUE;
        bool done = false;
        portENTER_CRITICAL(&s_mux);
        if (s_op.gen == gen && !s_op.pending) {                    /* 收过尾了（give 也许还在路上：那就是这次超时醒的） */
            err = s_op.err;
            im = s_op.im;
            done = true;
        } else if (!got) {
            if (s_op.gen == gen) {
                s_op.pending = false;
                err = s_op.sent ? MATTER_CTRL_CTL_E_NO_ANSWER : MATTER_CTRL_CTL_E_UNREACHABLE;
            }
            done = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (done) break;                                           /* 否则是别件迟到的 give：接着等剩下的时间 */
    }
    out->err = err;
    out->im_status = im;
    out->ms = (uint32_t)(now_ms() - t0);
    if (err != MATTER_CTRL_CTL_E_OK) {
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }
    out->result = MATTER_CTRL_CTL_DELIVERED;
    if (already) {
        out->result = MATTER_CTRL_CTL_CONFIRMED;                   /* 本来就是目标：设备不会报（没变化），invoke 成功即是 */
    } else {
        for (int64_t t = now_ms(); now_ms() - t < MC_CTL_CONFIRM_MS; vTaskDelay(pdMS_TO_TICKS(MC_CTL_POLL_MS))) {
            mc_live_t lv;
            if (matter_ctrl_node_live(node, &lv) && mc_ctl_reached(&lv.st, act, want_on, level_pct, t0)) {
                out->result = MATTER_CTRL_CTL_CONFIRMED;
                out->ms = (uint32_t)(now_ms() - t0);
                break;
            }
        }
    }
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}
