# Flashing, updates and recovery

Detailed, step-by-step instructions will be published together with the source release and the
first signed images. This page records what is fixed by design.

## Official updates

- Release images are published on this repository's **Releases** page (ESP32-C5) and on
  [bitengine-hub-zigbee-ncp](https://github.com/bitEngineTech/bitengine-hub-zigbee-ncp/releases)
  (EFR32MG24).
- Images are **signed**. The hub verifies the signature before applying an update.
- Updates for both MCUs are applied from the hub's web UI; the ESP32-C5 updates the EFR32MG24
  over their UART link.

## Running your own firmware

- The ESP32-C5 **secure-boot and flash-encryption eFuses are not burned** on production units.
  Signature verification of official images does not lock you out: you can build, sign with your
  own key, and flash your own image.
- Both MCUs remain user-flashable over their programming interfaces. The physical access
  points are documented in
  [bitengine-hub-hardware](https://github.com/bitEngineTech/bitengine-hub-hardware).

## Recovery

A recovery procedure for a hub that no longer boots its application (serial flashing of the
ESP32-C5, re-flashing the EFR32MG24 NCP) will be documented here before the first units ship.
