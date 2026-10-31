# Direct profile

> **Status: under development.** The Direct profile is not released as product
> firmware. Interfaces, storage formats and behavior may change without
> notice. The released firmware is the HA profile.

## What it is

The firmware builds in one of two profiles, selected at compile time by the
Kconfig choice `BITENGINE_PROFILE`. There is no runtime switch; changing
profile means flashing a different image.

| | HA profile | Direct profile |
|---|---|---|
| CMake project name (image `project_name`) | `bitengine` | `bitengine-direct` |
| Zigbee | MG24 bridged as EZSP over TCP port 6638 to ZHA / Zigbee2MQTT | The hub is the Zigbee host (`zhub`); no TCP bridge |
| Matter | none | Matter controller (`matter_ctrl`) |
| Muse | none | Link to Meta's Muse (`muse_link`, `muse_pair`, `noise_core`) |
| Thread | OpenThread Border Router + OTBR REST API | Same |
| Uplinks | Wi-Fi (5 GHz) + Ethernet | Ethernet only; Wi-Fi is neither initialized nor linked |
| Partition table | `partitions.csv` | `partitions_direct.csv` |
| Web UI bundle | `www/` | `www-direct/` (derived from the HA bundle) |

In the Direct profile the hub manages Zigbee and Matter devices itself and
exposes them through a single command registry that is reachable both from
Muse and from the local REST API (`POST /api/v1/hub/invoke`).

## Components

| Component | Enabled by | What it does |
|---|---|---|
| `hub_core` | `BITENGINE_PROFILE_DIRECT` | Device layer and storage. `hub_store` is the only writer of the `hubdb` partition (debounced writes, versioned blobs). Also: the command registry and handlers, parameter validation and rate limits, the event pipeline and recent-events ring, the audit log, the Thread network inventory, device metadata (names, rooms, notification settings) and jobs for long-running commands. `hub_svc` runs the event pipeline and inventory polling in its own task. |
| `zhub` | `BITENGINE_ENABLE_ZHUB` (always on in Direct) | Zigbee host for the MG24 NCP: ASH link layer, EZSP v18 frames and session, NCP reset/handshake/back-off, network bring-up, device table (up to 64 devices) with join handling and interview, minimal ZCL, IAS zone enrollment, binding and reporting set-up, and decoders for Tuya and Xiaomi/Aqara devices. It resumes the network already stored in the MG24 and does not form a new network automatically. |
| `matter_ctrl` | `BITENGINE_ENABLE_MATTER_CTRL` | Matter controller on esp-matter: client-only controller and commissioner on the hub's own fabric (no Matter server, no BLE, does not take over OpenThread). On-network commissioning with a setup code, interview (Basic Information, Descriptor), subscriptions, on/off/toggle/brightness control, device removal, and a built-in PAA trust store. Up to `BITENGINE_MATTER_MAX_NODES` nodes (default 128). Starts with a delay after boot, and does not start at all if it failed to stay up during the previous boots (a power cycle clears this). |
| `muse_link` | `BITENGINE_ENABLE_MUSE_LINK` | Muse session: credentials and settings in `hubdb`, VM API client, TLS + WebSocket + Noise session, registration of the command list, command dispatch with permission checks, and push notifications for device events. Failures are contained to Muse; they do not restart the hub. |
| `muse_pair` | `BITENGINE_ENABLE_MUSE_LINK` | Muse community pairing over BLE, run in an isolated pairing boot (see below). |
| `noise_core` | `BITENGINE_ENABLE_MUSE_LINK` | Noise XX protocol core (C++). |
| `ble_window` | `BITENGINE_ENABLE_MUSE_LINK` | The only user of BLE in this profile: opens and closes the pairing window, and runs a Thread self-check after a pairing boot. |

`muse_link`, `muse_pair` and `noise_core` include code taken from the
open-source [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk)
(Apache-2.0). The pinned upstream commit and per-file provenance are recorded in
`components/muse_link/UPSTREAM.md`, and each of the three components carries the
upstream `LICENSE`. `components/muse_link/check_upstream_files.sh` verifies the
vendored files against that record.

Shared components behave differently in this profile only through
compile-time selection (conditional source lists and `#if` blocks), for example:

- `ezsp_bridge` builds only the UART side (`ezsp_bridge_init_uart_only()`);
  the TCP server, its ring buffers and the USB bridge are not compiled.
