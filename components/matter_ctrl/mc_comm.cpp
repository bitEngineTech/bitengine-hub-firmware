/* mc_comm —— 节点表、在网配网与移除（docs/15 设计 9.3、9.4 B、10.1；M7 第三步）。
 *
 * 配网（9.4 B，G0-5c 走通的路 + 9.8b 第 12、13 条）：
 *   调用方任务：规整配网码 → CHIP 的解析器解出 discriminator / PIN / VID·PID / 发现方式（只能蓝牙的码直接拒）→ 排进 CHIP 任务。
 *   CHIP 任务：分配节点号、PENDING 记录落盘读回确认（MC12）→ 每秒查一次本机代播的 _matterc，等 discriminator 对得上的出现
 *   （设备经 SRP 注册要开窗后 2–8 s；CHIP 的浏览只查一次，记录还没来就配必然"Discovery timed out"）—— 最多等 MC_LOCAL_WAIT_MS，
 *   等不到也照样发起（设备可能在局域网 / 别人的 Thread 网络上，CHIP 自己在网络上浏览）→ 撤掉残留的配网委托 → PairDevice。
 *   结果：成功 = OnCommissioningSuccess（节点改 OK 落盘）；PASE 失败 = OnPairingComplete(err)（码错 / 过期，之后不再有回调）；
 *   配网某一步失败 = OnCommissioningFailure（带阶段与认证结果）；**发现超时什么回调都没有**（9.8b 第 13 条）—— 自己的计时器兜底：
 *   PairDevice 之后 MC_FIND_MS 内 PASE 没成就 StopPairing、算"找不到"；总时长 MC_TOTAL_MS（低于 Muse 的 180 s）。失败删掉那条记录、号不还。
 *
 * 移除（10.1）：esp-matter 的 unpair（CHIP 的 AutoCurrentFabricRemover：CASE → RemoveFabric）→ 回调；设备确认了才删记录，
 *   force 时设备不答话也删。
 *
 * 线程：节点表与配网状态只在 CHIP 任务里改（配网的回调、计时器都在 CHIP 任务里）；别的任务读快照经 s_lock。
 * 同一时刻只做一件（配网或移除）：s_busy 在调用方任务里置位（临界区），完成回调之前在 CHIP 任务里清掉。
 *
 * H1（esp_matter 1.6.0~1，2026-10-24 读源码核过）：
 *  - pairing_command 的回调（esp_matter_controller_pairing_command.cpp）：OnPairingComplete(err≠OK) 撤委托 + pase_callback(err)；
 *    OnCommissioningSuccess / Failure 撤委托 + 对应回调；unpair 完成走 remove_fabric_handler → unpair_complete_callback(node, err)。
 *  - esp-matter 的 pairing_code() 吞掉 PairDevice 的返回值、失败时不撤委托 —— 这里直接调 commissioner 的 PairDevice，失败自己撤。
 *  - PairDevice(nodeId, setUpCode, params, DiscoveryType::kDiscoveryNetworkOnly)：SetUpCodePairer 自己解析 QR / 手动码。
 *  - mdns_lookup_delegated_service 查的是本机 mdns 组件里的代播表（srp_readvertise 推进去的），在调用方任务里同步返回。 */
#include "mc_priv.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mdns.h>

#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>

#include "mc_code_core.h"
#include "mc_nodes_core.h"

static const char *TAG = "mc_comm";

#define MC_LOCAL_WAIT_MS  15000      /* 等本机出现待配网记录 */
#define MC_POLL_MS        1000
#define MC_FIND_MS        45000      /* PairDevice 之后多久还没 PASE 就算找不到（CHIP 自己浏览 30 s + 余量） */
#define MC_TOTAL_MS       165000     /* 整个配网的上限：低于 hub.matter.commission 的 timeout_ms 180000（附录 A） */
#define MC_REMOVE_MS      60000      /* 移除的上限（设备不答话时 CASE 要重试一阵才失败） */

