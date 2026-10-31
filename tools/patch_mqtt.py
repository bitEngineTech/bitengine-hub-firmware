#!/usr/bin/env python3
"""给 managed_components/espressif__mqtt 打补丁：mqtt_task 的栈放 PSRAM（T9.39，2026-10-08）。

── 为什么需要 ────────────────────────────────────────────────────────────
开着 MQTT 时内部 SRAM 常驻少约 10 KB，跌破看门狗水位告警（内部堆 < 8 KB），每次开机都报一次
memory_low / heap_low。10-08 在板实测（堆水位标记）这 10 KB 是：
  · mqtt_task 的栈 6144 字节 —— esp-mqtt 用 xTaskCreate / xTaskCreatePinnedToCore 建，栈只能落内部
    （DMA 池跟着少 6.3 KB）；
  · 收发缓冲区各 1024 字节 —— 不在这里改，在 components/mqtt_client/bitengine_mqtt.c 把 buffer.size
    设成 > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL(1024)，malloc 就自己去 PSRAM 了；
  · 其余（客户端结构体、配置副本、连接）约 1.3 KB，留在内部。
组件没有"栈放外部内存"的配置项，只能改建任务那一行。

── 为什么 PSRAM 栈对 mqtt_task 是安全的（psram_task.h 的两条前提）────────
1. 这个任务自己不做 flash / NVS 操作：它跑 esp-mqtt 的收发循环和我们注册的事件回调
   （bitengine_mqtt.c 的 mqtt_event_handler：只发消息、拼字符串、补发事件）；outbox 在 RAM。
   components/mqtt_client/VERIFY.sh 钉着"回调里不碰 nvs_ / config_mgr_set"。
2. 它会自删（客户端 stop 时任务函数末尾 vTaskDelete(NULL)），而 WithCaps 建的任务必须用
   vTaskDeleteWithCaps 删，否则栈和 TCB 都泄漏。IDF v6.0.2 的 vTaskDeleteWithCaps 支持删自己
   （idf_additions.c：另起一个临时任务来回收）。补丁按"栈是不是在外部内存"二选一，PSRAM 分配失败
   退回内部栈时仍走原来的 vTaskDelete。

── 用法 ──────────────────────────────────────────────────────────────────
  python3 tools/patch_mqtt.py --component-dir managed_components/espressif__mqtt          # 打（幂等）
  python3 tools/patch_mqtt.py --component-dir managed_components/espressif__mqtt --check  # 只查
顶层 CMakeLists.txt 在构建期调用它（同 patch_mdns.py：managed_components/ 被 .gitignore 挡着，
改在那里不入库、fullclean 即失）。用原文精确匹配，上游一改就报错停下，不会打歪。
"""
import argparse
import pathlib
import sys

MARK = "BITENGINE_PATCH_T939"
CAPS = "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT"

PATCHES = [
    (
        "include",
        '#include "esp_heap_caps.h"\n',
        '#include "esp_heap_caps.h"\n'
        f'#include "esp_memory_utils.h"   /* {MARK}：mqtt_task 退出时判断栈在不在 PSRAM */\n',
    ),
    (
        "create_pinned",
        '    if (xTaskCreatePinnedToCore(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, '
        'client->config->task_prio, &client->task_handle, MQTT_TASK_CORE) != pdTRUE) {\n',
        f'    /* {MARK}：栈放 PSRAM（tools/patch_mqtt.py），分配失败退回内部 */\n'
        '    if (xTaskCreatePinnedToCoreWithCaps(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, '
        f'client->config->task_prio, &client->task_handle, MQTT_TASK_CORE, {CAPS}) != pdPASS &&\n'
        '        xTaskCreatePinnedToCore(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, '
        'client->config->task_prio, &client->task_handle, MQTT_TASK_CORE) != pdTRUE) {\n',
    ),
    (
        "create",
        '    if (xTaskCreate(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, '
        'client->config->task_prio, &client->task_handle) != pdTRUE) {\n',
        f'    /* {MARK}：栈放 PSRAM（tools/patch_mqtt.py），分配失败退回内部 */\n'
        '    if (xTaskCreateWithCaps(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, '
        f'client->config->task_prio, &client->task_handle, {CAPS}) != pdPASS &&\n'
        '        xTaskCreate(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, '
        'client->config->task_prio, &client->task_handle) != pdTRUE) {\n',
    ),
    (
        "delete",
        '    xEventGroupSetBits(client->status_bits, STOPPED_BIT);\n\n    vTaskDelete(NULL);\n}\n',
        '    xEventGroupSetBits(client->status_bits, STOPPED_BIT);\n\n'
        f'    /* {MARK}：WithCaps 建的任务必须 WithCaps 删，否则 PSRAM 栈和 TCB 泄漏（IDF v6.0.2 支持删自己） */\n'
        '    if (esp_ptr_external_ram(xTaskGetStackStart(NULL))) {\n'
        '        vTaskDeleteWithCaps(NULL);\n'
        '    } else {\n'
        '        vTaskDelete(NULL);\n'
        '    }\n}\n',
    ),
]


def run(component_dir: pathlib.Path, check_only: bool) -> int:
    src_path = component_dir / "mqtt_client.c"
    if not src_path.is_file():
        print(f"patch_mqtt: 找不到 {src_path}", file=sys.stderr)
        return 1
    src = src_path.read_text(encoding="utf-8")

    done = [name for name, _, new in PATCHES if src.count(new) == 1]
    if len(done) == len(PATCHES):
        print(f"patch_mqtt: 已打（{len(PATCHES)} 处，mqtt_task 栈在 PSRAM）")
        return 0
    if check_only:
        print(f"patch_mqtt: 未打或不完整（已打：{done or '无'}）", file=sys.stderr)
        return 1
    if done:
        print(f"patch_mqtt: 只打了一部分（{done}），文件状态不明 —— 删掉 managed_components/espressif__mqtt "
              f"让组件管理器重新下载后再构建", file=sys.stderr)
        return 1

    for name, old, new in PATCHES:
        n = src.count(old)
        if n != 1:
            print(f"patch_mqtt: 「{name}」原文匹配到 {n} 处（应为 1）—— 上游 esp-mqtt 改了，先读新版再改补丁",
                  file=sys.stderr)
            return 1
        src = src.replace(old, new)
    src_path.write_text(src, encoding="utf-8")
    print(f"patch_mqtt: 已打 {len(PATCHES)} 处（mqtt_task 栈放 PSRAM，T9.39）")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--component-dir", required=True, help="managed_components/espressif__mqtt 的路径")
    ap.add_argument("--check", action="store_true", help="只检查是否已打，不改文件")
    a = ap.parse_args()
    return run(pathlib.Path(a.component_dir), a.check)


if __name__ == "__main__":
    sys.exit(main())
