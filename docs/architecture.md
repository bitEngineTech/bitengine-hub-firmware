# Firmware architecture

This document describes how the bitEngine Hub firmware for the ESP32-C5 is put
together: what happens at boot, how work is split across FreeRTOS tasks, what
each component does, and how the flash is laid out.

The firmware targets **ESP-IDF v6.0.2** and an ESP32-C5 module with 8 MB flash
and 8 MB PSRAM. A second chip, an **EFR32MG24** running a Zigbee NCP image, is
connected to the C5 over a UART (460800 baud, hardware flow control).

## Product profiles

One source tree builds two firmware images. The profile is chosen at compile
time by the Kconfig choice `BITENGINE_PROFILE`; there is no runtime switch.

| Profile | CMake project name | Summary |
|---|---|---|
| **HA** (default) | `bitengine` | Bridge mode. The MG24 is exposed as EZSP over TCP port 6638 for ZHA / Zigbee2MQTT. OpenThread Border Router on the C5, OTBR REST API. Wi-Fi (5 GHz only) + Ethernet. |
| **Direct** (under development) | `bitengine-direct` | The hub is itself the Zigbee host (`zhub`), a Matter controller (`matter_ctrl`), and links to Meta's Muse. Ethernet only, separate partition table, no TCP bridge. See [direct-profile.md](direct-profile.md). |

Direct-only Kconfig options all `depend on BITENGINE_PROFILE_DIRECT`, so they
cannot be enabled in an HA build. Direct-only components still exist as
directories in an HA build, but their `CMakeLists.txt` compiles no sources
unless the corresponding option is set.

## Boot sequence

`app_main()` in `main/main.c` runs the following steps in order. Steps marked
*(Direct)* exist only in the Direct profile.

1. **Hardware watchdog kick.** If `CONFIG_BITENGINE_HAS_HW_WATCHDOG` is set, the
   external supervisor's WDI pin is toggled before anything else.
2. *(Direct, Muse enabled)* Take the one-shot "pairing boot" flag from RTC memory.
3. **Reset-reason handling.** After the first non-clean reset (anything other
   than power-on or software reset) the firmware performs one software restart.
   A counter in RTC memory prevents loops.
4. **NVS init** (erase and re-init on `NO_FREE_PAGES` / `NEW_VERSION_FOUND`).
5. **Log store** (`log_store`) hooks `esp_log` so early boot logs are kept.
6. **Default event loop** is created; `main` subscribes to `NETWORK_EVENT`.
7. **Factory identity** (`device_identity`) is read once, read-only, from the
   `fctry` partition. Boards without an identity boot normally.
8. **PoE detection** (`poe`) configures its input pin.
9. **Watchdog** (`watchdog_init`) starts feeding and registers an "is an update
   writing flash?" query used before any whole-device restart.
10. *(Direct, Muse enabled)* Decide whether this is a pairing boot; if not, BT
    static memory is released back to the heap.
11. **PSRAM self-test** (optional, `CONFIG_BITENGINE_PSRAM_SELFTEST`).
12. **Configuration** (`config_mgr_init`) loads settings from NVS.
13. **LEDs** (`led_ctrl_init`) start before networking so the status LEDs show
    the boot state while the network comes up.
14. **Network manager** (`network_mgr_init`). If it fails, the hub restarts after 10 s.
15. **Wait for network.**
    - HA: up to 10 minutes for either a working uplink or the SoftAP
      provisioning fallback; otherwise restart.
    - Direct: up to 30 s, then continue without a network.
16. **OTA confirmation.** The firmware detects whether this is the first boot
    after an update (`ESP_OTA_IMG_PENDING_VERIFY`) and logs a previous rollback
    if there was one. It then marks the running image valid:
    - HA: as soon as step 15 succeeds.
    - Direct: only once Ethernet has an address. If the first boot after an
      update has no address yet, a background task waits (up to 10 minutes from
      boot) and restarts if it never arrives, so the bootloader rolls back.
17. **SNTP** with the configured server, sync interval and POSIX time zone.
18. **mDNS** (`mdns_svc_init`).
19. *(Direct, pairing boot)* Start `hub_store`, a reduced web UI, the button and
    `muse_pair`, then return. Nothing else starts in a pairing boot.
20. **MG24 link.** HA: `ezsp_bridge_init()` (UART + TCP bridge on port 6638).
    Direct: `ezsp_bridge_init_uart_only()` (UART only, no TCP bridge).
21. **Update engines.** `ota_mg24_init()` (MG24 NCP updater) and
    `ota_cloud_init()` (cloud update). These run before the web UI because the
    web UI exposes their endpoints immediately.
