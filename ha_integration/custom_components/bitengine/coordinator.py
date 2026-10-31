"""轮询 bitEngine 网关的 /api/v1/status。

只打一个接口：网关把 35 个字段一次吐全，没必要为每类实体各开一次连接 ——
webui 是单 httpd 任务，并发请求只会互相排队。
"""

from __future__ import annotations

from typing import Any

import aiohttp
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import (
    API_LED,
    API_STATUS,
    AUTH_USERNAME,
    CONF_PASSWORD,
    DOMAIN,
    LOGGER,
    REQUEST_TIMEOUT,
    SCAN_INTERVAL,
)

type BitEngineConfigEntry = ConfigEntry[BitEngineCoordinator]


class BitEngineCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """拉取并缓存网关状态。"""

    config_entry: BitEngineConfigEntry

    def __init__(self, hass: HomeAssistant, entry: BitEngineConfigEntry) -> None:
        super().__init__(
            hass,
            LOGGER,
            name=DOMAIN,
            update_interval=SCAN_INTERVAL,
            config_entry=entry,
        )
        self._host: str = entry.data[CONF_HOST]
        self._auth = aiohttp.BasicAuth(AUTH_USERNAME, entry.data[CONF_PASSWORD])
        self._session = async_get_clientsession(hass)

    @property
    def host(self) -> str:
        return self._host

    def _url(self, path: str) -> str:
        return f"http://{self._host}{path}"

    async def _async_update_data(self) -> dict[str, Any]:
        try:
            async with self._session.get(
                self._url(API_STATUS),
                auth=self._auth,
                timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT),
            ) as resp:
                # 401 要抛 ConfigEntryAuthFailed 而不是 UpdateFailed：前者会让 HA
                # 弹出"重新认证"对话框让用户改密码，后者只会无限重试并把实体标成
                # 不可用 —— 用户看到的是"设备坏了"，而其实只是密码改了。
                if resp.status == 401:
                    raise ConfigEntryAuthFailed("网关拒绝了密码")
                resp.raise_for_status()
                data: dict[str, Any] = await resp.json(content_type=None)
        except ConfigEntryAuthFailed:
            raise
        except aiohttp.ClientError as err:
            raise UpdateFailed(f"连接网关失败: {err}") from err
        except TimeoutError as err:
            raise UpdateFailed(f"网关响应超时（{REQUEST_TIMEOUT}s）") from err

        # LED 配置在另一个接口。**失败不算整体失败** —— 老固件没有 GET /led/config
        # （2026-08-17 才补上），拿不到就让 LED 那几个实体显示不可用，其余照常。
        # 让整次刷新失败会把一台好设备整个标成离线，代价大得多。
        data["led"] = await self._try_get(API_LED) or {}
        return data

    async def _try_get(self, path: str) -> dict[str, Any] | None:
        try:
            async with self._session.get(
                self._url(path),
                auth=self._auth,
                timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT),
            ) as resp:
                if resp.status != 200:
                    return None
                return await resp.json(content_type=None)
        except (aiohttp.ClientError, TimeoutError):
            return None

    async def async_post(self, path: str, json_body: dict[str, Any] | None = None) -> None:
        """动作/设置请求。失败要抛，好让 HA 弹错误给用户。

        改完立刻 async_request_refresh()：不刷的话 UI 会先跳回旧值、等下一个
        30s 周期才变对，用户以为没生效又点一次。
        """
        async with self._session.post(
            self._url(path),
            auth=self._auth,
            json=json_body,
            timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT),
        ) as resp:
            resp.raise_for_status()
        await self.async_request_refresh()
