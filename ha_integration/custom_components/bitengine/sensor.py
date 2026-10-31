"""bitEngine Hub 的传感器实体。"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.const import (
    EntityCategory,
    UnitOfInformation,
    UnitOfTemperature,
    UnitOfTime,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.helpers.typing import StateType

from .coordinator import BitEngineConfigEntry, BitEngineCoordinator
from .entity import BitEngineEntity


@dataclass(frozen=True, kw_only=True)
class BitEngineSensorDescription(SensorEntityDescription):
    """带取值函数的传感器描述。"""

    value_fn: Callable[[dict[str, Any]], StateType]


SENSORS: tuple[BitEngineSensorDescription, ...] = (
    BitEngineSensorDescription(
        key="temperature",
        translation_key="temperature",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda d: d.get("temperature"),
    ),
    BitEngineSensorDescription(
        key="uptime",
        translation_key="uptime",
        device_class=SensorDeviceClass.DURATION,
        native_unit_of_measurement=UnitOfTime.SECONDS,
        suggested_unit_of_measurement=UnitOfTime.DAYS,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("uptime"),
    ),
    # 连着桥的协调器客户端数（Z2M / ZHA）。这是"Zigbee 到底通不通"最直接的
    # 一个数：网关重启后 ZHA 卡死不自愈时，它会停在 0 —— 见网关侧那条已知问题。
    BitEngineSensorDescription(
        key="tcp_clients",
        translation_key="tcp_clients",
        state_class=SensorStateClass.MEASUREMENT,
        value_fn=lambda d: d.get("tcp_clients"),
    ),
    BitEngineSensorDescription(
        key="wifi_rssi",
        translation_key="wifi_rssi",
        device_class=SensorDeviceClass.SIGNAL_STRENGTH,
        native_unit_of_measurement="dBm",
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        # 走以太网时 RSSI 没有意义，报 None 让实体显示 unknown 而不是一个假数字
        value_fn=lambda d: d.get("wifi_rssi") if d.get("wifi_connected") else None,
    ),
    # ⚠️ 用 internal_free_heap 而不是 free_heap。后者是**含 8MB PSRAM 的聚合值**，
    # 永远好看（8MB 出头），对判断内存健康毫无意义；真正会耗尽的是内部 SRAM，
    # 只有十几 KB 余量。网关自己的文档专门强调过这一点。
    BitEngineSensorDescription(
        key="internal_free_heap",
        translation_key="internal_free_heap",
        device_class=SensorDeviceClass.DATA_SIZE,
        native_unit_of_measurement=UnitOfInformation.BYTES,
        suggested_unit_of_measurement=UnitOfInformation.KIBIBYTES,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("internal_free_heap"),
    ),
    BitEngineSensorDescription(
        key="zigbee_channel",
        translation_key="zigbee_channel",
        entity_category=EntityCategory.DIAGNOSTIC,
        # ⚠️ 这是**用户声明值**，不是实测值 —— 本架构读不到 NCP 的真实信道
        # （纯字节流透传，不做 EZSP 主机）。channel_source 会说明是 declared
        # 还是 unknown，未声明时不报数字，免得给人"这就是实际信道"的错觉。
        value_fn=lambda d: (
            d.get("zigbee_channel") if d.get("zigbee_channel_source") == "declared" else None
        ),
    ),
    BitEngineSensorDescription(
        key="active_interface",
        translation_key="active_interface",
        device_class=SensorDeviceClass.ENUM,
        options=["eth", "wifi", "softap", "none"],
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda d: d.get("active_interface"),
    ),
    BitEngineSensorDescription(
        key="ncp_version",
        translation_key="ncp_version",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value_fn=lambda d: d.get("ncp_version"),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: BitEngineConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(BitEngineSensor(coordinator, d) for d in SENSORS)


class BitEngineSensor(BitEngineEntity, SensorEntity):
    """一个 bitEngine 传感器。"""

    entity_description: BitEngineSensorDescription

    def __init__(
        self, coordinator: BitEngineCoordinator, description: BitEngineSensorDescription
    ) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> StateType:
        return self.entity_description.value_fn(self.coordinator.data or {})
