/* 层 H — 故障注入（docs/04_watchdog_design_v2.md 第十一节）
 *
 * v1 最大的缺失不是少了一层看门狗，而是**没有人证明过看门狗会响**。
 * 堆告警写了、堆重启写了、泄漏检测写了，三条路径都不可能触发，跑了几个月
 * 没人发现——因为从来没有一个用例去抽干内存看它响不响。
 *
 * 这个文件就是补这一课。默认不编译（CONFIG_BITENGINE_FAULT_INJECTION=n），
 * 出厂固件里不存在这些入口。
 */

#include "watchdog.h"

#if CONFIG_BITENGINE_FAULT_INJECTION

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "wd_fault";

#define DRAIN_MAX_BLOCKS 64

static void       *s_drain[DRAIN_MAX_BLOCKS];
static int         s_drain_count;
static volatile bool s_mg24_mute;
static volatile bool s_thread_freeze;

bool wd_fault_active(const char *kind)
{
    if (!kind) return false;
    if (!strcmp(kind, "mg24_mute"))          return s_mg24_mute;
    if (!strcmp(kind, "thread_radio_freeze")) return s_thread_freeze;
    return false;
}

/* task_hang：挂起目标任务。用 vTaskSuspend 而不是死循环——死循环会饿死
   同优先级任务，测的就不是"单任务停转"而是"CPU 被霸占"了（那是 F1）。 */
static esp_err_t inject_task_hang(const char *target, uint32_t ms)
{
    if (!target) return ESP_ERR_INVALID_ARG;
    TaskHandle_t h = xTaskGetHandle(target);
    if (!h) {
        ESP_LOGE(TAG, "找不到任务 %s", target);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "注入: 挂起任务 %s %"PRIu32"ms", target, ms);
    vTaskSuspend(h);

    /* 用 esp_timer 恢复，避免占一个任务；回调在 timer 任务上下文，
       vTaskResume 在那里调用是安全的（非阻塞）。 */
    esp_timer_handle_t t;
    esp_timer_create_args_t args = {
        .callback = (esp_timer_cb_t)vTaskResume,
        .arg      = h,
        .name     = "unhang",
        .dispatch_method = ESP_TIMER_TASK,
    };
    if (esp_timer_create(&args, &t) != ESP_OK) {
        vTaskResume(h);            /* 创建失败就别把任务永久挂死 */
        return ESP_FAIL;
    }
    esp_timer_start_once(t, (uint64_t)ms * 1000);
    return ESP_OK;
}

/* heap_drain：按 caps 抽干内存，直到分配失败。这是唯一能证明层 D 会响的手段。 */
static esp_err_t inject_heap_drain(const char *caps_str, uint32_t bytes)
{
    uint32_t caps = MALLOC_CAP_DEFAULT;
    if (caps_str && !strcmp(caps_str, "dma"))      caps = MALLOC_CAP_DMA;
    if (caps_str && !strcmp(caps_str, "internal")) caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (caps_str && !strcmp(caps_str, "spiram"))   caps = MALLOC_CAP_SPIRAM;

    if (bytes == 0) bytes = 8192;
    uint32_t got = 0;
    while (s_drain_count < DRAIN_MAX_BLOCKS && got < bytes) {
        void *p = heap_caps_malloc(4096, caps);
        if (!p) break;                       /* 抽到失败为止，这正是目的 */
        s_drain[s_drain_count++] = p;
        got += 4096;
    }
    ESP_LOGW(TAG, "注入: 占住 %"PRIu32" 字节 [%s]，共 %d 块",
             got, caps_str ? caps_str : "default", s_drain_count);
    return ESP_OK;
}

/* panic（T9.27 验收用）：0.5 s 后在 esp_timer 回调里 abort()，让 HTTP 回包先发出去。
   复位原因是 ESP_RST_PANIC；main.c 会再补一次 esp_restart()，第二次开机 esp_reset_reason() 是 SW，
   /status 的 last_reset_reason 应仍报 4（PANIC），异常开机告警带 reason=panic。
   panic 后走的是软件复位，不重采 strap —— 以太网在线也不会卡进下载模式（X6 只怕芯片级复位）。 */
static void panic_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "注入: panic（abort）");
    abort();
}

static esp_err_t inject_panic(void)
{
    static esp_timer_handle_t s_panic_timer;
    if (!s_panic_timer) {
        const esp_timer_create_args_t args = { .callback = panic_cb, .name = "wd_panic" };
        esp_err_t err = esp_timer_create(&args, &s_panic_timer);
        if (err != ESP_OK) return err;
    }
    ESP_LOGW(TAG, "注入: 0.5 s 后 panic");
    return esp_timer_start_once(s_panic_timer, 500 * 1000);
}

esp_err_t wd_fault_inject(const char *kind, const char *target, uint32_t arg)
{
    if (!kind) return ESP_ERR_INVALID_ARG;

    if (!strcmp(kind, "task_hang"))   return inject_task_hang(target, arg ? arg : 30000);
    if (!strcmp(kind, "heap_drain"))  return inject_heap_drain(target, arg);
    if (!strcmp(kind, "mg24_mute"))   { s_mg24_mute = true;
                                        ESP_LOGW(TAG, "注入: MG24 静默"); return ESP_OK; }
    if (!strcmp(kind, "thread_radio_freeze")) { s_thread_freeze = true;
                                        ESP_LOGW(TAG, "注入: Thread 射频冻结"); return ESP_OK; }
    if (!strcmp(kind, "panic"))       return inject_panic();
    if (!strcmp(kind, "httpd_block")) {
        /* httpd 是单任务串行的，挂起它就等价于被滞留连接堵住 */
        return inject_task_hang("httpd", arg ? arg : 120000);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void wd_fault_clear_all(void)
{
    s_mg24_mute     = false;
    s_thread_freeze = false;
    while (s_drain_count > 0) {
        heap_caps_free(s_drain[--s_drain_count]);
        s_drain[s_drain_count] = NULL;
    }
    ESP_LOGW(TAG, "注入: 全部清除");
}

#endif /* CONFIG_BITENGINE_FAULT_INJECTION */
