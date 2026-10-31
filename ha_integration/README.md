# bitEngine Hub — Home Assistant integration

Adds the **gateway itself** to Home Assistant: chip temperature, uptime,
coordinator connections, free internal memory, network interface, MG24 status,
Ethernet link, safe mode, LED brightness, night mode, and reboot buttons.

> **This does not give you your Zigbee devices.** Those come from ZHA or
> Zigbee2MQTT talking to the gateway's bridge on port 6638. This integration is
> purely about the gateway's own health and controls, and is entirely optional —
> the gateway works without it.

## Requirements

- Home Assistant **2025.2.0** or newer (the integration imports
  `homeassistant.helpers.service_info.zeroconf`, which first shipped in 2025.2.0).
- A bitEngine Hub reachable over the LAN. No cloud account, no API key.

## Install

Install manually (the integration is not in HACS yet).

1. Download `bitengine-ha-v<version>.zip` (ships alongside the firmware release).
2. Unpack it **into your Home Assistant configuration directory** — the one
   holding `configuration.yaml`. The archive already carries the
   `custom_components/bitengine/` path, so it lands in the right place:

   ```bash
   unzip bitengine-ha-v0.2.0.zip -d /config
   ```

   Unpacking over an older copy is how you upgrade; the archive replaces every
   file the integration owns.
3. Restart Home Assistant. A restart is required — reloading is not enough for a
   newly added custom component.

### Adding the device

The gateway advertises `_zigbee-bridge._tcp`, so after the restart Home
Assistant shows a **discovered** card for it under Settings → Devices &
Services. Click Add, then Confirm.

No password is requested when the gateway has none set, which is the factory
state. If you have set an admin password on the gateway, the flow asks for it
and re-asks automatically whenever you change it later. The username is ignored
by the firmware; only the password is checked.

If discovery does not appear, add it by hand: Settings → Devices & Services →
Add Integration → bitEngine Hub → enter the gateway's IP address.

## Entities

17 entities, two of which are disabled by default.

| Kind | Entities |
|---|---|
| Sensor | chip temperature, uptime, coordinator connections, WiFi signal, free internal memory, Zigbee channel, active network interface, NCP version |
| Binary sensor | MG24 online, Ethernet link, safe mode, firmware unsigned, night mode active |
| Switch | night mode |
| Number | LED brightness |
| Button | reboot gateway, restart Zigbee chip |

Polling runs every 30 seconds. The gateway's web server is a single task, so a
tighter interval would compete with anyone browsing its web interface.

> ⚠️ **The "restart Zigbee chip" button reboots the whole gateway** on the
> current hardware revision, not just the Zigbee chip. The MG24 reset line is
> tied to the supervisor. With the Ethernet cable connected the gateway will most
> likely not come back on its own and needs a power cycle. Since 0.2.0 the button
> is **disabled by default**; enable it in the entity settings if you really need it.
> The next board revision fixes it.

## Also worth doing: the ZHA reconnect setting

Unrelated to this integration, but it affects everyone using ZHA with this
gateway. Without it, roughly every other gateway reboot leaves ZHA unable to
reconnect, silently, until Home Assistant is fully restarted. Add to
`configuration.yaml` and restart Home Assistant completely:

```yaml
zha:
  zigpy_config:
    use_thread: false
    network:
      tx_power: 20   # USA (FCC). EU/UK: 10
```

The `tx_power` line is separate advice: the gateway never sets the radio
transmit power, ZHA does, and it writes 8 dBm when nothing is configured. The
legal ceiling is regional, so check your local rules before raising it.

## Known gaps

- Not published to HACS. HACS expects the integration at the root of its own
  repository; until it gets one, install manually as above (`hacs.json` is
  already in place for that move).