22. **Web UI** (`webui_init`): HTTP server on port 80 and an OTBR-only server on
    port 8081.
23. **Safe mode exit point.** If the watchdog has recorded 3 or more self-heal
    restarts within one hour of runtime, the hub stops here with only network,
    web UI and button running. The user leaves safe mode from the web UI.
24. **Spectrum engine** and **Thread Border Router** (`spectrum_init`, `thread_br_init`).
25. **MQTT client** and **webhook** (each only does work if enabled in config).
    On the first boot after an update, the "update verified" event is posted
    here, after the webhook has subscribed.
26. *(Direct)* `hub_store` (the `hubdb` partition), `hub_svc` (event pipeline,
    Thread inventory, audit), `zhub` (Zigbee host), `matter_ctrl` (Matter
    controller, starts itself with a delay and a crash-loop guard), then
    `muse_link` (Muse session).
27. *(HA, opt-in)* BLE proxy, only if `CONFIG_BITENGINE_ENABLE_BLE_PROXY=y` (default off).
28. `matter_bridge_init()` (scaffold, see below), **button**, **scheduler**.
29. A per-heap memory snapshot is logged and the hub reports ready.

## Tasks and threading

The ESP32-C5 is a single-core RISC-V SoC running FreeRTOS. A few rules shape
the firmware:

- **Events, not callbacks into other modules.** Modules talk through the
  default `esp_event` loop (`NETWORK_EVENT`, `CONFIG_EVENT`, `WEBUI_EVENT`,
  `BUTTON_EVENT`, ...). `esp_timer` callbacks only post events or queue
  messages; they never block or take locks.
- **One HTTP task.** All requests on port 80 are served by a single
  `esp_http_server` task (8 KB stack). Handlers that block (spectrum scans,
  cloud update check) stall the whole web UI for their duration; long jobs
  (UART benchmark, MG24 flashing, cloud install) run in their own tasks and
  are polled. The OTBR server on port 8081 is a second, smaller instance.
- **PSRAM stacks for resident, non-flash tasks.** Internal SRAM is scarce, so
  long-lived tasks that never touch flash/NVS put their stacks in PSRAM
  (`psram_task.h`). Tasks that write flash (button, OpenThread, OTA, `hub_store`)
  and latency-sensitive tasks keep internal stacks.
- **Configuration writes are debounced** (2 s) in `config_mgr`; security
  settings are written immediately.
- **Layered watchdog.** Tasks register a heartbeat with a severity
  (observe / recover / fatal). The recovery engine escalates from logging (L1)
  to targeted repair (L2), subsystem restart (L3) and whole-device restart (L4).
  Repeated L4 restarts lead to safe mode (see boot step 23).

Main tasks (priority in parentheses; ESP-IDF tasks such as `esp_timer`, lwIP,
mDNS and the HTTP server use their own defaults):

| Task | Prio | Profile | Role |
|---|---|---|---|
| `wdt_feed` | 22 | both | Feeds the external hardware watchdog |
| `uart_to_tcp`, `tcp_to_uart` | 21, 20 | HA | EZSP byte stream between UART and the TCP client |
| `tcp_accept` | 18 | HA | Accepts the TCP client on port 6638 |
| `usb_to_uart`, `uart_to_usb` | 20, 21 | HA | Bridge over USB Serial/JTAG (USB bridge mode) |
| `ot_main` | 16 | both | OpenThread main loop |
| `zhub` | 12 | Direct | Zigbee host: ASH + EZSP session, network management |
| `mqtt_pub`, `ws_push`, `webhook` | 8 | both | MQTT publishing, WebSocket status push, webhook delivery |
| `button`, `led_task`, `wd_action` | 6 | both | Button, LED rendering, watchdog recovery engine |
| `health`, `stats_log`, `net_status`, `ot_br_init`, `mg24_ota` | 5 | both | Health monitor, bridge stats, network status log, BR bring-up, MG24 flashing |
| `scheduler`, `ota_cloud`, `srp_readv` | 4 | both | Night mode / periodic jobs, cloud install, SRP re-advertising |
| `hub_store`, `muse_sess`, `mc_start` | 4 | Direct | `hubdb` writer, Muse session, Matter controller start-up |
| `mdns_refresh`, `zb_check` | 3 | both | Post-start mDNS refresh, Zigbee/Thread channel check |
| `hub_svc` | 3 | Direct | Event pipeline, Thread inventory polling |

## Components

Every directory under `components/`. "Both" means the component's sources are
compiled into both profiles (possibly with profile-specific `#if` blocks).

