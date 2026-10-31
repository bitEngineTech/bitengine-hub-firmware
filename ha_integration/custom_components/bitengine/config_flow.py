"""配对流程：zeroconf 自动发现 + 手工添加 + 密码变更后的重新认证。"""

from __future__ import annotations

from typing import Any

import aiohttp
import voluptuous as vol
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.device_registry import format_mac
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import (
    API_STATUS,
    AUTH_USERNAME,
    CONF_PASSWORD,
    DOMAIN,
    LOGGER,
    REQUEST_TIMEOUT,
)


async def _probe(hass, host: str, password: str | None) -> dict[str, Any]:
    """连一次网关，回 status。抛 ValueError(原因) 表示这次配对失败。

    password=None 表示**不带凭据探一次** —— 网关在"开放"状态下（出厂默认）
    /api/v1/status 是匿名可读的，那时用户不需要输任何东西。
    只有真的收到 401 才去问密码。这是生态里的通行做法
    （SMLIGHT/Shelly 都是先探再问），也修掉了一个实打实的 bug：
    出厂网关根本没有密码（固件 auth.c 里"没设密码→全部放行"），
    而我们以前把密码做成必填 —— 用户被一个不会被校验的框挡住，
    填任意字符串都能过，还因此以为设备受了保护。
    """
    session = async_get_clientsession(hass)
    try:
        async with session.get(
            f"http://{host}{API_STATUS}",
            auth=aiohttp.BasicAuth(AUTH_USERNAME, password) if password is not None else None,
            timeout=aiohttp.ClientTimeout(total=REQUEST_TIMEOUT),
        ) as resp:
            if resp.status == 401:
                raise ValueError("invalid_auth")
            resp.raise_for_status()
            return await resp.json(content_type=None)
    except ValueError:
        raise
    except (aiohttp.ClientError, TimeoutError) as err:
        LOGGER.debug("探测 %s 失败: %s", host, err)
        raise ValueError("cannot_connect") from err


class BitEngineConfigFlow(ConfigFlow, domain=DOMAIN):
    """bitEngine Hub 的配置流程。"""

    VERSION = 1

    def __init__(self) -> None:
        self._host: str | None = None
        self._discovered_name: str | None = None

    _status: dict[str, Any] | None = None

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        """网关广播 _zigbee-bridge._tcp，HA 发现后走这里。"""
        host = discovery_info.host
        props = discovery_info.properties

        # unique_id 优先用 TXT 里的 mac：**在连接之前**就能判断"这台已经加过了"，
        # 于是重复发现不会骚扰用户，换了 IP 也能自动更新地址而不是变成第二台设备。
        mac = props.get("mac")
        if mac:
            await self.async_set_unique_id(format_mac(mac))
            self._abort_if_unique_id_configured(updates={CONF_HOST: host})

        self._host = host
        self._discovered_name = props.get("model") or "bitEngine Hub"
        self.context["title_placeholders"] = {"name": self._discovered_name}

        # 先不带凭据探一次。通了 → 走零输入确认；401 → 才去要密码。
        try:
            self._status = await _probe(self.hass, host, None)
        except ValueError:
            self._status = None
        if self._status is not None:
            return await self.async_step_confirm()
        return await self.async_step_user()

    async def async_step_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """零输入确认。HA 规范要求发现流程**必须**有用户确认这一步，
        不能自动建条目 —— 所以是"零输入"，不是"零点击"。"""
        if user_input is not None:
            assert self._status is not None
            return self.async_create_entry(
                title=self._status.get("hostname") or self._discovered_name or "bitEngine Hub",
                data={CONF_HOST: self._host, CONF_PASSWORD: ""},
            )
        return self.async_show_form(
            step_id="confirm",
            description_placeholders={"name": self._discovered_name or "bitEngine Hub"},
        )

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}

        if user_input is not None:
            host = user_input.get(CONF_HOST) or self._host
            password = user_input.get(CONF_PASSWORD) or None   # 留空 = 匿名探测
            try:
                status = await _probe(self.hass, host, password)
            except ValueError as err:
                errors["base"] = str(err)
            else:
                # 手工添加这条路径没有 TXT，unique_id 只能连上之后从 status 取。
                # 固件 v0.5.0-165 起 status 带 mac；更老的固件退回用 hostname，
                # 那样换主机名会被当成新设备，但至少能用。
                mac = status.get("mac")
                unique = format_mac(mac) if mac else f"host:{status.get('hostname', host)}"
                await self.async_set_unique_id(unique)
                self._abort_if_unique_id_configured(updates={CONF_HOST: host})
                return self.async_create_entry(
                    title=status.get("hostname") or self._discovered_name or "bitEngine Hub",
                    data={CONF_HOST: host, CONF_PASSWORD: password or ""},
                )

        # 自动发现时 host 已知，不要再问一遍
        # 密码是 **Optional** —— 开放状态的网关不需要它。留空即匿名连接。
        if self._host:
            schema = vol.Schema({vol.Optional(CONF_PASSWORD, default=""): str})
        else:
            schema = vol.Schema(
                {vol.Required(CONF_HOST): str, vol.Optional(CONF_PASSWORD, default=""): str}
            )

        return self.async_show_form(
            step_id="user",
            data_schema=schema,
            errors=errors,
            description_placeholders={"name": self._discovered_name or "bitEngine Hub"},
        )

    async def async_step_reauth(
        self, entry_data: dict[str, Any]
    ) -> ConfigFlowResult:
        """网关密码被改过之后，协调器抛 ConfigEntryAuthFailed 会走到这里。"""
        self._host = entry_data[CONF_HOST]
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        entry = self._get_reauth_entry()

        if user_input is not None:
            try:
                await _probe(self.hass, self._host, user_input[CONF_PASSWORD])
            except ValueError as err:
                errors["base"] = str(err)
            else:
                return self.async_update_reload_and_abort(
                    entry, data_updates={CONF_PASSWORD: user_input[CONF_PASSWORD]}
                )

        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema({vol.Required(CONF_PASSWORD): str}),
            errors=errors,
        )
