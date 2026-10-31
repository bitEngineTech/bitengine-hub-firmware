# Home Assistant integration

This page applies to the **HA profile** (the released firmware). The hub exposes standard
interfaces; nothing needs to be installed on the Home Assistant host for the radios themselves —
you point the integrations at the hub's address.

Give the hub a fixed address (a DHCP reservation, or the web UI's network settings) so the
integrations do not lose it. The hub keeps one service address even when its uplink fails over
between Ethernet and Wi-Fi.

## Zigbee

The Zigbee coordinator (EFR32MG24, EmberZNet NCP) is reachable as **EZSP over TCP on port
6638**. Use **one** Zigbee integration at a time — ZHA *or* Zigbee2MQTT — exactly as with a USB
coordinator. The hub serves one coordinator client at a time; a new connection from the same
host replaces the old one, so two integrations pointed at the same hub take the radio from each
other.

### ZHA

The hub announces itself over mDNS (`_zigbee-coordinator._tcp`), so Home Assistant normally
offers ZHA under *Settings → Devices & services → Discovered*. To add it by hand:

1. *Settings → Devices & services → Add integration → Zigbee Home Automation*.
2. Radio type: **EZSP** (Silicon Labs EmberZNet).
3. Serial device path: `socket://<hub-ip>:6638`
4. Port speed `115200`, flow control none (ignored over TCP, but required by the form).

The hub's web UI (*Zigbee* page) shows the exact address to paste.

### Zigbee2MQTT

In the Zigbee2MQTT configuration (`configuration.yaml` or the add-on settings):

```yaml
serial:
  port: tcp://<hub-ip>:6638
  adapter: ember
```

Tested with Zigbee2MQTT 2.14.2 (October 2026).

### Moving between ZHA and Zigbee2MQTT

The Zigbee network (keys, PAN ID, channel) lives in the EFR32MG24, not in the integration. Stop
or disable one integration before starting the other, and give Zigbee2MQTT the network
parameters already stored in the NCP (`pan_id`, `ext_pan_id`, `channel`, `network_key`);
otherwise it forms a new network and every device has to be paired again.

### Transmit power

The hub does not set the Zigbee transmit power; ZHA or Zigbee2MQTT does. The NCP limits it to
the maximum for the regulatory region selected in the hub's settings — see
[bitengine-hub-zigbee-ncp](https://github.com/bitEngineTech/bitengine-hub-zigbee-ncp).

## Thread and Matter

The OpenThread Border Router runs **on the hub**; the *OpenThread Border Router* add-on is
**not** needed.

1. *Settings → Devices & services → Add integration → Open Thread Border Router*.
2. URL: `http://<hub-ip>:8081`
3. The hub then appears under *Settings → Devices & services → Thread*, where you can make its
   network the preferred network.

The OTBR REST API is unauthenticated by default, because Home Assistant's OTBR integration does
not send credentials, and it exposes the Thread dataset (including the network key) to the LAN.
On an untrusted LAN, close it — either *OTBR / read-only API authentication* in the hub's
settings or a generated OTBR token — and enter the URL the web UI then shows
(`http://otbr:<secret>@<hub-ip>:8081`) in the OTBR integration. See
[web-api.md](web-api.md#otbr-authentication).

The *Thread* page of the web UI can download a backup of the Thread network dataset.

For **Matter over Thread** devices you still need Home Assistant's *Matter* integration (Matter
Server add-on); the hub provides the Thread network the devices join.

## Hub health in Home Assistant (optional)

`ha_integration/custom_components/bitengine/` is a custom integration that adds the hub itself
— chip temperature, uptime, coordinator connections, memory, network interface, MG24 status,
Ethernet link, LED brightness, night mode and a restart button. It does not provide Zigbee
devices. Installation: [ha_integration/README.md](../ha_integration/README.md).

## Channel planning

Zigbee and Thread use separate radios and run on separate 802.15.4 channels. Keep them apart
from each other and from your 2.4 GHz Wi-Fi channels; the usual 2.4 GHz coexistence guidance
applies.

The hub's own Wi-Fi uplink is 5 GHz only: 2.4 GHz Wi-Fi is disabled in firmware so that band
stays clear for Zigbee and Thread. The 5 GHz uplink shares the ESP32-C5's radio with Thread, so
Ethernet is the recommended uplink.

## Placement

Because the hub is powered over the Ethernet cable, put it where RF coverage is best — central
in the home, away from the server rack, USB 3.0 devices and other noise sources — rather than
next to the Home Assistant host.

## After a hub restart

Zigbee and Thread devices normally reconnect within a minute after the hub restarts. If ZHA
stays unavailable, reload the ZHA integration (*Settings → Devices & services → Zigbee Home
Automation → ⋮ → Reload*).
