# Building the firmware

## Requirements

- **ESP-IDF v6.0.2** (the release tag, not `master`). Install it with the
  [Espressif instructions](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c5/get-started/index.html)
  and source `export.sh` in the shell you build from.
- `git`, `python3`, `bash`, `gzip` — the web UI build (`frontend/build.sh`) is a shell script with
  no Node.js dependency.
- Network access on the first build: the ESP-IDF component manager downloads the managed
  components pinned in `dependencies.lock` (mdns, mqtt, led_strip, Ethernet drivers, cJSON) and,
  for the Direct profile, `dependencies.direct.lock` (adds esp_matter).

## Get the source

The EFR32MG24 NCP firmware is a git submodule at `mg24/`. The ESP32-C5 build needs it (the build
embeds the MG24 image-signing public key from `mg24/release/`, and host tests use its release
images and test vectors), so clone recursively:

```bash
git clone --recursive https://github.com/bitEngineTech/bitengine-hub-firmware.git
cd bitengine-hub-firmware
# already cloned without --recursive:
git submodule update --init
```

The submodule pins the NCP release that this firmware is paired with. The UART settings on
both sides (460800 baud, RTS/CTS flow control) must match; update the two together.

## HA profile (default)

```bash
source ~/esp-idf-v6.0.2/export.sh     # wherever you installed it
idf.py build
```

`sdkconfig` is committed and already targets `esp32c5`; no `set-target` is needed.

Outputs in `build/`:

| File | What it is |
|---|---|
| `bitengine.bin` | Application image, signed (see *Signing* below) |
| `bitengine-unsigned.bin` | The same image before signing. **Never flash or upload it**: the hub only accepts signed images, and a board left with an unsigned app cannot update itself |
| `bitengine-<version>.bin` | Copy of `bitengine.bin` named after the version |
| `www.bin` | Web UI (SPIFFS image for the `www` partition), built from `frontend/` by `tools/stage_www.py`. Byte-stable when the UI did not change |
| `bootloader/bootloader.bin`, `partition_table/partition-table.bin`, `ota_data_initial.bin` | Usual ESP-IDF artifacts |

## Direct profile

The Direct profile (`bitengine-direct`, under development — see
[direct-profile.md](direct-profile.md)) has its own sdkconfig, partition table, lock file and
build directory:

```bash
idf.py -B build-direct -DSDKCONFIG=sdkconfig.direct \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.direct" \
       -DBITENGINE_PROFILE=direct build
# or build both profiles and run the profile-isolation checks:
bash tools/build_profiles.sh
```

The top-level `CMakeLists.txt` refuses mixed combinations (Direct sdkconfig built as HA, Direct
built into `build/`, and so on) instead of producing a mislabeled image.

## Version string

The version comes from `git describe` (ESP-IDF default). Official releases are built from tagged
commits; a build from an untagged checkout carries the commit hash. The version is shown in the
web UI and reported by `GET /api/v1/status`.

## Signing

Release images are signed (ESP-IDF "signed app images without secure boot", RSA-3072). The
**secure-boot and flash-encryption eFuses are not burned**, and the trust anchor for an update is
the signature block of the application that is currently running — not a key fused into the
chip. Consequences:

- On the first build, `tools/signing_key.sh ensure` (called from `CMakeLists.txt`) generates a
  development key `secure_boot_signing_key.pem` in the repository root if none exists. It is
  ignored by git. Keep it if you want to keep updating a hub over the network with your own
  builds.
- A hub running an image signed with key A accepts updates signed with key A. To move a hub from
  official images to your own (or back), flash over USB once — see [flashing.md](flashing.md).
- `secure_boot_pubkey.digest` is the fingerprint of the **official** release key;
  `bash tools/signing_key.sh check` compares your key against it. It is expected to fail for a
  self-generated key.

## Tests and checks

Each component that has them ships two kinds of checks. Run them from the component directory:

```bash
cd components/<name>
bash VERIFY.sh            # design-invariant checks (code shape: grep / small scripts)
bash test_host/run.sh     # host unit tests (gcc on Linux, no hardware) where present
```

Repository-wide checks:

```bash
bash tools/check_english_only.sh      # UI strings in frontend sources must be English (translations live in frontend/js/i18n/)
python3 tools/check_uri_slots.py      # enough httpd URI handler slots for every registered endpoint
python3 tools/check_profile_rules.py  # source-level rules keeping Direct-profile code out of the HA profile
python3 tools/check_profiles.py       # the same on build outputs (build/ and build-direct/ must exist)
```

`tools/test_api.sh <hub-ip>` and the other `tools/test_*.sh` scripts are end-to-end tests against
a hub on your network; read the header of each before running it — some change settings or
reboot the hub.

A few checks compare code against design documents that are not part of this repository; they
report those documents as missing. Source comments are largely in Chinese and refer to the same
internal documents.

## Patches applied during the build

Three managed components are patched in place after download, from `CMakeLists.txt`:

| Script | Component | Why |
|---|---|---|
| `tools/patch_mdns.py` | espressif/mdns | Answers that do not fit in one packet were silently truncated; CPU, queue and packet-completeness fixes for many services proxied for Thread devices (SRP) |
| `tools/patch_mqtt.py` | espressif/mqtt | Puts the MQTT task's stack in PSRAM to free internal SRAM |
| `tools/patch_esp_matter.py` | espressif/esp_matter (Direct profile) | DNS-SD browse type truncated for commissioning by setup code; no CHIP test group keys / default IPK on the controller's fabric; no operational advertising for a client-only controller |

Each script's header explains the patch in detail.
