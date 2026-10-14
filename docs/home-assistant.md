# Home Assistant integration

The hub exposes two standard network interfaces. Nothing needs to be installed on the Home
Assistant host for the radios themselves; you only point the integrations at the hub's address.

Give the hub a fixed IP address (DHCP reservation) so the integrations do not lose it.

## Zigbee

The Zigbee coordinator (EFR32MG24, EmberZNet NCP) is reachable as **EZSP over TCP on port 6638**.
Use **one** Zigbee integration at a time — ZHA *or* Zigbee2MQTT — the same way you would with a USB
coordinator.

### ZHA

1. *Settings → Devices & services → Add integration → Zigbee Home Automation*.
2. Radio type: **EZSP** (Silicon Labs EmberZNet).
3. Serial device path: `socket://<hub-ip>:6638`
4. Port speed: `115200`, flow control: none (the values are ignored over TCP but must be set).

Status: tested.

### Zigbee2MQTT

In the Zigbee2MQTT configuration (`configuration.yaml` or the add-on settings):

```yaml
serial:
  port: tcp://<hub-ip>:6638
  adapter: ember
```

Status: being validated; will be marked tested once confirmed on a current Zigbee2MQTT release.

## Thread and Matter

The OpenThread Border Router runs **on the hub**. The *OpenThread Border Router* add-on is **not**
needed.

1. *Settings → Devices & services → Add integration → Open Thread Border Router*.
2. URL: `http://<hub-ip>:8081`
3. The hub then appears under *Settings → Devices & services → Thread*, where you can make it the
   preferred network, or join it to an existing Thread network credentials-wise.

For **Matter over Thread** devices you still need Home Assistant's *Matter* integration (Matter
Server add-on); the hub provides the Thread network the devices join.

## Channel planning

Zigbee and Thread use separate radios and can run on separate 802.15.4 channels. Keep them apart
from each other and from your 2.4 GHz Wi-Fi channel; the usual guidance for 2.4 GHz coexistence
applies. If the hub's Wi-Fi uplink is used, prefer 5 GHz so it does not compete with the
802.15.4 radios.

## Placement

Because the hub is powered over the Ethernet cable, put it where RF coverage is best — central
in the home, away from the server rack, USB 3.0 devices and other noise sources — rather than
next to the Home Assistant host.