using esp_matter::controller::matter_controller_client;
using esp_matter::controller::pairing_command;

/* ---------------------------------------------------------------- 节点表 */

static SemaphoreHandle_t s_lock;                 /* 护 s_nodes（快照读者在别的任务里） */
static StaticSemaphore_t s_lock_buf;
static mc_nodes_t s_nodes;
static bool s_nodes_ok;                          /* 开机读回成功（或确实没有）；失败时这次开机不配网、不移除 */

/* 编码 + 经钩子落盘读回确认。CHIP 任务 / 启动任务里调（内部栈；hub_store 同步等） */
static esp_err_t nodes_save(void)
{
    const matter_ctrl_hooks_t *h = mc_hooks();
    if (!h || !h->nodes_save) return ESP_ERR_INVALID_STATE;
    uint8_t *buf = (uint8_t *)malloc(MC_NODES_BLOB_MAX);
    if (!buf) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);       /* 只护 memcpy 级的编码，持有者都不阻塞 */
    const int n = mc_nodes_encode(&s_nodes, buf, MC_NODES_BLOB_MAX);
    xSemaphoreGive(s_lock);
    const esp_err_t e = n > 0 ? h->nodes_save(MC_NODES_VER, buf, (size_t)n) : ESP_ERR_INVALID_SIZE;
    free(buf);
    return e;
}

esp_err_t mc_nodes_boot_load(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    mc_nodes_init(&s_nodes, (uint8_t)CONFIG_BITENGINE_MATTER_MAX_NODES);
    s_nodes_ok = false;
    const matter_ctrl_hooks_t *h = mc_hooks();
    if (!h || !h->nodes_load || !h->nodes_save) return ESP_ERR_INVALID_STATE;
    uint8_t *buf = (uint8_t *)malloc(MC_NODES_BLOB_MAX);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = MC_NODES_BLOB_MAX;
    const matter_ctrl_load_t r = h->nodes_load(MC_NODES_VER, buf, &len);
    esp_err_t err = ESP_OK;
    if (r == MATTER_CTRL_LOAD_OK) {
        mc_nodes_t t;
        const mc_nodes_err_t de = mc_nodes_decode(buf, len, (uint8_t)CONFIG_BITENGINE_MATTER_MAX_NODES, &t);
        if (de != MC_NODES_OK) {
            /* 存着的表坏了：不覆盖（覆盖 = next_node 回到 2，号会重发），这次开机不配网，等人看 */
            ESP_LOGE(TAG, "节点表解不开（%d）—— 这次开机不配网、不移除", (int)de);
            err = ESP_ERR_INVALID_CRC;
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_nodes = t;
            const unsigned dropped = mc_nodes_drop_pending(&s_nodes);
            xSemaphoreGive(s_lock);
            if (dropped) {
                ESP_LOGW(TAG, "上次有 %u 台配到一半（断电 / 重启）：删掉，号不还", dropped);
                err = nodes_save();
            }

        }
    } else if (r != MATTER_CTRL_LOAD_EMPTY) {
        err = ESP_ERR_INVALID_STATE;
    }
    free(buf);
    s_nodes_ok = err == ESP_OK;
    if (s_nodes_ok) {
        /* 访谈结果（第四步）：读回来，没访谈完的开机补。表是空的也要调（分配访谈表：之后配进来的要用） */
        static uint64_t ids[MC_NODES_CAP];       /* 512 B，别放启动任务的栈上 */
        int k = 0;
        for (unsigned j = 0; j < s_nodes.n; j++) if (s_nodes.nodes[j].state == MC_NODE_OK) ids[k++] = s_nodes.nodes[j].node_id;
        const esp_err_t ie = mc_iv_boot_load(ids, k);
        if (ie != ESP_OK) ESP_LOGE(TAG, "访谈表没起来（%s）：设备照常，只是没有厂家 / 型号 / 能力", esp_err_to_name(ie));
    }
    return err;
}

