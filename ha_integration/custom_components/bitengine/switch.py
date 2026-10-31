"""bitEngine Hub 的开关实体。"""

from __future__ import annotations

from typing import Any

from homeassistant.components.switch import SwitchEntity, SwitchEntityDescription
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .const import API_LED
from .coordinator import BitEngineConfigEntry, BitEngineCoordinator
from .entity import BitEngineEntity

NIGHT_MODE = SwitchEntityDescription(
    key="night_mode",
    translation_key="night_mode",
    entity_category=EntityCategory.CONFIG,
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: BitEngineConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    async_add_entities([BitEngineNightMode(entry.runtime_data)])


class BitEngineNightMode(BitEngineEntity, SwitchEntity):
    """LED 夜间模式总开关。

    ⚠️ 打开只是**启用这个功能**，不等于此刻就熄灯 —— 实际熄不熄由时间窗决定。
    "此刻是否真的在夜间模式"是另一个实体（binary_sensor 的 night_active）。
    这两件事必须分开：网关侧曾经有过一个 bug，正是把"功能开关"直接当成
    "立刻熄灯"，导致精心算好的时间窗从来没起过作用。
    """

    entity_description = NIGHT_MODE

    def __init__(self, coordinator: BitEngineCoordinator) -> None:
        super().__init__(coordinator, NIGHT_MODE.key)

    @property
    def available(self) -> bool:
        # 老固件没有 GET /led/config，此时 led 是空 dict —— 如实标成不可用，
        # 而不是显示一个永远是 off 的假开关。
        return super().available and bool(self.coordinator.data.get("led"))

    @property
    def is_on(self) -> bool | None:
        return self.coordinator.data.get("led", {}).get("night_mode_enabled")

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self.coordinator.async_post(API_LED, {"night_mode_enabled": True})

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.coordinator.async_post(API_LED, {"night_mode_enabled": False})
