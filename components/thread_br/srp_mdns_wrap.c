/* components/thread_br/srp_mdns_wrap.c — 诊断构建专用（CONFIG_BITENGINE_DIAG_SRP_INJECT，出厂编出来是空的）：
 * 给 SRP advertising proxy 调的那几个 mdns 写接口计时。
 *
 * 为什么要它（2026-10-07）：注册潮压测里 OT 任务处理 SRP 注册慢到把 IDLE 饿死、任务看门狗 panic（coredump 里占着
 * CPU 的是 ot_main；数据见 docs/test_cases_scale_baseline.md「SRP 代播满载压测」）。要分清时间花在 SRP 自身（解析、验签）
 * 还是代播调用的 mdns 上 —— 后者在 PCB 正在探测时会把全部探测中的服务重拼一遍包（O(P²)），而且就在
 * 调用方（OT 任务）里做。代播库是预编译的（libopenthread_br.a），改不了它，只能在链接时用 --wrap 截住。
 *
 * 怎么接上的：thread_br/CMakeLists.txt 在同一个 Kconfig 下给链接器加 -Wl,--wrap=<函数>（清单与下面的
 * __wrap_ 一一对应）。--wrap 只截**别的目标文件**对这些符号的未定义引用：代播库（libopenthread_br.a）的调用都会被截住；
 * 我们自己的 mdns_svc 调的是不带 _for_host 的版本，它们在 mdns_responder.c 同一个目标文件里转调，不经过这里。
 *
 * 计的是**墙钟**：mdns_delegate_hostname_add 要等 mdns 任务处理完（action_sema，portMAX_DELAY），
 * 那段等待也算在里面 —— 正是"OT 任务被 mdns 卡住"的时长。 */
#include "sdkconfig.h"

#if CONFIG_BITENGINE_DIAG_SRP_INJECT
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "thread_br_diag.h"