int matter_ctrl_nodes(matter_ctrl_node_t *out, int max)
{
    if (!out || max <= 0 || !s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (unsigned i = 0; i < s_nodes.n && n < max; i++, n++) {
        out[n].node_id = s_nodes.nodes[i].node_id;
        out[n].vid = s_nodes.nodes[i].vid;
        out[n].pid = s_nodes.nodes[i].pid;
        out[n].commissioned = s_nodes.nodes[i].state == MC_NODE_OK;
    }
    xSemaphoreGive(s_lock);
    return n;
}

/* ---------------------------------------------------------------- 一件事（配网 / 移除）的状态 */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;                              /* 调用方任务里置位，CHIP 任务里清 */

typedef enum { OP_NONE = 0, OP_COMMISSION, OP_REMOVE } op_t;
typedef enum { PH_IDLE = 0, PH_WAIT_LOCAL, PH_PAIRING, PH_REMOVING } phase_t;

static struct {
    op_t     op;
    phase_t  phase;
    char     code[MC_CODE_MAX + 1];
    bool     disc_short;
    uint16_t disc, vid, pid;
    uint64_t node;
    bool     force;
    bool     pase_ok;
    int64_t  t0_ms, t_pair_ms;
    uint32_t gen;                                /* 计时器回调认这一代：完成之后迟到的计时器 / 回调不算数 */
    matter_ctrl_done_cb_t cb;
    void    *ctx;
} s_op;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

void mc_comm_status(uint8_t *nodes, bool *busy)
{
    if (nodes) *nodes = s_lock ? s_nodes.n : 0;
    if (busy) {
        portENTER_CRITICAL(&s_mux);
        *busy = s_busy;
        portEXIT_CRITICAL(&s_mux);
    }
}

static bool try_claim(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool ok = !s_busy;
    if (ok) s_busy = true;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static void release(void)
{
    portENTER_CRITICAL(&s_mux);
    s_busy = false;
    portEXIT_CRITICAL(&s_mux);
}

static void set_why(char *why, size_t n, const char *s)
{
    if (why && n) snprintf(why, n, "%s", s);
}

/* 收尾：恰好一次。清状态、放开 busy，再调完成回调（回调里可能马上发起下一件） */
static void finish(bool ok, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void finish(bool ok, const char *code, const char *fmt, ...)
{
    if (s_op.op == OP_NONE) return;
    matter_ctrl_result_t r = {};
    r.ok = ok;
    r.node_id = s_op.node;
    r.vid = s_op.vid;
    r.pid = s_op.pid;
    r.err_code = ok ? nullptr : code;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r.msg, sizeof(r.msg), fmt, ap);
        va_end(ap);
    }
    const matter_ctrl_done_cb_t cb = s_op.cb;
    void *ctx = s_op.ctx;
    const op_t op = s_op.op;
    s_op.gen++;
    s_op.op = OP_NONE;
    s_op.phase = PH_IDLE;
    memset(s_op.code, 0, sizeof(s_op.code));     /* 配网码里有 PIN */
    release();
    if (ok) ESP_LOGI(TAG, "%s 节点 %" PRIu64 "：成功", op == OP_COMMISSION ? "配网" : "移除", r.node_id);
    else ESP_LOGW(TAG, "%s 节点 %" PRIu64 "：%s —— %s", op == OP_COMMISSION ? "配网" : "移除", r.node_id, code, r.msg);
    if (cb) cb(ctx, &r);
}

/* 配网失败：删掉那条记录（号不还）再收尾 */
static void comm_fail(const char *code, const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool had = mc_nodes_remove(&s_nodes, s_op.node) == MC_NODES_OK;
    xSemaphoreGive(s_lock);
    if (had && nodes_save() != ESP_OK) ESP_LOGE(TAG, "删配网失败的节点 %" PRIu64 " 没落盘（下次开机按 PENDING 丢掉）", s_op.node);
    finish(false, code, "%s", msg);
}

/* 先收尾、再停：StopPairing 会同步回调（PASE 中 → OnPairingComplete(CANCELLED)；配网中 → OnCommissioningFailure），收尾之后
   s_op.op 已是 OP_NONE、回调一律不认 —— 否则用户看到的是 code_rejected / internal 而不是 not_found / timeout，而且第二次收尾会把
   紧接着排进来的下一件收掉（审计）。节点号用调用方存下的那份：收尾之后别的任务可能已经占了 s_op */
static void fail_and_stop(const char *code, const char *msg)
{
    const uint64_t node = s_op.node;
    comm_fail(code, msg);
    auto *comm = matter_controller_client::get_instance().get_commissioner();
    (void)comm->StopPairing(node);
    comm->RegisterPairingDelegate(nullptr);
}

/* ---------------------------------------------------------------- CHIP 任务：计时器 */

static const char *const k_not_found =
    "The hub could not find the device. Check that the sharing window in the other app is still open and the device is on this hub's "
    "Thread network or local network, then retry with a new code.";

static void comm_pair(void);

static void op_timer(chip::System::Layer *, void *app)
{
    const uint32_t gen = (uint32_t)(uintptr_t)app;
    if (gen != s_op.gen || s_op.op == OP_NONE) return;   /* 上一件留下的 */
    const int64_t now = now_ms();
    if (s_op.op == OP_REMOVE) {
        if (now - s_op.t0_ms >= MC_REMOVE_MS) {
            if (s_op.force) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                (void)mc_nodes_remove(&s_nodes, s_op.node);
                xSemaphoreGive(s_lock);
                const esp_err_t se = nodes_save();
                if (se == ESP_OK) mc_iv_forget(s_op.node);
                else mc_sub_forget(s_op.node);           /* 落盘失败：访谈记录留着（下次开机它还在表里），订阅只是运行态，照样拆（审计） */
                if (se != ESP_OK) finish(false, "storage_error", "The device was removed from the hub's list, but the change could not be saved.");
                else finish(true, nullptr, "The device did not answer; it was removed from the hub only. Reset it in its own app.");
            } else {
                finish(false, "unreachable", "The device did not answer, so it still has access from this hub. Make sure it is powered and "
                       "reachable and try again, or remove it with force and then reset it in its own app.");
            }
            return;
        }
    } else if (s_op.phase == PH_WAIT_LOCAL) {
        mdns_result_t *res = nullptr;
        bool found = false;
        if (mdns_lookup_delegated_service(nullptr, "_matterc", "_udp", 20, &res) == ESP_OK) {
            for (mdns_result_t *r = res; r && !found; r = r->next) {
                for (size_t i = 0; i < r->txt_count && !found; i++) {
                    if (r->txt[i].key && !strcmp(r->txt[i].key, "D") && r->txt[i].value)
                        found = mc_code_disc_match(s_op.disc_short, s_op.disc, r->txt[i].value);
                }
            }
        }
        mdns_query_results_free(res);
        if (found || now - s_op.t0_ms >= MC_LOCAL_WAIT_MS) {
            ESP_LOGI(TAG, "本机代播里%s discriminator 对得上的待配网设备（等了 %" PRId64 " ms）—— 发起配网", found ? "有" : "没等到",
                     now - s_op.t0_ms);
            comm_pair();
            if (s_op.gen != gen) return;             /* 收过尾了：别给（可能已经排进来的）下一件再起一条计时器链 */
        }
    } else if (s_op.phase == PH_PAIRING) {
        if (now - s_op.t0_ms >= MC_TOTAL_MS) {
            fail_and_stop("timeout", "Adding the device took too long and was stopped. Open the sharing window again in the other app "
                          "and retry with the new code.");
            return;
        }
        if (!s_op.pase_ok && now - s_op.t_pair_ms >= MC_FIND_MS) {
            fail_and_stop("not_found", k_not_found);     /* 发现超时 CHIP 不回调（9.8b 第 13 条） */
            return;
        }
    }
    if (s_op.gen != gen) return;
    (void)chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(MC_POLL_MS), op_timer, (void *)(uintptr_t)s_op.gen);
}

