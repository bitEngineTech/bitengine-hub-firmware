# Flashing, updates and recovery

The hub has two MCUs and two update paths for each:

| | Over the network (web UI) | Wired |
|---|---|---|
| ESP32-C5 application + web UI | *Firmware Update* page: cloud update or file upload | USB-C (built-in USB Serial/JTAG) |
| ESP32-C5 bootloader, partition table | — | USB-C only |
| EFR32MG24 NCP application | *Firmware Update* page, or `mg24/ota_mg24.sh` | SWD (J-Link), see the NCP repository |
| EFR32MG24 bootloader | — | SWD only |

## Updating from the web UI

Open the hub's web UI (`http://<hub-ip>/`) and go to **Firmware Update**.

- **Cloud update** — *Check for updates* fetches the release manifest; an update installs the
  firmware and the web UI together and reboots once. The downloaded image's signature is checked
  before anything is written.
- **Upload new firmware** — upload `build/bitengine.bin` (signed). The hub refuses images not
  signed with the key of the firmware it is currently running, and images built for the other
  product profile (`bitengine` vs `bitengine-direct`).
- **Update frontend (web UI)** — upload `build/www.bin`. Zigbee and Thread are not touched, but
  the hub reboots.
- **Update Zigbee firmware (MG24 NCP)** — upload a `.gbl` image (see
  [bitengine-hub-zigbee-ncp](https://github.com/bitEngineTech/bitengine-hub-zigbee-ncp)). The
  ESP32-C5 writes it to the EFR32MG24 over UART through the Gecko bootloader; Zigbee is down for
  about 20–30 s, the hub itself does not reboot, and ZHA / Zigbee2MQTT reconnect on their own.
  Only images signed with the official MG24 key are accepted, and images that would shrink the
  NCP's NVM3 cache below what the chip has used are refused (that would make the NCP crash at
  boot). To flash a third-party NCP image, press *Allow unsigned firmware* and confirm on the
  device: hold the button until all LEDs turn solid red (about 2 s), then release before 10 s.

After an ESP32-C5 update, Zigbee and Thread devices normally reconnect within a minute.

## Flashing over USB

The USB-C port is connected to the ESP32-C5's built-in USB Serial/JTAG controller; no adapter
is needed. With the board powered and connected:

```bash
idf.py -p /dev/ttyACM0 flash        # bootloader, partition table, otadata, application, web UI
idf.py -p /dev/ttyACM0 monitor      # serial console
```

On Windows use `COM<n>`, or run esptool from `build/`:
`esptool --chip esp32c5 -p COM<n> write_flash @flash_args`.

Rules:

- **Never flash `bitengine-unsigned.bin`.** Flash the signed `bitengine.bin` (what
  `idf.py flash` does). A running image's signature block is what later network updates are
  verified against.
- Flashing an image signed with *your* key moves the hub to your key: from then on it accepts
  network updates signed with your key, and official images only after you flash one over USB
  again.
- The board has **no BOOT button**. Download mode is entered by esptool through USB Serial/JTAG.
- Close any serial monitor before flashing. If flashing is interrupted after the erase, the
  application partition may be left empty; the chip then reboots continuously and esptool cannot
  connect. Recover through the built-in JTAG instead, which does not depend on download mode:

  ```bash
  cd build
  openocd -f board/esp32c5-builtin.cfg \
    -c "init; reset halt; flash write_image erase bitengine.bin 0x20000; reset run; exit"
  ```

  (`0x20000` is the first application slot in `partitions.csv`; use `partitions_direct.csv` for
  the Direct profile.)

## Switching between the HA and Direct profiles

The two profiles use different partition tables, so switching is a wired operation; the
anti-cross-flash check refuses it over the network. `tools/migrate_profile.py` backs up the whole
flash and performs the switch over USB — read its header first. The Zigbee network lives in the
EFR32MG24 and is not affected.

## Recovery summary

| Symptom | Cause | What to do |
|---|---|---|
| Reboots every ~150 ms, esptool cannot connect | Empty application partition (interrupted flash) | Write the app over JTAG (above) |
| Web UI stops responding for a while, the hub still answers ping | The web server has a single request worker; a long request or a stalled client connection holds it | Wait up to two minutes, then reload |
| Zigbee completely silent after flashing one MCU | UART settings of the two sides do not match | Flash the paired versions: this repository's `mg24/` submodule pins the NCP release that matches |
