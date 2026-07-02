This repo is Arduino firmware for an ESP32-WROOM-32 DevKit driving a 0.96"
SSD1306 128x64 I2C OLED at address 0x3C, wired SDA=GPIO21, SCL=GPIO22, VDD=3V3
(my board silkscreens VCC as VDD and SCL as SCK — it is still I2C, not SPI).
Compile with arduino-cli, core esp32:esp32, using an OTA-capable partition
scheme. Libraries: Adafruit SSD1306, Adafruit GFX, ArduinoJson, ESP32-OTA-Pull.
The device serves a config web portal on the LOCAL WiFi only (no cloud, no
Tailscale — target network has no smart-home infra). Code updates ship via
GitHub Releases + GitHub Actions OTA; content changes happen through the local
portal and persist in NVS. Always verify a change by running
`arduino-cli compile` and showing the exit code before considering work done.