static void comm_pair(void)
{
    auto *comm = matter_controller_client::get_instance().get_commissioner();
    if (comm->GetPairingDelegate() != nullptr) {
        ESP_LOGW(TAG, "上一次配网的委托还挂着，先撤掉（9.8b 第 13 条）");
        comm->RegisterPairingDelegate(nullptr);
    }
    comm->RegisterPairingDelegate(&pairing_command::get_instance());
    chip::Controller::CommissioningParameters params;
    const CHIP_ERROR e = comm->PairDevice(s_op.node, s_op.code, params, chip::Controller::DiscoveryType::kDiscoveryNetworkOnly);
    if (e != CHIP_NO_ERROR) {
        comm->RegisterPairingDelegate(nullptr);
        char msg[120];
        snprintf(msg, sizeof(msg), "The hub could not start adding the device (%s).", e.AsString());
        comm_fail("internal", msg);
        return;
    }
    s_op.phase = PH_PAIRING;
    s_op.t_pair_ms = now_ms();
}

/* ---------------------------------------------------------------- CHIP 任务：pairing_command 的回调 */

static void on_pase(CHIP_ERROR err)
{
    if (s_op.op != OP_COMMISSION || s_op.phase != PH_PAIRING) return;
    if (err == CHIP_NO_ERROR) {
        s_op.pase_ok = true;
        ESP_LOGI(TAG, "PASE 建立（节点 %" PRIu64 "），开始配网", s_op.node);
        return;
    }
    /* PASE 失败之后 CHIP 不再有回调（esp-matter 已撤委托）：码错、窗口关了、PIN 被设备拒了 */
    comm_fail("code_rejected", "The device did not accept the pairing code. The code may have expired or been mistyped: open the "
              "sharing window again in the other app and use the new code.");
    (void)err;
}