- `network_mgr` builds `wifi_none.c` instead of the Wi-Fi manager.
- `mdns_svc` announces only `_http._tcp` (no Zigbee bridge services).
- `webui` drops the coordinator-compatibility layer and the Wi-Fi, bridge-mode
  and whitelist endpoints, and adds the Muse, hub and Matter endpoints. See
  [web-api.md](web-api.md#direct-profile-endpoints).
- `ble_proxy` cannot be enabled (`BITENGINE_ENABLE_BLE_PROXY` depends on the HA profile).

### Kconfig

All Direct-only options depend on `BITENGINE_PROFILE_DIRECT`. The defaults in
`sdkconfig.defaults.direct` turn on Muse and the Matter controller.

| Option | Default (Direct) | Notes |
|---|---|---|
| `BITENGINE_NET_ETH_ONLY` | y | Ethernet-only network profile |
| `BITENGINE_ENABLE_ZHUB` | y | Not user-selectable; follows the profile |
| `BITENGINE_ENABLE_MUSE_LINK` | y (via defaults file) | Requires PSRAM; selects BT |
| `BITENGINE_ENABLE_MATTER_CTRL` | y (via defaults file) | Requires `NET_ETH_ONLY` and `MUSE_LINK` |
| `BITENGINE_MATTER_MAX_NODES` | 128 | 1-128 |
| `BITENGINE_MUSE_PAIR_WINDOW_S` | 180 | Pairing window, 60-600 s |
| `BITENGINE_MUSE_API_URL`, `BITENGINE_MUSE_NOISE_HOST` | built-in defaults | Values received during pairing take precedence; the API URL must be `https://` |
| `BITENGINE_BLE_WINDOW_POLICY` | isolated | Only the isolated-restart policy exists |
| `BITENGINE_DIAG_HUBDB` | y (via defaults file) | Development-only diagnostic endpoint `/api/v1/diag/hubdb` |

## Boot differences

Compared with the HA boot sequence in [architecture.md](architecture.md#boot-sequence):

- **Network wait.** The hub waits at most 30 s for Ethernet and then
  continues without it; components that need the network wait for the
  network-online event themselves. SNTP is restarted as soon as the network
  comes up.
- **Update confirmation.** A new image is marked valid only after Ethernet has
  an address. If the first boot after an update has no address within 10
  minutes of boot, the hub restarts and the bootloader rolls back to the
  previous image.
- **Start order** after the web UI and the safe-mode exit point: Thread Border
  Router, MQTT, webhook, then `hub_store`, `hub_svc`, `zhub`, `matter_ctrl`,
  and finally `muse_link`. None of the Direct components start in safe mode.
- **Pairing boot.** Muse pairing uses an isolated restart: `POST
  /api/v1/muse/pair` sets a one-shot flag and restarts the hub. That boot
  starts only the network, configuration, LEDs, `hub_store`, a reduced web UI,
  the button and `muse_pair`. No Thread, MG24, MQTT, webhook, scheduler or
  Matter. The hub advertises a BLE peripheral for the pairing window; the user
  confirms in person by holding the hub's button for 2-10 s when the Muse app
  asks. When pairing succeeds, fails, times out or is cancelled, the hub
  restarts back to a normal boot. Pairing requires an admin password, an SDK
  token, Muse enabled, no update in progress, and a confirmed running image.

## Building

The Direct profile has its own sdkconfig, build directory and dependency lock
file (`dependencies.direct.lock`; esp-matter is resolved only for this profile).

```bash
source <esp-idf-v6.0.2>/export.sh

idf.py -B build-direct \
       -DSDKCONFIG=sdkconfig.direct \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.direct" \
       -DBITENGINE_PROFILE=direct build
```

Output: `build-direct/bitengine-direct.bin` (signed), a versioned copy
`bitengine-direct-<version>.bin`, and `build-direct/www.bin` built from
`frontend/direct/`. As with the HA profile, `*-unsigned.bin` is an
intermediate and must not be flashed.

Or build both profiles and run all profile checks:

```bash
bash tools/build_profiles.sh                 # both profiles + checks
bash tools/build_profiles.sh --only direct   # Direct only
bash tools/build_profiles.sh --check-only    # checks on existing builds
```

The checks cover source-shape rules (`tools/check_profile_rules.py`), build
artifacts (`tools/check_profiles.py`: no Direct-only libraries or symbols in
the HA image, project names, partition tables), the host tests of the
anti-cross-flash check, `tools/migrate_profile.py --self-test`, and the
vendored Muse SDK files and their host tests.

The top-level `CMakeLists.txt` refuses inconsistent builds:

- the sdkconfig selects the Direct profile but `-DBITENGINE_PROFILE=direct` was
  not given, or the other way round;
- a Direct build in `build/`, or an HA build in `build-direct/`.

`BITENGINE_PROFILE` is a CMake cache variable and stays set in a build
directory; pass `-DBITENGINE_PROFILE=ha` to clear it. Do not switch the profile
in `menuconfig`.

## Anti-cross-flash guard

Both profiles are signed with the same key, so signature verification cannot
tell an HA image from a Direct image. Installing the wrong one would leave a
device running firmware that does not match its partition layout (and a Direct
image has no Wi-Fi). Both app-update paths therefore compare the CMake project name stored
in the image's `esp_app_desc_t` with the running firmware **before writing
anything to flash** (`components/ota_cloud/ota_profile.c`):

- **Web upload** (`POST /api/v1/ota/esp32`): the whole image is received into
  PSRAM first and checked. Responses: 409 `profile_mismatch` or
  `profile_unknown`, 400 `image_invalid` (not an ESP app image), 500
  `internal_error`.
- **Cloud update**: after the download starts, the image descriptor is read
  with `esp_https_ota_get_img_desc()` and checked before
  `esp_https_ota_perform()` writes flash. A mismatch fails the update with the
  same reason codes.

The comparison is exact (length and bytes), so `bitengine` does not match
`bitengine-direct`. An empty or unreadable name on either side is treated as a
mismatch. The guard is pure C without ESP-IDF dependencies and has host tests
in `components/ota_cloud/test_host/`.

The guard only protects devices that already run firmware containing it. The
two profiles also use separate cloud manifests
(`CONFIG_BITENGINE_OTA_CLOUD_URL`; `tools/make_manifest.py --profile direct`).

## Switching a board between profiles

The two partition tables are not OTA-compatible, so changing profile is a USB
flashing operation intended for service, not an end-user step.
`tools/migrate_profile.py` handles it (requires the ESP-IDF environment for
`esptool` and the NVS partition tool):

```bash
python3 tools/migrate_profile.py to-direct -p <port> [--dry-run] [--backup-dir <dir>] [--build-dir build-direct]
python3 tools/migrate_profile.py restore   -p <port> --from <backup-dir>/full_8mb.bin
python3 tools/migrate_profile.py --self-test     # offline, no board needed
```

`to-direct` (HA to Direct):

1. Checks the Direct build in `build-direct/` (Direct project, signed,
   partition table matching `partitions_direct.csv`).
2. Reads a **full 8 MB backup** of the board, plus a metadata file with its
   SHA-256 and MAC address. The backup contains the factory identity, the
   admin password hash and Wi-Fi credentials; keep it private.
3. Verifies that the board currently has the HA layout and a valid factory
   identity; it refuses boards without one. `--dry-run` stops here.
4. Erases the regions that change meaning (`0xF000`-`0x30000`, which becomes
   the end of the larger `nvs`, `otadata`, `phy_init` and `matter`; and the
   Direct `hubdb` / `coredump` area).
5. Writes the Direct bootloader, partition table, app and `www` image, and
   moves the factory identity from `0x6B0000` to `0x7F0000`. Left in place, the
   old copy would sit inside the Direct `ota_1` slot and be overwritten by the
   first update.
6. Reads back and verifies: the first 24 KB of `nvs` (the whole HA `nvs`:
   configuration, admin password, Thread dataset) is unchanged, the factory
   identity is at its new address, the partition table is the Direct one, and
   `matter` is empty.

The tool enters the download mode once and does not reset the board at the
end; power-cycle the board afterwards. The Zigbee network stored in the MG24 is
not touched.

There is no `to-ha` command. To return a board to the HA profile, use
`restore` with the full backup taken by `to-direct`; it writes the 8 MB image
back unchanged. `restore` checks the backup's SHA-256 and refuses a backup
taken from a different board (MAC mismatch), since that would copy another
board's factory identity.

A board that has never had a factory identity written can be flashed with
`idf.py -B build-direct -p <port> flash` like any ESP-IDF project.