| Component | Purpose | Profiles |
|---|---|---|
| `ble_proxy` | Passive BLE scanning published over MQTT for Home Assistant's Bluetooth proxy. | HA, opt-in (`BITENGINE_ENABLE_BLE_PROXY`, default off) |
| `ble_window` | The only entry point to BLE in the Direct profile: opens a time-limited BLE peripheral window in an isolated pairing boot. | Direct (`BITENGINE_ENABLE_MUSE_LINK`) |
| `button` | Physical button: a short press (0.1-2 s) restarts; a 2-10 s hold does nothing by itself and serves as a physical confirmation for actions started elsewhere; a 10-20 s hold resets to factory settings (keeping the Thread network); longer holds are ignored as a stuck button. | Both |
| `config_mgr` | Configuration in NVS with 2 s write debounce, validation, JSON export/import, migration, and the radio-region menu. | Both |
| `device_identity` | Read-only factory identity (serial number, hardware revision, manufacturing date) from the `fctry` partition. Never writes it. | Both |
| `ezsp_bridge` | MG24 UART driver, NCP version reading/caching, and (HA only) the transparent EZSP byte-stream bridge: TCP server on port 6638 with per-client TX ring buffer, optional IP whitelist, USB bridge mode. | Both; TCP/USB bridge sources HA only |
| `hub_core` | Direct-profile device layer: `hubdb` store, command registry shared by Muse and the local REST API, command handlers, event pipeline, audit log, Thread inventory, device metadata, jobs. | Direct |
| `idf_fixes` | Local backport of an upstream ESP-IDF fix to crypto-peripheral locking, applied with linker `--wrap`. To be removed once the IDF version includes it. | Both |
| `led_ctrl` | Four WS2812B status LEDs (SYS / Zigbee / Thread / NET): state rendering, activity blinks, night-mode suppression, test and timing endpoints. | Both |
| `log_store` | Keeps a copy of `ESP_LOG` output in a PSRAM ring plus a short RTC tail that survives soft resets; served by `/api/v1/logs`. Nothing is written to flash. | Both |
| `matter_bridge` | Scaffold for a future Zigbee-to-Matter bridge. Only logs at init; no Matter SDK is linked. | Both |
| `matter_ctrl` | Direct-profile Matter controller built on esp-matter: client-only controller + commissioner on the hub's own fabric, on-network commissioning, interview, subscriptions, basic control, device removal, built-in PAA trust store. | Direct (`BITENGINE_ENABLE_MATTER_CTRL`) |
| `mdns_svc` | mDNS hostname and services: `_http._tcp` always; `_zigbee-bridge._tcp` and `_zigbee-coordinator._tcp` in HA only; brands the Thread `_meshcop._udp` TXT. `mdns_load.c` is a synthetic-load generator for capacity tests (`BITENGINE_ENABLE_MDNS_LOAD`, default off). | Both |
| `mqtt_client` | MQTT client: periodic status publishing, Home Assistant MQTT discovery, network failover/recovery events, optional TLS. | Both |
| `muse_link` | Muse session: credentials in `hubdb`, VM API client, TLS + WebSocket + Noise session, registration, command dispatch, push notifications. Includes code taken unchanged from the Muse Gadget SDK. | Direct (`BITENGINE_ENABLE_MUSE_LINK`) |
| `muse_pair` | Muse community pairing over BLE, run inside the isolated pairing boot; physical button confirmation. Includes code from the Muse Gadget SDK. | Direct (`BITENGINE_ENABLE_MUSE_LINK`) |
| `network_mgr` | Wi-Fi station (5 GHz only) with SoftAP provisioning fallback, DM9051 SPI Ethernet, uplink failover, DHCP client-identity handling, network events. In the Direct profile Wi-Fi is replaced by a stub (`wifi_none.c`). | Both; Wi-Fi sources HA only |
| `noise_core` | Noise XX protocol core (C++), vendored from the Muse Gadget SDK. | Direct (`BITENGINE_ENABLE_MUSE_LINK`) |
| `ota_cloud` | Cloud update: fetches an HTTPS manifest, compares versions (no downgrades), installs app / web UI / NCP images; anti-cross-flash profile check (`ota_profile.c`). Signature verification is done by ESP-IDF in `esp_ota_end()`. | Both |
| `ota_mgr` | MG24 NCP update over the existing UART through the Gecko bootloader's XMODEM menu; GBL parsing and ECDSA-P256 signature verification on the C5; developer unlock for unsigned images; UART link benchmark; `www` image validation. | Both |
| `poe` | PoE presence detection from a digital input. | Both |
| `scheduler` | LED night-mode time window and periodic jobs (configuration backup notification). | Both |
| `spectrum` | IEEE 802.15.4 energy scan, beacon scan, channel recommendation and background interference monitoring on the C5's 802.15.4 radio (through OpenThread when Thread is built in). | Both |
| `thread_br` | OpenThread Border Router on the C5's built-in 802.15.4 radio: stack lifecycle, backbone selection across Ethernet / Wi-Fi, SRP advertising-proxy helpers, Zigbee/Thread channel coexistence, regional TX power. | Both |
| `utf8` | Single UTF-8 validator/repairer shared by JSON output and configuration input. | Both |
| `watchdog` | Hardware watchdog feeding, task heartbeat registry, subsystem probes (including MG24 health), resource guards, recovery engine, safe mode, crash forensics, optional fault injection. | Both |
| `webhook` | HTTP(S) webhook notifications for selected events (Zigbee crash, network down/failover/recovered, temperature, memory, interference, update available/complete, config backup). | Both |
| `webui` | HTTP server: REST API, WebSocket, OTBR REST API, factory self-test, static web UI from the `www` partition. HA adds a coordinator-compatibility layer (`/api2/*`); Direct adds `/api/v1/muse/*`, `/api/v1/hub/*`, `/api/v1/matter/*`. See [web-api.md](web-api.md). | Both; some files per profile |
| `zhub` | Direct-profile Zigbee host on the MG24: ASH link layer, EZSP v18 frames and session, network bring-up, device table and interview, minimal ZCL, Tuya and Xiaomi/Aqara value converters (tables generated from zigbee-herdsman-converters, MIT). | Direct (`BITENGINE_ENABLE_ZHUB`) |

