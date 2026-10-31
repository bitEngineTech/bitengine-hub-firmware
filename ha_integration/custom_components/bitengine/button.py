"""bitEngine Hub 的按钮实体。"""

from __future__ import annotations

from dataclasses import dataclass

from homeassistant.components.button import (
    ButtonDeviceClass,
    ButtonEntity,
    ButtonEntityDescription,
)
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .const import API_REBOOT, API_ZIGBEE_RESTART
from .coordinator import BitEngineConfigEntry, BitEngineCoordinator
from .entity import BitEngineEntity


@dataclass(frozen=True, kw_only=True)
class BitEngineButtonDescription(ButtonEntityDescription):
    """带请求路径的按钮描述。"""

    path: str


BUTTONS: tuple[BitEngineButtonDescription, ...] = (
    # ⚠️ 重启网关会**断开 Zigbee 连接**。固件从 v0.5.0-165 起会在重启前主动
    # 发 RST，让 ZHA/Z2M 能自己重连（实测约 2/3 概率、25 秒内恢复）；剩下的
    # 情况需要在 HA 里重载 ZHA 集成。所以标成 CONFIG 而不是随手可按。
    BitEngineButtonDescription(
        key="reboot",
        translation_key="reboot",
        device_class=ButtonDeviceClass.RESTART,
        entity_category=EntityCategory.CONFIG,
        path=API_REBOOT,
    ),
    # 复位 Zigbee 芯片。⚠️ 在 AM1 板上它**会连带整板复位**（MG24 复位线接着电源监控，
    # /api/v1/zigbee/status 的 restart_resets_board=true），插着网线时多半卡在下载模式、
    # 要断电重上电才回来（硬件缺陷 X6）。网页上有警告，这里没有地方放警告，所以默认不启用
    # （2026-10-08，T9.19 ⑥ 第二轮审计 E1）；要用的人在实体设置里自己打开。
    # 网关侧有冷却限制，按太频会回 429，那时 HA 会弹一个错误 —— 这是对的，用户该知道没生效。
    BitEngineButtonDescription(
        key="zigbee_restart",
        translation_key="zigbee_restart",
        device_class=ButtonDeviceClass.RESTART,
        entity_category=EntityCategory.CONFIG,
        entity_registry_enabled_default=False,
        path=API_ZIGBEE_RESTART,
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: BitEngineConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(BitEngineButton(coordinator, d) for d in BUTTONS)


class BitEngineButton(BitEngineEntity, ButtonEntity):
    """一个 bitEngine 动作按钮。"""

    entity_description: BitEngineButtonDescription

    def __init__(
        self, coordinator: BitEngineCoordinator, description: BitEngineButtonDescription
    ) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    async def async_press(self) -> None:
        await self.coordinator.async_post(self.entity_description.path)
