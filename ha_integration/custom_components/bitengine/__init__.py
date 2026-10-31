"""bitEngine Hub 集成入口。"""

from __future__ import annotations

from homeassistant.const import Platform
from homeassistant.core import HomeAssistant

from .coordinator import BitEngineConfigEntry, BitEngineCoordinator

PLATFORMS: list[Platform] = [
    Platform.BINARY_SENSOR,
    Platform.BUTTON,
    Platform.NUMBER,
    Platform.SENSOR,
    Platform.SWITCH,
]


async def async_setup_entry(hass: HomeAssistant, entry: BitEngineConfigEntry) -> bool:
    """建立一个网关条目。"""
    coordinator = BitEngineCoordinator(hass, entry)
    # 首次刷新失败会抛 ConfigEntryNotReady（HA 自动退避重试）或
    # ConfigEntryAuthFailed（弹重新认证）。两者都比"加载成功但实体全空"好。
    await coordinator.async_config_entry_first_refresh()

    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: BitEngineConfigEntry) -> bool:
    """卸载条目。

    ⚠️ 这里刻意不做任何"关闭连接"的收尾：aiohttp session 是 HA 共享的，
    协调器没有自己的长连接或后台线程。多余的拆解正是 ZHA 那个
    unload_in_progress 死锁的来源 —— 卸载路径越简单越不会卡住。
    """
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