Profile switches used by the component `CMakeLists.txt` files:

| Option | Set by | Effect |
|---|---|---|
| `CONFIG_BITENGINE_PROFILE_DIRECT` | Direct profile | `hub_core` sources, Direct web UI files, Direct boot path |
| `CONFIG_BITENGINE_NET_ETH_ONLY` | Direct (default y) | Wi-Fi not initialized or linked; `/wifi/*` and `/network/softap` not registered |
| `CONFIG_BITENGINE_ENABLE_ZHUB` | Direct (always) | `zhub` built; TCP/USB bridge and `/api2/*` not built |
| `CONFIG_BITENGINE_ENABLE_MUSE_LINK` | Direct (`sdkconfig.defaults.direct`) | `ble_window`, `muse_pair`, `muse_link`, `noise_core`, Muse/hub endpoints |
| `CONFIG_BITENGINE_ENABLE_MATTER_CTRL` | Direct (`sdkconfig.defaults.direct`) | `matter_ctrl` and the esp-matter dependency |

## Other top-level directories

### `frontend/`

The web UI: a single-page application in plain JavaScript (`js/`, `css/`,
pages for dashboard, Wi-Fi, Ethernet, Zigbee, Thread, spectrum, LEDs,
MQTT/webhook, updates, logs, settings; English, German and Chinese strings).

- `frontend/build.sh` inlines the CSS and JavaScript into one HTML file and
  gzips it (byte-stable output) to `www/index.html.gz`.
- `frontend/direct/build.sh` derives the Direct-profile bundle from the HA
  bundle (drops pages the Direct profile does not have, adds Direct pages) and
  writes `www-direct/index.html.gz`.
- `tools/stage_www.py` is run by the top-level CMake target `www_stage` on every
  `idf.py build`. It rebuilds the frontend, writes `version.json` (frontend git
  revision + content hash, no timestamp) and stages the files. CMake then packs
  them into `build/www.bin` (SPIFFS) for the `www` partition, and
  `idf.py flash` writes it together with the app. The firmware reports the
  frontend version as `www_version` in `/api/v1/status`.

The firmware serves only pre-compressed files and requires
`Accept-Encoding: gzip`.

### `ha_integration/`

An optional Home Assistant custom integration, domain `bitengine`
(`custom_components/bitengine/`). It is discovered over zeroconf
(`_zigbee-bridge._tcp` with model `bitengine-hub*`), polls `/api/v1/status`,
and exposes the hub's own health and controls (temperature, uptime, coordinator
connections, memory, network interface, MG24 status, Ethernet link, safe mode,
LED brightness, night mode, reboot buttons). It does **not** provide Zigbee
devices; those come from ZHA or Zigbee2MQTT through port 6638.
`tools/make_ha_zip.sh` packages it.

### `mg24/`

