> Part of [**app-pixels.com**](https://www.app-pixels.com) — browse + flash this app at [`/apps/http-trigger`](https://www.app-pixels.com/apps/http-trigger).

# http-trigger

**HTTP Trigger** · v1.0.0

A tiny configurable HTTP remote. Define screen layouts in a text file on the SD card, reboot, and start tapping.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#smart-home` `#tool`

Use it to control smart-home gear, fire Pushover / Discord / IFTTT webhooks, glance at a Bitcoin or GitHub stat, scrub the volume on a Sonos — any service with a REST endpoint.

Four widget types — **button**, **toggle**, **slider**, **display** — that can be combined in any order. Button screens stack 1–3 pills; display screens show 1–3 polled values. Sliders substitute `{value}` into the URL / body / headers. Toggles can optionally poll a status endpoint and reflect the real server state (white = active, grey = inactive).

## Controls

- **PWR** short — next screen · **BOOT** short — previous screen
- **Swipe left / right** — also navigates
- **Touch tap** — fires the widget
- **Slider drag** — vertical; sends 1 s after release so you can correct

## `setup.txt` keys

```
SSID            = your-wifi
PASSWORD        = your-password
BRIGHTNESS      = 180
TIMEOUT         = 300                 # idle seconds before auto-off; 0 = never
INFO_REFRESH_M  = 15                  # display + status re-poll interval, minutes
# HTTPS_INSECURE = true               # opt-out of cert verification (LAN with self-signed certs)
```

HTTPS endpoints are verified by default against the embedded Mozilla root CA bundle — works out of the box for CoinGecko, GitHub, Home Assistant Cloud, Tado, Pushover, and any other public-CA host. Set `HTTPS_INSECURE = true` only if your LAN endpoint presents a self-signed certificate.

## `http-trigger.txt` — screen definitions

INI-style sections. Up to 16 screens. Each `[screen]` carries shared `method` / `header` / `header2` / `header3` that cascade into its `[button]` or `[value]` sub-sections.

```ini
[screen]
type   = button
label  = Music
method = POST

  [button]
  label = Prev
  url   = http://192.168.1.70:5005/Kitchen/previous

  [button]
  label = Play
  url   = http://192.168.1.70:5005/Kitchen/playpause

  [button]
  label = Next
  url   = http://192.168.1.70:5005/Kitchen/next

[screen]
type      = toggle
label     = Lamp
label_on  = ON
label_off = OFF
method    = POST
header    = Authorization: Bearer YOUR_TOKEN
header2   = Content-Type: application/json
url_on    = https://home.example.com/api/services/switch/turn_on
body_on   = {"entity_id":"switch.lamp"}
url_off   = https://home.example.com/api/services/switch/turn_off
body_off  = {"entity_id":"switch.lamp"}
status_url       = https://home.example.com/api/states/switch.lamp
status_json_path = state

[screen]
type  = display
label = Bitcoin

  [value]
  label     = EUR
  url       = https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=eur
  json_path = bitcoin.eur
  unit      = EUR
  decimals  = 0
```

The shipped `http-trigger.txt` contains seven working screens covering every widget type. Edit, drop on the SD card, reboot.

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - Adafruit XCA9554
   - GFX Library for Arduino (moononournation)
   - SensorLib (lewishe)
   - XPowersLib (lewishe)
   - ArduinoJson (bblanchon)
   - TouchDrvFT6X36 (part of SensorLib)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/http-trigger_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/http-trigger_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/http-trigger).

## USB-MSC

While plugged into a PC over USB-C, the SD card is exposed as a removable drive — edit `setup.txt` / `http-trigger.txt` straight from the host, save, reboot.

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/http-trigger
