/* matter_ctrl —— 直连型的 Matter 控制器（docs/15_muse_direct_design_v1.md 第九节；M7 第一、二步）。
 *
 * 启动顺序：开机晚起 → 读 fabric 身份（hubdb 正本，经 hub_core 的钩子；没有就预选 fabric ID）→ matter 分区与身份绑定
 * （对不上就擦掉 CHIP 的状态）→ esp_matter::start() → CHIP 任务里：新建身份（要的话）并落盘读回确认 → 装进自己的签发器
 * （mc_issuer.h）→ matter_controller_client::init() + setup_commissioner() → 控制器这一侧写入自己的 IPK。
 *
 * H1（esp_matter 1.6.0~1 源码核过，2026-10-23）：
 *  - esp_matter::start(event_cb, arg)（esp_matter_core.h）：不带 server 时只起 CHIP 协议栈与 CHIP 任务；之后 CHIP 的 API
 *    一律投到 CHIP 任务里调（PlatformMgr().ScheduleWork），不能在别的任务里直接碰。
 *  - matter_controller_client::init(node, fabric, port)：建 DeviceControllerFactory（KVS 经 CONFIG_CHIP_KVS_NAMESPACE_PARTITION_LABEL
 *    落 matter 分区）；setup_commissioner() 经 get_credentials_issuer()（CONFIG_CUSTOM_OPERATIONAL_CREDS_ISSUER 时 = 我们
 *    set_custom_credentials_issuer 的那个）给控制器签一张 NOC，同一 fabric ID 下 fabric 表的条目开机复用。
 *  - setup_commissioner() 原本还写 CHIP 的测试组数据与公开的默认 IPK：构建期补丁去掉，IPK 由这里写（GroupDataProvider 的
 *    keyset 0 = IPK，SetSingleIpkEpochKey，GroupDataProvider.h）。
 *  - 控制器宣告自己（operational advertising）会改掉网关的 mDNS 主机名（9.8a 第 7 条）：构建期补丁 tools/patch_esp_matter.py 关掉。
 *
 * 防崩溃循环（红线 1：本板没有 BOOT 键）：RTC 里记"起了 Matter 但没撑过 MC_STABLE_MS 就崩了"的连续次数，到 MC_MAX_TRIES 就这次不起。
 * 只有崩溃类复位（panic / 各类看门狗，按 wd_boot_reason_get() 的真原因 —— main.c 崩溃后会补一次 esp_restart，esp_reset_reason()
 * 那时恒为 SW）才累计；其余复位（OTA 装完、用户点重启、配对剖面往返、上电）一律清零 —— 否则两次正常重启恰好落在开机后
 * 30–90 s 里就把 Matter 一直关着，只有断电才解（M7 第一步审计）。晚起 MC_START_DELAY_MS：避开开机时 SRP 成批重注册的 CPU 高峰
 * （memory matter-srp-scale-256），也给"起 CHIP 就崩"的固件留出 OTA 刷回的窗口。 */
#include "matter_ctrl.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "watchdog.h"                    /* wd_boot_reason_get()：崩溃后补重启过的真复位原因（T9.27） */

#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <credentials/GroupDataProvider.h>

#include "mc_ident_core.h"
#include "mc_issuer.h"
#include "mc_paa.h"
#include "mc_priv.h"

static const char *TAG = "matter_ctrl";

#define MC_PART            "matter"     /* CHIP 的 KVS 与绑定记号都在这个分区（设计 2.4） */
/* matter 分区里的绑定记号：这份 CHIP 状态属于哪个 fabric 身份（= 身份的 fabric ID）。不叫 mctrl —— 那是 hubdb 里身份正本的命名空间，
   同名两处会被当成一回事（CHECKLIST MX03）。M7 第一步的记号在 "mctrl"/"fabric_id"，换到这里后对不上 → 擦掉重建，正是要的 */