static void on_ok(chip::ScopedNodeId id)
{
    if (s_op.op != OP_COMMISSION || s_op.phase != PH_PAIRING || id.GetNodeId() != s_op.node) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    (void)mc_nodes_set_state(&s_nodes, s_op.node, MC_NODE_OK);
    xSemaphoreGive(s_lock);
    const esp_err_t se = nodes_save();
    if (se != ESP_OK) {
        /* 设备已经配进来了，网关这边却没记住：报失败，内存里也撤掉，和报给用户的一致（不然下一次随便哪次落盘就把它以 OK 写进去，审计）。
           设备上会留着我们的 fabric：句子里让用户去它自己的 App 里删 */
        comm_fail("storage_error", "The device was added but the hub could not save it; remove it in its own app and try again.");
        return;
    }
    mc_iv_request(s_op.node, true);                  /* 访谈（第四步）：厂家 / 型号 / 端点 / 能力；完了 hub_core 推"加好了" */
    finish(true, nullptr, "Added.");
}

static void on_fail(chip::ScopedNodeId id, CHIP_ERROR err, chip::Controller::CommissioningStage stage,
                    std::optional<chip::Credentials::AttestationVerificationResult> att)
{
    if (s_op.op != OP_COMMISSION || s_op.phase != PH_PAIRING || id.GetNodeId() != s_op.node) return;
    char msg[200];
    if (att.has_value() && att.value() != chip::Credentials::AttestationVerificationResult::kSuccess) {
        snprintf(msg, sizeof(msg), "The device's certificate could not be verified (code %d), so the hub did not add it. It may not be "
                 "a certified Matter device.", (int)att.value());
        comm_fail("attestation_failed", msg);
        return;
    }
    snprintf(msg, sizeof(msg), "Adding the device failed at step %s (%s). Open the sharing window again and retry.",
             chip::Controller::StageToString(stage), err.AsString());
    comm_fail("internal", msg);
}

