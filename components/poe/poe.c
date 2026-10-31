/* PoE 供电检测 —— 读 POE_DETECT(IO0) 的数字电平。
 *
 * 2026-08-13 实测确认：插着 PoE 冷启动 5 次均正常进应用，**IO0 的 strapping
 * 风险已排除**。这次判定比 IO28 那次结论更硬 —— IO28 的失败取决于复位瞬间有无
 * 挂起中断，本质是间歇的；而 IO0 上是稳定直流电平，strapping 要么锁存要么不锁存，
 * 行为确定，5/5 正常即可定论。
 */

#include "poe.h"
#include "devkit_pins.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "poe";
static bool s_ready;

/* 切换上下拉后要等电平稳定再采样。分压网络戴维南电阻约 40K，与引脚和走线的
   几十 pF 构成 RC，时间常数在微秒量级；给 2ms 是极宽裕的余量，而这个函数只在
   诊断时调用，不在意耗时。 */
#define SETTLE_MS 2

static esp_err_t configure(gpio_pull_mode_t pull)
{
    /* 只配输入。外部是来自 5V 轨的分压，配成输出就是内部驱动器对顶外部电路。 */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << POE_DETECT_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = (pull == GPIO_PULLUP_ONLY)   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == GPIO_PULLDOWN_ONLY) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t poe_init(void)
{
    esp_err_t err = configure(GPIO_FLOATING);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POE_DETECT(IO%d) 配置失败: %s", POE_DETECT_PIN, esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "PoE 检测就绪：IO%d %s（分压 R96 68K/R97 100K，比值 0.595）",
             POE_DETECT_PIN, gpio_get_level(POE_DETECT_PIN) ? "高 = PoE 供电" : "低 = 无 PoE");
    return ESP_OK;
}

bool poe_is_present(void)
{
    if (!s_ready) return false;
    return gpio_get_level(POE_DETECT_PIN) != 0;
}

esp_err_t poe_probe(bool *no_pull, bool *pull_up, bool *pull_down)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_OK;
    struct { gpio_pull_mode_t mode; bool *out; } steps[] = {
        { GPIO_FLOATING,      no_pull   },
        { GPIO_PULLUP_ONLY,   pull_up   },
        { GPIO_PULLDOWN_ONLY, pull_down },
    };

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        err = configure(steps[i].mode);
        if (err != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
        if (steps[i].out) *steps[i].out = gpio_get_level(POE_DETECT_PIN) != 0;
    }

    /* 无论成功与否都恢复成无上下拉 —— 内部上拉留着会让 poe_is_present()
       在无 PoE 时读到不确定区的电平（见 poe.h 的说明）。 */
    configure(GPIO_FLOATING);
    return err;
}