#define MC_BIND_NS         "mc_bind"
#define MC_BIND_KEY        "fabric_id"
#define MC_START_DELAY_MS  30000
#define MC_STABLE_MS       60000
#define MC_MAX_TRIES       2
#define MC_MAGIC           0x4D435452u   /* "MCTR"：RTC_NOINIT 上电是随机值，靠它判断计数有没有被初始化过 */
#define MC_TASK_STACK      6144   /* 第一步实测 4096 里用了约 1.2 KB；第二步多了 hub_store 的同步读与分区绑定，留足余量 */
#define MC_TASK_PRIO       4

static constexpr uint64_t kControllerNodeId = 1;   /* 9.3：控制器自己的 NOC 节点号 1，新设备从 2 起 */
/* 控制器的 UDP 监听口：不带 server，只用来收发 CASE / 订阅报文；避开设备默认的 5540 */
static constexpr uint16_t kListenPort = 5541;

static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_tries;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static const matter_ctrl_hooks_t *s_hooks;        /* hub_core 在 matter_ctrl_start 之前登记 */
/* 启动任务 → CHIP 任务的交接（ScheduleWork 之前写好、之后只有 CHIP 任务读）：读到的身份，或者"要新建"与预先选好的 fabric ID。
   ⚠️ 里面有私钥：装进签发器后立即清掉 */
static mc_ident_t s_ident;
static bool s_ident_need_new;
static mc::issuer s_issuer;
static matter_ctrl_status_t s_st;                 /* 写：启动任务 / CHIP 任务；读：matter_ctrl_get_status（加锁拷贝） */
static bool s_started;

using esp_matter::controller::matter_controller_client;

const char *matter_ctrl_state_str(matter_ctrl_state_t st)
{
    switch (st) {
    case MATTER_CTRL_OFF:      return "off";
    case MATTER_CTRL_WAITING:  return "waiting";
    case MATTER_CTRL_STARTING: return "starting";
    case MATTER_CTRL_RUNNING:  return "running";
    case MATTER_CTRL_FAILED:   return "failed";
    case MATTER_CTRL_HELD:     return "held";
    }
    return "unknown";
}

static void apply_test_work(intptr_t) { mc_paa_apply_test(); }

esp_err_t matter_ctrl_set_allow_test_devices(bool on)
{
    mc_paa_set_allow_test(on);                          /* 信任库那一半立即生效（查的时候读） */
    if (!mc_running()) return ESP_OK;                   /* 没在跑：起来时 setup 之后会按它设 CD 密钥 */
    return chip::DeviceLayer::PlatformMgr().ScheduleWork(apply_test_work, 0) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

void matter_ctrl_get_status(matter_ctrl_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
    mc_comm_status(&out->nodes, &out->busy);
    mc_sub_status(&out->subs, &out->subs_up);
    out->allow_test_devices = mc_paa_allow_test();
}

static void set_state(matter_ctrl_state_t st)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = st;
    portEXIT_CRITICAL(&s_mux);
}

static void fail(const char *step, int32_t err)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = MATTER_CTRL_FAILED;
    s_st.fail_step = step;
    s_st.fail_err = err;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGE(TAG, "控制器没起来：%s（%" PRId32 "）—— 这次开机不再试", step, err);
}