static void on_unpair(chip::NodeId node, CHIP_ERROR err)
{
    if (s_op.op != OP_REMOVE || node != s_op.node) return;
    if (err != CHIP_NO_ERROR && !s_op.force) {
        finish(false, "unreachable", "The device did not answer, so it still has access from this hub. Make sure it is powered and "
               "reachable and try again, or remove it with force and then reset it in its own app.");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    (void)mc_nodes_remove(&s_nodes, s_op.node);
    xSemaphoreGive(s_lock);
    if (nodes_save() != ESP_OK) {
        mc_sub_forget(s_op.node);                        /* 设备已执行 RemoveFabric：订阅只会一直失败、过 10 min 误报离线（审计） */
        finish(false, "storage_error", "The device was removed, but the hub could not save the change.");
        return;
    }
    mc_iv_forget(s_op.node);
    if (err != CHIP_NO_ERROR) finish(true, nullptr, "The device did not answer; it was removed from the hub only. Reset it in its own app.");
    else finish(true, nullptr, "Removed.");
}

void mc_comm_on_setup(void)
{
    pairing_command::get_instance().set_callbacks({ on_pase, on_ok, on_fail, on_unpair });
    mc_ctl_init();
    mc_iv_on_setup();
    mc_sub_on_setup();
}

/* ---------------------------------------------------------------- 发起（调用方任务 → CHIP 任务） */

static void comm_start_in_chip(intptr_t)
{
    uint64_t node = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const mc_nodes_err_t ae = mc_nodes_alloc(&s_nodes, s_op.vid, s_op.pid, &node);
    xSemaphoreGive(s_lock);
    if (ae != MC_NODES_OK) {
        finish(false, "internal", ae == MC_NODES_EFULL ? "The hub already has as many Matter devices as it can manage."
                                                        : "The hub's Matter device list is damaged.");
        return;
    }
    s_op.node = node;
    /* 号落盘读回确认之前绝不拿去配（MC12） */
    if (nodes_save() != ESP_OK) {
        comm_fail("storage_error", "The hub could not save the new device's number, so it did not start adding it.");
        return;
    }
    s_op.phase = PH_WAIT_LOCAL;
    s_op.t0_ms = now_ms();
    ESP_LOGI(TAG, "配网：节点 %" PRIu64 "，discriminator %s %u，先等本机出现待配网记录", node, s_op.disc_short ? "短" : "长", s_op.disc);
    (void)chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(MC_POLL_MS), op_timer, (void *)(uintptr_t)s_op.gen);
}