A git submodule, [bitengine-hub-zigbee-ncp](https://github.com/bitEngineTech/bitengine-hub-zigbee-ncp):
the EFR32MG24 Zigbee NCP firmware project and its release images. The C5 build reads the NCP image-signing public key from
`mg24/release/mg24_gbl_signing_key.pub` and embeds it, so clone with
submodules. The C5 firmware and the NCP image must use matching UART settings
(460800 baud, hardware flow control on both sides).

### `tools/`

Build, signing and maintenance helpers. The most relevant ones:

| Tool | Purpose |
|---|---|
| `build_profiles.sh` | Builds the HA and/or Direct profile and runs the profile checks |
| `stage_www.py` | Stages the web UI and `version.json` for the `www` partition |
| `signing_key.sh` | App signing key handling: `ensure` (run by CMake; creates a development key if none exists), `check`, `verify <app.bin>`, `backup` |
| `patch_mdns.py`, `patch_mqtt.py`, `patch_esp_matter.py` | Build-time patches to managed components. Each matches exact upstream text and fails the build if upstream changed. `patch_esp_matter.py` runs only when the Matter controller is enabled. |
| `gen_mg24_pubkey.py` | Generates the C array for the embedded MG24 signing key |
| `gbl_verify.py` | Verifies an MG24 `.gbl` signature on the host with the same rules as the firmware |
| `make_manifest.py` | Produces a cloud-update `manifest.json` (per profile) |
| `migrate_profile.py` | Moves a board from the HA to the Direct profile over USB, or restores a full-flash backup |
| `fetch_paa.py` | Regenerates the Matter PAA snapshot built into `matter_ctrl` |
| `zhc_export/` | Regenerates the `zhub` Tuya / Xiaomi tables from zigbee-herdsman-converters |
| `generate_nvs.py` | Generates (and optionally flashes) the `fctry` factory-identity image |
| `check_profile_rules.py`, `check_profiles.py`, `check_uri_slots.py`, `grep_code.py` | Source-shape and build-artifact checks |
| `test_api.sh`, `factory_test.sh` / `.ps1` | End-to-end REST tests and the factory self-test driver (need a hub on the network) |

### Signed images

App images are signed (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`,
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, RSA). An update is accepted
only if it is signed with the same key as the running image. A fresh clone
builds with an automatically generated development key, so a self-built image
can update a board that is already running a self-built image. The build also
produces a `*-unsigned.bin` intermediate; do not flash it.

## Partition layouts

Both layouts assume 8 MB flash, with the bootloader at `0x2000` and the
partition table at `0x8000`.

### HA profile (`partitions.csv`)

The CSV leaves offsets blank; the resolved offsets below are what the
partition tool assigns (they match the build's `flash_args`).

| Name | Type | SubType | Offset in CSV | Resolved offset | Size |
|---|---|---|---|---|---|
| `nvs` | data | nvs | (auto) | `0x9000` | `0x6000` |
| `otadata` | data | ota | (auto) | `0xF000` | `0x2000` |
| `phy_init` | data | phy | (auto) | `0x11000` | `0x1000` |
| `ota_0` | app | ota_0 | (auto) | `0x20000` | `0x300000` |
| `ota_1` | app | ota_1 | (auto) | `0x320000` | `0x300000` |
| `www` | data | spiffs | (auto) | `0x620000` | `0x80000` |
| `coredump` | data | coredump | (auto) | `0x6A0000` | `0x10000` |
| `fctry` | data | nvs | (auto) | `0x6B0000` | `0x4000` |

### Direct profile (`partitions_direct.csv`)

All offsets are fixed in the file.

| Name | Type | SubType | Offset | Size |
|---|---|---|---|---|
| `nvs` | data | nvs | `0x9000` | `0x10000` |
| `otadata` | data | ota | `0x19000` | `0x2000` |
| `phy_init` | data | phy | `0x1B000` | `0x1000` |
| `matter` | data | nvs | `0x1C000` | `0x10000` |
| `ota_0` | app | ota_0 | `0x30000` | `0x380000` |
| `ota_1` | app | ota_1 | `0x3B0000` | `0x380000` |
| `www` | data | spiffs | `0x730000` | `0x80000` |
| `hubdb` | data | nvs | `0x7B0000` | `0x30000` |
| `coredump` | data | coredump | `0x7E0000` | `0x10000` |
| `fctry` | data | nvs | `0x7F0000` | `0x4000` |

- `nvs`: configuration (and, in both profiles, OpenThread data).
- `matter`: esp-matter / CHIP storage (Direct only).
- `hubdb`: Direct-profile data written only by `hub_store` (Muse credentials and
  settings, device names/rooms, the Zigbee device table, the Matter fabric).
- `fctry`: factory identity, written once on the production line; the firmware
  only reads it.

The two layouts are not OTA-compatible. Changing a board's profile requires a
USB flash; see [direct-profile.md](direct-profile.md).
