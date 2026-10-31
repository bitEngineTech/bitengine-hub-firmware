"""bitEngine Hub 的数值实体（LED 亮度）。"""

from __future__ import annotations

from homeassistant.components.number import NumberEntity, NumberEntityDescription, NumberMode
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .const import API_LED
from .coordinator import BitEngineConfigEntry, BitEngineCoordinator
from .entity import BitEngineEntity

BRIGHTNESS = NumberEntityDescription(
    key="led_brightness",
    translation_key="led_brightness",
    native_min_value=0,
    native_max_value=255,
    native_step=1,
    mode=NumberMode.SLIDER,
    entity_category=EntityCategory.CONFIG,
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: BitEngineConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    async_add_entities([BitEngineBrightness(entry.runtime_data)])


class BitEngineBrightness(BitEngineEntity, NumberEntity):
    """LED 亮度（0-255）。

    做成滑块而不是"禁用 LED"那种布尔：网关侧本来就是 0-255 的连续量，
    压成布尔会丢掉用户自定义的亮度 —— SMLIGHT 兼容层就得在关灯时把原亮度
    偷偷记在 RAM 里再还原，那是被接口形状逼出来的将就。这里没这个问题。
    0 = 全灭。
    """

    entity_description = BRIGHTNESS

    def __init__(self, coordinator: BitEngineCoordinator) -> None:
        super().__init__(coordinator, BRIGHTNESS.key)

    @property
    def available(self) -> bool:
        return super().available and bool(self.coordinator.data.get("led"))

    @property
    def native_value(self) -> float | None:
        return self.coordinator.data.get("led", {}).get("brightness")

    async def async_set_native_value(self, value: float) -> None:
        await self.coordinator.async_post(API_LED, {"brightness": int(value)})
