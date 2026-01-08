# Cat Cam

ESP32-CAM that takes photos and uploads them to Blossom.

## Hardware

- ESP32-CAM (AI-Thinker) with OV2640 or OV3660 camera
- ESP32-CAM-MB USB base board
- Micro-USB cable

## Setup

1. Copy `include/secrets.h.example` to `include/secrets.h`
2. Edit with your WiFi credentials, Nostr private key, and Blossom server
3. Plug ESP32-CAM into MB board, connect USB
4. Flash: `pio run -e esp32cam --target upload`
5. Monitor (Optional): `pio device monitor`

## Wiring

With ESP32-CAM-MB board: just plug the boards together (camera facing away from USB port) and connect USB. No extra wiring needed.

## Configuration

Edit `include/secrets.h`:

| Setting | Description |
|---------|-------------|
| `WIFI_SSID` | Your 2.4GHz WiFi network |
| `WIFI_PASS` | WiFi password |
| `NOSTR_PRIVKEY` | 64-char hex private key |
| `BLOSSOM_SERVER` | Blossom upload URL |
| `CAPTURE_INTERVAL_MS` | Time between photos (ms) |
