# Web UI

The hub's web interface: status, network settings (Ethernet / Wi-Fi), Zigbee and Thread radio
status, firmware updates.

The UI is built from this directory and **embedded into the ESP32-C5 firmware image**, which is why
it lives in the firmware repository: UI and firmware share one API and one version number, and
the firmware build consumes the UI build output.

Source, toolchain and build steps arrive with the November 2026 source release.
