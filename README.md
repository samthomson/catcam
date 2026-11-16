# Cat Cam

ESP32-CAM that takes a photo when the PIR motion sensor detects movement, uploads it to Blossom, and posts the URL to Nostr.

- ESP32-CAM (AI-Thinker) with OV2640 or OV3660 camera
- ESP32-CAM-MB USB base board
- Micro-USB cable
- PIR motion sensor — HC-SR501 type (ours: HW-416, i.e. an HC-SR501)

## Setup

1. Copy `include/secrets.h.example` to `include/secrets.h`
2. Edit with your WiFi credentials, Nostr private key, and Blossom server
3. Wire it up (see below)
4. Flash in place any time: spare jumper from `a20` (any free hole in column 20) → − rail, replug USB, `pio run -e esp32cam --target upload`, remove spare jumper, replug
5. Monitor (any time): `pio device monitor`
6. After the first wired flash, update over WiFi (no cables/strap): `pio run -e esp32cam -t upload --upload-protocol espota --upload-port <device-ip>`

## Wiring

**Visual wiring guide: [`wiring.html`](wiring.html)** — open it in a browser. Full table, diagram in this breadboard's orientation, assembly order.

Nothing plugs into anything directly. The CAM module is **not** stacked in the MB (stacking uses up every pin on both boards), and the PIR's pins are too short to seat in a breadboard — so all three parts hang off jumper cables, with the breadboard rails as the +5V/GND junction:

| # | Wire | From | To |
|---|------|------|-----|
| 1 | red M–M | MB socket 5V | + rail |
| 2 | black M–M | MB socket GND | − rail |
| 3 | red F–M | CAM pin 5V | + rail |
| 4 | black F–M | CAM pin GND | − rail |
| 5 | red F–M | PIR VCC | + rail |
| 6 | black F–M | PIR GND | − rail |
| 7 | yellow F–M | CAM pin 13 | breadboard d20 |
| 8 | yellow F–M | PIR OUT | breadboard e20 |
| 9 | blue F–M | MB socket U0T | CAM pin U0T (permanent) |
| 10 | blue F–M | MB socket U0R | CAM pin U0R (permanent) |
| 11 | white F–M | CAM pin 0 | breadboard b20 (permanent) |

MB's USB is the only power source. HC-SR501 needs the 5V rail (its output is 3.3V-safe for GPIO13). Turn both PIR potentiometers fully anticlockwise and set its jumper to H — the firmware handles re-trigger timing itself. Find every pin/socket by its printed label, not by position.

## Configuration

Edit `include/secrets.h`:

| Setting | Description |
|---------|-------------|
| `WIFI_SSID` | Your 2.4GHz WiFi network |
| `WIFI_PASS` | WiFi password |
| `NOSTR_PRIVKEY` | 64-char hex private key |
| `NOSTR_RELAY_HOST` | Nostr relay to post to |
| `BLOSSOM_SERVER` | Blossom upload URL |
| `CAMERA_NAME` | Name used in Nostr posts |

Motion behaviour (in `src/main.cpp`): photo on motion, minimum 5s between photos (`MOTION_COOLDOWN_MS`), PIR ignored for 30s after boot while it stabilizes (`PIR_WARMUP_MS`), and no new photo while the previous one is still uploading.
