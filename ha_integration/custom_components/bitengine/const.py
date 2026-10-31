"""bitEngine Hub 集成的常量。"""

from __future__ import annotations

from datetime import timedelta
import logging

DOMAIN = "bitengine"
LOGGER = logging.getLogger(__package__)

# 轮询周期。网关的 /api/v1/status 很便宜（一次 snprintf），但 webui 是**单 httpd
# 任务**，轮询太密会和用户自己开网页抢。30s 与设备自带的 MQTT 上报周期一致。
SCAN_INTERVAL = timedelta(seconds=30)

# 单次请求超时。网关在 WiFi 上偶发几百毫秒抖动，10s 足够宽松；
# 再长就会让 HA 的协调器卡住，反而掩盖"设备真的没了"。
REQUEST_TIMEOUT = 10

CONF_PASSWORD = "password"

# 网关的 Basic 认证**只校验冒号后那半**（固件 auth.c 的 check_basic_auth），
# 用户名填什么都行。这里固定一个可读的值，方便用户在网关日志里认出来源。
AUTH_USERNAME = "homeassistant"

API_STATUS = "/api/v1/status"
API_REBOOT = "/api2/reboot"
API_ZIGBEE_RESTART = "/api/v1/zigbee/restart"
# LED 配置：GET 读全量，POST 支持**部分更新**（只发要改的字段，其余保持原值）。
# 一个接口同时管亮度和夜间模式，实测确认（2026-08-20）。
API_LED = "/api/v1/led/config"