enum {
    W_DELEGATE_ADD, W_DELEGATE_SET_ADDR, W_DELEGATE_REMOVE,
    W_SERVICE_ADD, W_SERVICE_REMOVE, W_PORT_SET, W_TXT_SET, W_SUBTYPE_UPDATE,
    W_OT_ECDSA_VERIFY, W_ESP_HW_ECDSA_VERIFY,
    W_COUNT
};
static const char *const k_names[W_COUNT] = {
    "delegate_hostname_add", "delegate_hostname_set_address", "delegate_hostname_remove",
    "service_add_for_host", "service_remove_for_host", "service_port_set_for_host",
    "service_txt_set_for_host", "service_subtype_update_multiple_items_for_host",
    /* 不是 mdns，但同一个问题：SRP 注册在 OT 任务里的时间，除了 mdns 还花在哪 —— 是不是验签？OT 走 PSA；
       ESP 的硬件验签是 PSA 驱动，第二个计数为 0 就说明根本没派发到硬件（10-07 实测是派发了的）。 */
    "ot_ecdsa_verify", "esp_hw_ecdsa_verify",
};
static uint32_t s_n[W_COUNT], s_total_us[W_COUNT], s_max_us[W_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void account(int w, int64_t t0)
{
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    portENTER_CRITICAL(&s_mux);
    s_n[w]++;
    s_total_us[w] += dt;
    if (dt > s_max_us[w]) s_max_us[w] = dt;
    portEXIT_CRITICAL(&s_mux);
}

size_t thread_br_diag_mdns_calls(thread_br_diag_mdns_call_t *out, size_t max)
{
    if (!out) return 0;
    size_t n = max < W_COUNT ? max : W_COUNT;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        out[i].name = k_names[i];
        out[i].n = s_n[i];
        out[i].total_us = s_total_us[i];
        out[i].max_us = s_max_us[i];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* ---- mdns 任务里每个动作的耗时（tools/patch_mdns.py 在诊断构建里给 service_task 加的钩子调这里）----
   类型号是 mdns_private.h 的 mdns_action_type_t（1.11.0）；超出范围的记到 OTHER。
   为什么要它：258 条代播服务时应答一次 _matter._tcp 浏览查询就把 mdns 任务占了几秒（coredump），
   要知道是哪类动作、单次最长多久。
   16 / 17 不是动作：补丁 SCALE ⑧ 的让拍（YIELD = 让了一次，记睡了多久；YIELD_NOIDLE = 睡满上限 IDLE 也没轮到）。 */
#define MDNS_ACT_TYPES 18
#define MDNS_ACT_OTHER 15
static const char *const k_act_names[MDNS_ACT_TYPES] = {
    "SYSTEM_EVENT", "HOSTNAME_SET", "INSTANCE_SET", "SEARCH_ADD", "SEARCH_SEND", "SEARCH_END",
    "BROWSE_ADD", "BROWSE_SYNC", "BROWSE_END", "TX_HANDLE", "RX_HANDLE", "TASK_STOP",
    "DELEGATE_HOSTNAME_ADD", "DELEGATE_HOSTNAME_REMOVE", "DELEGATE_HOSTNAME_SET_ADDR", "OTHER",
    "YIELD", "YIELD_NOIDLE",
};
static uint32_t s_act_n[MDNS_ACT_TYPES], s_act_total_us[MDNS_ACT_TYPES], s_act_max_us[MDNS_ACT_TYPES];

void bitengine_diag_mdns_action(int type, uint32_t us)
{
    if (type < 0 || type >= MDNS_ACT_TYPES) type = MDNS_ACT_OTHER;
    portENTER_CRITICAL(&s_mux);
    s_act_n[type]++;
    s_act_total_us[type] += us;
    if (us > s_act_max_us[type]) s_act_max_us[type] = us;
    portEXIT_CRITICAL(&s_mux);
}

size_t thread_br_diag_mdns_actions(thread_br_diag_mdns_call_t *out, size_t max)
{
    if (!out) return 0;
    size_t n = max < MDNS_ACT_TYPES ? max : MDNS_ACT_TYPES;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        out[i].name = k_act_names[i];
        out[i].n = s_act_n[i];
        out[i].total_us = s_act_total_us[i];
        out[i].max_us = s_act_max_us[i];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void thread_br_diag_mdns_calls_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_n, 0, sizeof(s_n));
    memset(s_total_us, 0, sizeof(s_total_us));
    memset(s_max_us, 0, sizeof(s_max_us));
    memset(s_act_n, 0, sizeof(s_act_n));
    memset(s_act_total_us, 0, sizeof(s_act_total_us));
    memset(s_act_max_us, 0, sizeof(s_act_max_us));
    portEXIT_CRITICAL(&s_mux);
}

/* ---- 被截住的函数：签名必须与 mdns.h 逐字一致（编译器会对 __real_ 的声明与调用做检查） ---- */
esp_err_t __real_mdns_delegate_hostname_add(const char *hostname, const mdns_ip_addr_t *address_list);
esp_err_t __wrap_mdns_delegate_hostname_add(const char *hostname, const mdns_ip_addr_t *address_list)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_mdns_delegate_hostname_add(hostname, address_list);
    account(W_DELEGATE_ADD, t0);
    return r;
}

esp_err_t __real_mdns_delegate_hostname_set_address(const char *hostname, const mdns_ip_addr_t *address_list);
esp_err_t __wrap_mdns_delegate_hostname_set_address(const char *hostname, const mdns_ip_addr_t *address_list)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_mdns_delegate_hostname_set_address(hostname, address_list);
    account(W_DELEGATE_SET_ADDR, t0);
    return r;
}

esp_err_t __real_mdns_delegate_hostname_remove(const char *hostname);
esp_err_t __wrap_mdns_delegate_hostname_remove(const char *hostname)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_mdns_delegate_hostname_remove(hostname);
    account(W_DELEGATE_REMOVE, t0);
    return r;
}

esp_err_t __real_mdns_service_add_for_host(const char *instance_name, const char *service_type, const char *proto,
                                           const char *hostname, uint16_t port, mdns_txt_item_t txt[], size_t num_items);