static uint32_t internal_free(void) { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

void matter_ctrl_set_hooks(const matter_ctrl_hooks_t *hooks) { s_hooks = hooks; }
const matter_ctrl_hooks_t *mc_hooks(void) { return s_hooks; }
bool mc_running(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool r = s_st.state == MATTER_CTRL_RUNNING;
    portEXIT_CRITICAL(&s_mux);
    return r;
}

static uint64_t rand_fabric_id(void)
{
    uint64_t fid = 0;
    do {
        esp_fill_random(&fid, sizeof(fid));            /* 9.3：随机 64 位；0 是 kUndefinedFabricId */
    } while (fid == 0);
    return fid;
}

/* 读 fabric 身份（hubdb 正本，经 hub_core 的钩子）。EMPTY = 确实没有 → 调用方新建；读不出来、或读出来解不开 → 失败，绝不当成"没有"：
   新建 = 换一个根 CA，之前配进来的设备全成孤儿。启动任务里跑（内部栈；缓冲在堆上，用完清零） */
static esp_err_t ident_read(bool *need_new)
{
    *need_new = false;
    if (!s_hooks || !s_hooks->ident_load || !s_hooks->ident_save) return ESP_ERR_INVALID_STATE;
    uint8_t *buf = (uint8_t *)calloc(1, MC_IDENT_BLOB_MAX);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = MC_IDENT_BLOB_MAX;
    const matter_ctrl_load_t r = s_hooks->ident_load(MC_IDENT_VER, buf, &len);
    esp_err_t err = ESP_OK;
    if (r == MATTER_CTRL_LOAD_EMPTY) {
        *need_new = true;
    } else if (r != MATTER_CTRL_LOAD_OK) {
        err = ESP_ERR_INVALID_STATE;
    } else if (mc_ident_decode(buf, len, &s_ident) != MC_IDENT_OK) {
        err = ESP_ERR_INVALID_CRC;                     /* 存着的身份坏了：不新建、不覆盖，等人看（状态里报） */
    }
    mc_ident_wipe(buf, MC_IDENT_BLOB_MAX);
    free(buf);
    return err;
}

/* matter 分区里有没有绑定记号（= 以前绑过某个身份）。只读；读不了（不是"没有"）回错误 */
static esp_err_t bind_marker_present(bool *present)
{
    *present = false;
    esp_err_t err = nvs_flash_init_partition(MC_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) return ESP_OK;   /* 分区坏了：等于没有，matter_bind 会重建 */
    if (err != ESP_OK) return err;
    nvs_handle_t h;
    uint64_t bound = 0;
    err = nvs_open_from_partition(MC_PART, MC_BIND_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u64(h, MC_BIND_KEY, &bound);
        nvs_close(h);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    *present = bound != 0;
    return ESP_OK;
}

esp_err_t matter_ctrl_forget_binding(void)
{
    esp_err_t err = nvs_flash_init_partition(MC_PART);         /* 已初始化时直接回 OK（NVSPartitionManager::init_partition） */
    if (err != ESP_OK) return err;
    nvs_handle_t h;
    err = nvs_open_from_partition(MC_PART, MC_BIND_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, MC_BIND_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* matter 分区与身份绑定：记号（MC_BIND_NS/MC_BIND_KEY）= 身份的 fabric ID。对不上（新身份、分区坏了、装过别的固件 ——
 * G0-5 原型、M7 第一步都在这里写过别的东西）就整个擦掉、写上新记号：CHIP 的 KVS（fabric 表、控制器自己的 NOC、会话、组数据）
 * 必须属于这个身份。身份在 hubdb，擦 matter 分区不丢设备：控制器用同一个根 CA 重新签自己的 NOC（9.3）。
 * 读不了记号（不是"没有"）就不擦、报错。只碰 matter 分区：主 nvs、fctry（红线 2）、hubdb 一概不碰。启动任务里跑（内部栈，写 flash）。 */
static esp_err_t matter_bind(uint64_t fid, bool *rebound)
{
    *rebound = false;
    esp_err_t err = nvs_flash_init_partition(MC_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "matter 分区要重新格式化（%s）", esp_err_to_name(err));
        err = nvs_flash_erase_partition(MC_PART);
        if (err == ESP_OK) err = nvs_flash_init_partition(MC_PART);
    }
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    uint64_t bound = 0;
    err = nvs_open_from_partition(MC_PART, MC_BIND_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u64(h, MC_BIND_KEY, &bound);
        nvs_close(h);
    }
    if (err == ESP_OK && bound == fid) return ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;   /* 读不了（不是"没有"）：别擦 */

    ESP_LOGW(TAG, "matter 分区不属于当前的 fabric 身份 —— 擦掉重建 CHIP 的状态（身份在 hubdb，设备不受影响）");
    err = nvs_flash_erase_partition(MC_PART);          /* 已初始化的分区它会先反初始化（IDF nvs_flash.h） */
    if (err == ESP_OK) err = nvs_flash_init_partition(MC_PART);
    if (err != ESP_OK) return err;
    err = nvs_open_from_partition(MC_PART, MC_BIND_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u64(h, MC_BIND_KEY, fid);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) *rebound = true;
    return err;
}

/* CHIP 任务里：新建身份（要的话）→ 落盘并读回确认 → 装进签发器 → DeviceControllerFactory + commissioner → 控制器这一侧的 IPK */
static void setup_in_chip_task(intptr_t)
{
    const uint64_t fid = s_ident.fabric_id;
    if (s_ident_need_new) {
        const CHIP_ERROR ge = mc::ident_generate(fid, s_ident);
        if (ge != CHIP_NO_ERROR) {
            mc_ident_wipe(&s_ident, sizeof(s_ident));
            fail("ident_generate", (int32_t)ge.AsInteger());
            return;
        }
        uint8_t *buf = (uint8_t *)calloc(1, MC_IDENT_BLOB_MAX);
        const int n = buf ? mc_ident_encode(&s_ident, buf, MC_IDENT_BLOB_MAX) : -1;
        /* 落盘并读回比对一致之前绝不拿它签任何证书：防抖窗口里断电 = 设备拿着一个已经丢了的根（CHECKLIST MC12） */
        const esp_err_t se = n > 0 ? s_hooks->ident_save(MC_IDENT_VER, buf, (size_t)n) : ESP_ERR_NO_MEM;
        if (buf) {
            mc_ident_wipe(buf, MC_IDENT_BLOB_MAX);
            free(buf);
        }
        if (se != ESP_OK) {
            mc_ident_wipe(&s_ident, sizeof(s_ident));
            fail("ident_save", se);
            return;
        }
        ESP_LOGW(TAG, "新建了 fabric 身份 %016" PRIX64 "（自签根 CA，已落盘并读回确认）", fid);
    }
    const CHIP_ERROR le = s_issuer.load(s_ident);
    mc_ident_wipe(&s_ident, sizeof(s_ident));          /* 签发器有自己的拷贝；这一份里的私钥现在就清 */
    if (le != CHIP_NO_ERROR) {
        fail("ident_load", (int32_t)le.AsInteger());
        return;
    }
    esp_matter::controller::set_custom_credentials_issuer(&s_issuer);
    /* 设备认证的 PAA 用内置快照（T9.53）：setup_commissioner 里取一次信任库交给 DAC 校验器 */
    if (!mc_paa_install()) {
        fail("paa_store", ESP_ERR_NO_MEM);
        return;
    }

    auto &client = matter_controller_client::get_instance();
    esp_err_t err = client.init(kControllerNodeId, fid, kListenPort);
    if (err != ESP_OK) {
        fail("client_init", err);
        return;
    }
    err = client.setup_commissioner();
    if (err != ESP_OK) {
        fail("setup_commissioner", err);
        return;
    }
    /* 控制器这一侧的 IPK = 发给设备的那个（两边不一致 CASE 建不起来）。esp-matter 原本在 setup_commissioner 里写 CHIP 的公开默认值
       与测试组数据，构建期补丁去掉了（tools/patch_esp_matter.py 第 3 条）—— 这里不写就没有 IPK，CASE 一律失败而不是悄悄用测试值 */
    auto *comm = client.get_commissioner();
    uint8_t cfid_bytes[sizeof(uint64_t)] = {};
    chip::MutableByteSpan cfid_span(cfid_bytes);
    CHIP_ERROR ce = comm->GetCompressedFabricIdBytes(cfid_span);
    if (ce == CHIP_NO_ERROR) {
        ce = chip::Credentials::SetSingleIpkEpochKey(chip::Credentials::GetGroupDataProvider(), comm->GetFabricIndex(),
                                                    chip::ByteSpan(s_issuer.ipk(), MC_IDENT_IPK_LEN), cfid_span);
    }
    if (ce != CHIP_NO_ERROR) {
        fail("ipk", (int32_t)ce.AsInteger());
        return;
    }
    /* v1 不做 ICD 注册（9.1）：ICD 按它自己报的间隔被动订阅（9.5） */
    esp_matter::controller::pairing_command::get_instance().set_icd_registration(false);
    mc_comm_on_setup();                                /* 配网 / 移除的回调（mc_comm.cpp） */

    const uint64_t cfid = comm->GetCompressedFabricId();
    const uint32_t after = internal_free();
    portENTER_CRITICAL(&s_mux);
    s_st.compressed_fabric_id = cfid;
    s_st.started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_st.heap_after = after;
    s_st.state = MATTER_CTRL_RUNNING;
    portEXIT_CRITICAL(&s_mux);
    /* esp-matter 在 setup_commissioner 里按编译开关关了测试 CD 密钥：按本地开关重设。放在 RUNNING 之后（审计）：这之后的切换都会投
       ScheduleWork 再设一次；之前的切换已经改了 s_allow_test，这里读到的就是新值。配网 / 移除都排在本函数之后，插不到中间 */
    mc_paa_apply_test();
    ESP_LOGI(TAG, "控制器起来了：fabric %016" PRIX64 "（压缩 %016" PRIX64 "）、节点 %" PRIu64 "，内部 SRAM 空闲 %" PRIu32,
             fid, cfid, kControllerNodeId, after);
}

/* CHIP 的设备层事件（CHIP 任务里调）：第一步不用 */
static void on_chip_event(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    (void)event;
}

static void start_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(MC_START_DELAY_MS));
    s_rtc_tries++;                                     /* 从这里起到撑过稳定期之前崩了，都算一次 */
    portENTER_CRITICAL(&s_mux);
    s_st.state = MATTER_CTRL_STARTING;
    s_st.tries = s_rtc_tries;
    portEXIT_CRITICAL(&s_mux);

    /* "允许测试设备"（本地开关，hubdb mctrl/opts）：起 CHIP 之前读，setup_commissioner 之后设进校验器。读不出来按关（安全的一边） */
    mc_paa_set_allow_test(s_hooks && s_hooks->allow_test_load && s_hooks->allow_test_load());

    bool need_new = false, rebound = false;
    const char *step = "ident_read";
    esp_err_t err = ident_read(&need_new);
    if (err == ESP_OK && need_new) {
        /* hubdb 里没有身份，matter 分区却记着以前绑过一个：身份丢了（hubdb 整区重建过、这一条单独丢了），不是新网关。
           新建 = 换根，之前的设备全部失联 —— 停在这里等用户明确地"重置 Matter 控制器"（审计；恢复出厂全清会先删记号再删身份） */
        bool marked = false;
        err = bind_marker_present(&marked);
        if (err == ESP_OK && marked) {
            step = "ident_lost";
            err = ESP_ERR_NOT_FOUND;
        }
    }
    if (err == ESP_OK && need_new) {
        /* 新身份：fabric ID 先在这里选好（matter 分区按它绑定），密钥对与根证书在 CHIP 任务里生成（要 CHIP 的密码学 PAL） */
        memset(&s_ident, 0, sizeof(s_ident));
        s_ident.fabric_id = rand_fabric_id();
        ESP_LOGW(TAG, "hubdb 里没有 fabric 身份 —— 新建（新网关、恢复出厂清过；之前配进来的设备若有，都成了孤儿）");
    }
    if (err == ESP_OK) {
        step = "matter_partition";
        err = matter_bind(s_ident.fabric_id, &rebound);
    }
    if (err != ESP_OK) {
        mc_ident_wipe(&s_ident, sizeof(s_ident));
        fail(step, err);
        s_rtc_tries = 0;                               /* 没起 CHIP：不算崩溃循环 */
        vTaskDelete(NULL);
        return;
    }
    s_ident_need_new = need_new;
    /* 节点表（hubdb mctrl/nodes）：读不回来控制器照起（已有设备照常 CASE），只是这次开机不配网、不移除（mc_comm.cpp） */
    const esp_err_t ne = mc_nodes_boot_load();
    if (ne != ESP_OK) ESP_LOGE(TAG, "节点表读不回来（%s）—— 这次开机不配网、不移除", esp_err_to_name(ne));
    portENTER_CRITICAL(&s_mux);
    s_st.nodes_ok = ne == ESP_OK;
    portEXIT_CRITICAL(&s_mux);
    const uint32_t before = internal_free();
    portENTER_CRITICAL(&s_mux);
    s_st.fabric_id = s_ident.fabric_id;
    s_st.ident_new = need_new;
    s_st.matter_rebound = rebound;
    s_st.heap_before = before;
    portEXIT_CRITICAL(&s_mux);

    err = esp_matter::start(on_chip_event);
    if (err != ESP_OK) {
        mc_ident_wipe(&s_ident, sizeof(s_ident));      /* CHIP 任务不会来取了：私钥现在就清（审计） */
        fail("esp_matter_start", err);
    } else if (chip::DeviceLayer::PlatformMgr().ScheduleWork(setup_in_chip_task, 0) != CHIP_NO_ERROR) {
        mc_ident_wipe(&s_ident, sizeof(s_ident));
        fail("schedule_setup", ESP_FAIL);
    }

    vTaskDelay(pdMS_TO_TICKS(MC_STABLE_MS));
    s_rtc_tries = 0;                                   /* 撑过稳定期：清零 */
    portENTER_CRITICAL(&s_mux);
    s_st.tries = 0;
    portEXIT_CRITICAL(&s_mux);
    /* esp_matter::start() 里的 InitChipStack（读写 NVS、初始化 CHIP 配置）跑在本任务里：栈留的余量要看实测（审计） */
    ESP_LOGI(TAG, "启动任务的栈最少还剩 %u 字节（共 %d）", (unsigned)uxTaskGetStackHighWaterMark(NULL), MC_TASK_STACK);
    vTaskDelete(NULL);
}

esp_err_t matter_ctrl_start(void)
{
    if (s_started) return ESP_OK;
    const esp_reset_reason_t why = wd_boot_reason_get();
    const bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
    if (s_rtc_magic != MC_MAGIC || !crashed) {         /* 上电（RTC 内容随机）或不是崩溃：计数从零开始 */
        s_rtc_magic = MC_MAGIC;
        s_rtc_tries = 0;
    }
    portENTER_CRITICAL(&s_mux);
    s_st.node_id = kControllerNodeId;
    s_st.vendor_id = (uint16_t)CONFIG_ESP_MATTER_CONTROLLER_VENDOR_ID;
    s_st.tries = s_rtc_tries;
    portEXIT_CRITICAL(&s_mux);
    if (s_rtc_tries >= MC_MAX_TRIES) {
        set_state(MATTER_CTRL_HELD);
        ESP_LOGE(TAG, "前 %" PRIu32 " 次开机起了 Matter 都没撑过 %d s —— 这次不起（断电重上电清零）", s_rtc_tries, MC_STABLE_MS / 1000);
        return ESP_ERR_INVALID_STATE;
    }
    set_state(MATTER_CTRL_WAITING);
    if (xTaskCreate(start_task, "mc_start", MC_TASK_STACK, NULL, MC_TASK_PRIO, NULL) != pdPASS) {
        fail("task_create", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