esp_err_t matter_ctrl_commission(const char *code, matter_ctrl_done_cb_t cb, void *ctx, char *why, size_t why_len)
{
    if (!mc_running() || !s_nodes_ok) {
        set_why(why, why_len, "The hub's Matter controller is not running right now.");
        return ESP_ERR_INVALID_STATE;
    }
    char norm[MC_CODE_MAX + 1];
    const int kind = mc_code_normalize(code, norm, sizeof(norm));
    if (!kind) {
        set_why(why, why_len, "That is not a Matter pairing code: use the 11 or 21 digit code, or the QR text starting with MT:.");
        return ESP_ERR_INVALID_ARG;
    }
    chip::SetupPayload pl;
    const CHIP_ERROR pe = kind == MC_CODE_QR ? chip::QRCodeSetupPayloadParser(std::string(norm)).populatePayload(pl)
                                             : chip::ManualSetupPayloadParser(std::string(norm)).populatePayload(pl);
    if (pe != CHIP_NO_ERROR) {
        memset(norm, 0, sizeof(norm));
        set_why(why, why_len, "That pairing code is not valid (a digit may be wrong). Check it in the other app and try again.");
        return ESP_ERR_INVALID_ARG;
    }
    if (pl.rendezvousInformation.HasValue() &&
        !pl.rendezvousInformation.Value().Has(chip::RendezvousInformationFlag::kOnNetwork)) {
        memset(norm, 0, sizeof(norm));
        set_why(why, why_len, "This code is for a new device over Bluetooth, which this hub cannot add yet. Add it in another app first, "
                "then share it from there and use that code.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool full = s_nodes.n >= s_nodes.max;
    xSemaphoreGive(s_lock);
    if (full) {                                  /* 当场拒（头文件承诺的 ESP_ERR_NO_MEM；审计） */
        memset(norm, 0, sizeof(norm));
        set_why(why, why_len, "The hub already has as many Matter devices as it can manage; remove one first.");
        return ESP_ERR_NO_MEM;
    }
    if (!try_claim()) {
        memset(norm, 0, sizeof(norm));
        set_why(why, why_len, "The hub is already adding or removing a Matter device; wait for it to finish.");
        return ESP_ERR_INVALID_STATE;
    }
    s_op.op = OP_COMMISSION;
    s_op.phase = PH_IDLE;
    memcpy(s_op.code, norm, sizeof(s_op.code));
    memset(norm, 0, sizeof(norm));
    s_op.disc_short = pl.discriminator.IsShortDiscriminator();
    s_op.disc = s_op.disc_short ? pl.discriminator.GetShortValue() : pl.discriminator.GetLongValue();
    s_op.vid = pl.vendorID;
    s_op.pid = pl.productID;
    s_op.node = 0;
    s_op.pase_ok = false;
    s_op.cb = cb;
    s_op.ctx = ctx;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(comm_start_in_chip, 0) != CHIP_NO_ERROR) {
        s_op.op = OP_NONE;
        memset(s_op.code, 0, sizeof(s_op.code));
        release();
        set_why(why, why_len, "The hub's Matter controller is busy; try again in a moment.");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void remove_in_chip(intptr_t)
{
    s_op.phase = PH_REMOVING;
    s_op.t0_ms = now_ms();
    const esp_err_t e = pairing_command::unpair_device(s_op.node);
    if (e != ESP_OK) {
        on_unpair(s_op.node, CHIP_ERROR_INTERNAL);   /* 同"设备不答话"的口径：force 就从网关这边删 */
        return;
    }
    (void)chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(MC_POLL_MS), op_timer, (void *)(uintptr_t)s_op.gen);
}

esp_err_t matter_ctrl_remove(uint64_t node_id, bool force, matter_ctrl_done_cb_t cb, void *ctx, char *why, size_t why_len)
{
    if (!mc_running() || !s_nodes_ok) {
        set_why(why, why_len, "The hub's Matter controller is not running right now.");
        return ESP_ERR_INVALID_STATE;
    }
    bool known = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const mc_node_t *n = mc_nodes_find(&s_nodes, node_id);
    known = n && n->state == MC_NODE_OK;
    xSemaphoreGive(s_lock);
    if (!known) {
        set_why(why, why_len, "There is no such Matter device on this hub.");
        return ESP_ERR_NOT_FOUND;
    }
    if (!try_claim()) {
        set_why(why, why_len, "The hub is already adding or removing a Matter device; wait for it to finish.");
        return ESP_ERR_INVALID_STATE;
    }
    s_op.op = OP_REMOVE;
    s_op.phase = PH_IDLE;
    s_op.node = node_id;
    s_op.vid = s_op.pid = 0;
    s_op.force = force;
    s_op.cb = cb;
    s_op.ctx = ctx;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(remove_in_chip, 0) != CHIP_NO_ERROR) {
        s_op.op = OP_NONE;
        release();
        set_why(why, why_len, "The hub's Matter controller is busy; try again in a moment.");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
