"""bitEngine Hub 的二值实体。"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
    BinarySensorEntityDescription,
)
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .coordinator import BitEngineConfigEntry, BitEngineCoordinator
from .entity import BitEngineEntity


@dataclass(frozen=True, kw_only=True)
class BitEngineBinaryDescription(BinarySensorEntityDescription):
    """带取值函数的二值传感器描述。"""

    value_fn: Callable[[dict[str, Any]], bool | None]


BINARY_SENSORS: tuple[BitEngineBinaryDescription, ...] = (
    BitEngineBinaryDescription(
        key="mg24_alive",
        translation_key="mg24_alive",
        device_class=BinarySensorDeviceClass.CONNECTIVITY,
        value_fn=lambda d: d.get("mg24_alive"),
    ),
    # 供电方式。PoE 供电时"拔网线"等于断电 —— 写自动化的人需要知道这台设备
    # 的电源和网络是同一根线。老固件没有 poe_present 字段，value_fn 返回 None，
    # HA 会把实体显示成 unknown 而不是编一个 false 出来。
    BitEngineBinaryDescription(
        key="poe_present",
        translation_key="poe_present",
        device_class=BinarySensorDeviceClass.POWER,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("poe_present"),
    ),
    BitEngineBinaryDescription(
        key="eth_link_up",
        translation_key="eth_link_up",
        device_class=BinarySensorDeviceClass.CONNECTIVITY,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("eth_link_up"),
    ),
    # 安全模式 = 看门狗判定系统不健康后的降级运行。这是"设备出问题了"最权威的
    # 一个信号，所以用 PROBLEM 而不是普通开关语义。
    BitEngineBinaryDescription(
        key="safe_mode",
        translation_key="safe_mode",
        device_class=BinarySensorDeviceClass.PROBLEM,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("safe_mode"),
    ),
    # 夜间模式**此刻是否真的生效**。与 switch 的"功能开关"是两件事：
    # 开关只表示启用，实际熄不熄由时间窗决定，而时间窗依赖设备时区设对。
    # 网关侧曾经把这两者混为一谈，导致时间窗从未起作用 —— 单独暴露这个实体
    # 就是为了让"我开了夜间模式但灯没暗"这类问题一眼可查。
    BitEngineBinaryDescription(
        key="night_active",
        translation_key="night_active",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("led", {}).get("night_active"),
    ),
    # 固件签名。语义取反成 PROBLEM：**没有**签名才是问题 —— 那种机器会拒绝
    # 一切后续 OTA，只能拆机串口重刷，而且从外面完全看不出来。
    # 老固件不带这个字段，返回 None 让实体显示 unknown 而不是误报"有问题"。
    BitEngineBinaryDescription(
        key="firmware_unsigned",
        translation_key="firmware_unsigned",
        device_class=BinarySensorDeviceClass.PROBLEM,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: (not d["app_signed"]) if "app_signed" in d else None,
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: BitEngineConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(BitEngineBinarySensor(coordinator, d) for d in BINARY_SENSORS)


class BitEngineBinarySensor(BitEngineEntity, BinarySensorEntity):
    """一个 bitEngine 二值传感器。"""

    entity_description: BitEngineBinaryDescription

    def __init__(
        self, coordinator: BitEngineCoordinator, description: BitEngineBinaryDescription
    ) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def is_on(self) -> bool | None:
        return self.entity_description.value_fn(self.coordinator.data or {})
