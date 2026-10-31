"""所有 bitEngine 实体的公共基类。"""

from __future__ import annotations

from homeassistant.helpers.device_registry import CONNECTION_NETWORK_MAC, DeviceInfo, format_mac
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import BitEngineCoordinator


class BitEngineEntity(CoordinatorEntity[BitEngineCoordinator]):
    """带设备信息的基类。"""

    _attr_has_entity_name = True

    def __init__(self, coordinator: BitEngineCoordinator, key: str) -> None:
        super().__init__(coordinator)
        entry = coordinator.config_entry
        self._attr_unique_id = f"{entry.unique_id or entry.entry_id}_{key}"

        data = coordinator.data or {}
        mac = data.get("mac")

        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.unique_id or entry.entry_id)},
            # 有 MAC 就登记网络连接：HA 据此把本设备和 DHCP/路由器看到的那台
            # 关联起来，用户在设备页能看到 IP 与网络信息。
            connections={(CONNECTION_NETWORK_MAC, format_mac(mac))} if mac else set(),
            manufacturer="bitEngine",
            model="bitEngine Hub C5",
            name=data.get("hostname") or entry.title,
            sw_version=data.get("fw_version"),
            hw_version=data.get("chip_revision"),
            configuration_url=f"http://{coordinator.host}",
        )
