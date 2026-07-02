# Hardware

Arduino firmware for an ESP32-WROOM-32 DevKit driving a 0.96" SSD1306 128x64
I2C OLED at address 0x3C, wired SDA=GPIO21, SCL=GPIO22, VDD=3V3 (my board
silkscreens VCC as VDD and SCL as SCK — it is still I2C, not SPI). When
attached over USB the board usually shows up as `/dev/cu.usbserial-0001`.

# Build

Single sketch: `desk_display/desk_display.ino`. Compile with arduino-cli,
matching the release workflow's FQBN (OTA-capable partition scheme):

    arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=min_spiffs desk_display

Libraries: Adafruit SSD1306, Adafruit GFX, ArduinoJson, ESP32-OTA-Pull.
Always verify a change by running `arduino-cli compile` and showing the exit
code before considering work done.

# Architecture

- No cloud, no Tailscale — the target network has no smart-home infra. The
  device serves a config web portal on the LOCAL WiFi only; content changes
  (mode, countdown, city, message, tz) happen through the portal and persist
  in NVS. First boot / unreachable WiFi starts a captive-portal setup AP
  ("DeskDisplay-Setup" at 192.168.4.1).
- Portal JSON API: `GET /api/config` (returns config plus firmware `version`
  and `otaStatus` — the first thing to check when debugging),
  `POST /api/config`, `POST /api/checkupdate`, `POST /api/wifi`,
  `POST /api/wifireset`.
- The deployed device lives on WiFi "ORBI52", last seen at 192.168.1.182
  (DHCP — if it moved, scan the subnet for a host answering `/api/config`).
  `POST /api/checkupdate` triggers an OTA check remotely; the device blocks
  while checking and reboots if it updates.

# OTA / release flow

Code updates ship via GitHub Releases + GitHub Actions
(`.github/workflows/release.yml`). To release: bump `kFirmwareVersion` in the
sketch AND `version.txt` (the workflow fails the build if either mismatches
the tag), commit, then tag `vX.Y.Z` and push the tag. The workflow builds,
creates the GitHub Release, and commits `ota/manifest.json` +
`ota/desk_display-X.Y.Z.bin` to main. Devices poll the manifest via
raw.githubusercontent.com hourly, at boot, and on the portal's
"Check for update now" button.

Hard-won constraints — keep these invariants when touching OTA code:

- raw.githubusercontent.com serves with `cache-control: max-age=300`. The
  firmware cache-busts the manifest URL (`?nocache=<millis>`), and the
  workflow publishes the .bin under a versioned filename so a fresh manifest
  never points at a stale cached image. Reusing a fixed .bin filename or
  dropping the cache-buster reintroduces the "check for update does nothing
  right after a release" bug (root-caused 2026-07-02).
- The manifest is served from raw.githubusercontent.com (not release assets)
  because ESP32-OTA-Pull's HTTP client does not follow the 302 redirects
  GitHub release-asset URLs use.
- ESP32-OTA-Pull compares version strings byte-wise ("1.0.10" < "1.0.9"), so
  the sketch fetches the manifest itself and compares numerically
  (`compareVersions`), then calls the library with `AllowDowngrades(true)`
  purely to download/flash — the update decision must stay in the sketch.
- A release only becomes visible to devices once the Action finishes pushing
  the OTA commit to main (~2 min after tagging).
- The manifest `Board` field must stay `ESP32_DEV` (what `ARDUINO_BOARD`
  expands to for the `esp32:esp32:esp32` FQBN); a mismatch makes devices
  report "no build for this board".