esp_err_t __wrap_mdns_service_add_for_host(const char *instance_name, const char *service_type, const char *proto,
                                           const char *hostname, uint16_t port, mdns_txt_item_t txt[], size_t num_items)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_mdns_service_add_for_host(instance_name, service_type, proto, hostname, port, txt, num_items);
    account(W_SERVICE_ADD, t0);
    return r;
}

esp_err_t __real_mdns_service_remove_for_host(const char *instance, const char *service_type, const char *proto,
                                              const char *hostname);
esp_err_t __wrap_mdns_service_remove_for_host(const char *instance, const char *service_type, const char *proto,
                                              const char *hostname)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_mdns_service_remove_for_host(instance, service_type, proto, hostname);
    account(W_SERVICE_REMOVE, t0);
    return r;
}

esp_err_t __real_mdns_service_port_set_for_host(const char *instance, const char *service_type, const char *proto,
                                                const char *hostname, uint16_t port);
esp_err_t __wrap_mdns_service_port_set_for_host(const char *instance, const char *service_type, const char *proto,
                                                const char *hostname, uint16_t port)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_mdns_service_port_set_for_host(instance, service_type, proto, hostname, port);
    account(W_PORT_SET, t0);
    return r;
}

esp_err_t __real_mdns_service_txt_set_for_host(const char *instance, const char *service_type, const char *proto,
                                               const char *hostname, mdns_txt_item_t txt[], uint8_t num_items);
esp_err_t __wrap_mdns_service_txt_set_for_host(const char *instance, const char *service_type, const char *proto,
                                               const char *hostname, mdns_txt_item_t txt[], uint8_t num_items)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_mdns_service_txt_set_for_host(instance, service_type, proto, hostname, txt, num_items);
    account(W_TXT_SET, t0);
    return r;
}

esp_err_t __real_mdns_service_subtype_update_multiple_items_for_host(const char *instance_name, const char *service_type,
                                                                     const char *proto, const char *hostname,
                                                                     mdns_subtype_item_t subtype[], uint8_t num_items);
esp_err_t __wrap_mdns_service_subtype_update_multiple_items_for_host(const char *instance_name, const char *service_type,
                                                                     const char *proto, const char *hostname,
                                                                     mdns_subtype_item_t subtype[], uint8_t num_items)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_mdns_service_subtype_update_multiple_items_for_host(instance_name, service_type, proto,
                                                                             hostname, subtype, num_items);
    account(W_SUBTYPE_UPDATE, t0);
    return r;
}
/* OT 的验签计时：__wrap_otPlatCryptoEcdsaVerify 在生产代码 ot_cpu_guard.c 里（过载让出 CPU），
   诊断构建由它回调这里记账 —— 同一个符号只能被 wrap 一次。 */
void thread_br_diag_account_ecdsa(int64_t t0)
{
    account(W_OT_ECDSA_VERIFY, t0);
}

/* psa_crypto_driver_esp_ecdsa.h 的原型；这里不拉 PSA 头（include 路径牵连太多），按 ABI 等价写：
   psa_status_t = int32_t，psa_algorithm_t = uint32_t，attributes 只透传指针。链接器不核类型，改原型时两边一起看。 */
int32_t __real_esp_ecdsa_transparent_verify_hash(const void *attributes, const uint8_t *key_buffer, size_t key_buffer_size,
                                                 uint32_t alg, const uint8_t *hash, size_t hash_length,
                                                 const uint8_t *signature, size_t signature_length);
int32_t __wrap_esp_ecdsa_transparent_verify_hash(const void *attributes, const uint8_t *key_buffer, size_t key_buffer_size,
                                                 uint32_t alg, const uint8_t *hash, size_t hash_length,
                                                 const uint8_t *signature, size_t signature_length)
{
    int64_t t0 = esp_timer_get_time();
    int32_t r = __real_esp_ecdsa_transparent_verify_hash(attributes, key_buffer, key_buffer_size, alg, hash, hash_length,
                                                         signature, signature_length);
    account(W_ESP_HW_ECDSA_VERIFY, t0);
    return r;
}
#endif /* CONFIG_BITENGINE_DIAG_SRP_INJECT */
