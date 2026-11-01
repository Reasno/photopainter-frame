# PhotoPainter Frame

Daily photo frame for the Waveshare ESP32-S3-PhotoPainter (7.3" Spectra 6 e-paper), backed by Home Assistant.

- On battery: around 00:05 every day, or when BOOT or KEY is pressed, the frame wakes, joins Wi-Fi, reports its battery level and asks Home Assistant for a picture. KEY and scheduled updates show the next photo; BOOT shows the previous photo. It shows the picture, turns off Wi-Fi and the panel, and goes back to deep sleep. If the update fails it retries an hour later.
- On USB power: the frame stays awake with Wi-Fi connected and advances to the next picture, and reports the battery level, every 5 minutes. KEY advances immediately; BOOT goes back one photo. When USB is unplugged it returns to the daily battery schedule.
- Home Assistant traverses a shuffled list of all pictures, draws a status bar along the bottom and returns a 480×800 24-bit BMP. The list wraps at either end and keeps the same order across cycles. Its order and current position are saved across Home Assistant restarts.

The status bar is 40 px of solid white with black, red and green text and icons — no dithering — showing the capture date, the place (when known), and the battery level with an icon. Below 15% the battery turns red; while on USB it shows “充电”.

Home Assistant also exposes the frame as entities:

| Entity | Content |
| --- | --- |
| `sensor.photo_frame_battery` | Battery level reported on the last update |
| `binary_sensor.photo_frame_charging` | Charging or on USB power |
| `sensor.photo_frame_last_update` | Time of the last update |
| `sensor.photo_frame_photo` | File name of the current picture, with `taken` and `place` attributes |
| `image.photo_frame_current_photo` | Current picture including the status bar |

## Repository layout

| Path | Content |
| --- | --- |
| `firmware/` | ESP-IDF firmware for the frame |
| `ha/custom_components/photo_frame/` | Home Assistant integration that serves pictures and holds frame state |
| `tools/` | Mac tools that build the picture catalog |

## Pictures

Pictures must be 480×800, 24-bit BMP, already dithered to the six panel colors (black, white, yellow, red, blue, green). The integration does not dither; it only adds the status bar.

Put the pictures and a `photos.json` catalog in one directory that Home Assistant can read:

```json
[
  {"file": "family_0001.bmp", "taken": "2019-08-15T10:12:00", "place": "北京 北海"}
]
```

`taken` and `place` are optional. Pictures without a place show only the date.

### Building the catalog on a Mac

1. Export capture coordinates as `id<TAB>latitude<TAB>longitude` lines.
2. Reverse-geocode them once into Chinese address fields (uses Apple’s geocoder, no API key):

   ```bash
   swiftc -O -o /tmp/geocode_places tools/geocode_places.swift
   /tmp/geocode_places coords.tsv places.json
   ```

3. Combine a manifest (`[{"文件名", "拍摄时间", "图库标识"}]`) with the address fields:

   ```bash
   python3 tools/build_catalog.py manifest.json places.json photos.json
   ```

The place shown is the city plus the nearest point of interest or street, at most 7 characters.

### Weekly import of new photos

`tools/weekly/import_new_photos.py` adds new photos to the library on Home Assistant. It reads the Photos smart album `墨水相框人物` (People is any of the family members), keeps camera portrait photos captured after the watermark, scores sharpness and face quality, keeps the best shot of each burst (at most 4 per day, 40 per run), crops around the faces, dithers to the six panel colors, reverse-geocodes places, uploads the BMPs, appends them to `photos.json`, and moves the watermark forward.

```bash
python3 -m venv ~/.local/share/photopainter/venv
~/.local/share/photopainter/venv/bin/python -m pip install pillow
~/.local/share/photopainter/venv/bin/python tools/weekly/import_new_photos.py            # import
~/.local/share/photopainter/venv/bin/python tools/weekly/import_new_photos.py --dry-run  # preview only
```

State lives in `~/.local/state/photopainter/`: `config.json` holds the Home Assistant host (`{"remote": "root@<host>", "remote_dir": "<picture directory>", "ssh_options": []}`), `watermark.json` holds the newest capture time already examined, `weekly.log` records each run. The watermark only moves after a verified upload, so a failed run is retried from the same point. Home Assistant picks up the new `photos.json` on the next frame request; new pictures join the end of the shuffled list.

## Home Assistant

1. Copy `ha/custom_components/photo_frame` to `<config>/custom_components/photo_frame`.
2. Add a long random token to `secrets.yaml`:

   ```yaml
   photo_frame_token: "<random string>"
   ```

3. Add to `configuration.yaml` (or a package):

   ```yaml
   photo_frame:
     token: !secret photo_frame_token
     photo_dir: /media/tfcard/photopainter   # path inside the Home Assistant container
   ```

4. Restart Home Assistant.

Endpoint: `GET /api/photo_frame/next?token=<token>&battery=<0-100>&charging=<0|1>&direction=<next|previous>`. `direction` defaults to `next`; `previous` goes back one photo. It returns `image/bmp`; the `X-Frame-Time` header carries the server’s Unix time, which the frame uses as its clock. Requests with a wrong token get `401`; an invalid direction gets `400`.

The status bar font is [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) 12px (SIL OFL 1.1, see `fonts/OFL.txt`), rendered at 24 px without anti-aliasing.

## Firmware

Requires ESP-IDF 5.5 for `esp32s3`.

```bash
cd firmware
cp main/frame_secrets.example.h main/frame_secrets.h   # fill in Wi-Fi and token; this file is git-ignored
. $IDF_PATH/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

`FRAME_HA_URL` must point at Home Assistant on the local network, e.g. `http://192.168.1.10:8123/api/photo_frame/next`.

### Power

- On battery, Wi-Fi is on only while a picture is being fetched (about 5–10 s), then shut down before the panel refresh. On USB power Wi-Fi stays on.
- USB power is detected from the AXP2101 VBUS status at wake and checked every 5 s while awake.
- The e-paper panel is sent its deep-sleep command after the refresh (about 25 s).
- Before deep sleep the AXP2101 enters its sleep mode; unused rails, the panel rail (ALDO4) and the audio rail (ALDO3) are switched off, the speaker amplifier and LEDs are held off, and unused RTC pads are isolated.
- Wake sources: the ESP32-S3 timer (next 00:05 local time, China Standard Time) and the BOOT/KEY buttons (EXT1, active low). The timer runs on the internal RC oscillator, so the daily wake can drift by a few minutes; each successful update resets the clock from Home Assistant.

### Hardware notes

| Function | GPIO |
| --- | --- |
| E-paper DC / CS / SCK / MOSI / RST / BUSY | 8 / 9 / 10 / 11 / 12 / 13 |
| I2C SDA / SCL (AXP2101 0x34, PCF85063 0x51, SHTC3 0x70) | 47 / 48 |
| AXP2101 IRQ | 21 |
| BOOT / KEY | 0 / 4 |
| Green / red LED (active low) | 42 / 45 |
| Speaker amplifier enable | 7 |

Portrait pictures are drawn with portrait pixel (x, y) at panel pixel (y, 479 − x).
