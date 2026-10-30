# ESPHome web radio

Firmware for a small internet radio built on ESPHome: an ESP32-S3 with a MAX98357A amplifier,
an ST7735 160×128 display and a rotary encoder with push button.

- Internet radio from a station list stored on the device (MP3, Opus and FLAC streams).
- Music Assistant playback through Sendspin.
- Display UI built with LVGL: clock, now playing, station carousel with logos, settings, info.
- Web page on the device for playback control and for adding, renaming and sorting stations.
- Works without Home Assistant. Home Assistant is optional and only adds entities and actions.

## Hardware

| Part | Pins |
|------|------|
| ESP32-S3, 8 MB flash, 2 MB PSRAM | |
| MAX98357A (I²S) | LRCLK GPIO5, BCLK GPIO4, DIN GPIO6 |
| ST7735 160×128 (SPI) | CLK GPIO16, MOSI GPIO15, CS GPIO17, DC GPIO18, RST GPIO8, backlight GPIO7 (PWM, inverted) |
| Rotary encoder | A GPIO38, B GPIO47, button GPIO21 |

The display is used in landscape (LVGL rotation 270°). The ST7735 controller has 132×162 pixels of
memory; `offset_*` and `pad_*` in `webradio.yaml` describe that window. With wrong pads a strip of
noise shows at one edge.

## Use

Add the package to a device file, as in [`example/web-radio.yaml`](example/web-radio.yaml):

```yaml
esphome:
  name: web-radio
  friendly_name: Web Radio

packages:
  webradio:
    url: https://github.com/mvoss96/esphome-webradio
    files: [webradio.yaml]
```

The package expects the device file (or another package) to provide `logger`, `api`, `ota`,
`wifi` and two sensors with these ids, which the info page shows:

- `wifi_signal_strength` (`wifi_signal` sensor)
- `ip_address` (`wifi_info` text sensor)

Tested with ESPHome 2026.10.1.

Per-device settings go under `substitutions:` in the device file:

| Substitution | Default | Purpose |
|--------------|---------|---------|
| `highpass_frequency` | `90Hz` | Lowest frequency sent to the speaker; 90 Hz fits a Visaton FRS 8 M in a closed 1 l box. |

## Operation

| Screen | Turn | Click | Hold |
|--------|------|-------|------|
| Clock | Volume | Main menu | – |
| Now playing | Volume | Radio: station list; Music Assistant: main menu | Stop, then main menu |
| Menus | Move | Select / edit | Back |
| Equalizer | Select band / change it | Edit on/off | Back |

The first main menu tile shows what plays (the station or Music Assistant) and leads back to
it; with nothing playing it is "Zurück" to the clock.

The equalizer page (settings menu → Equalizer) shows bass, mid and treble as sliders.

While the display is dimmed, the first input only wakes it. The dim time is a setting in the
settings menu (and the Home Assistant entity "Display abdunkeln nach"); 0 turns dimming off.

## Web page

`http://<radio>/` shows the playing station, a stop/play button, the volume and a tile per station.
"Einstellungen" (`/#einstellungen`) sets bass, mid, treble and the dim time.
"Sender verwalten" (`/#sender`) edits the list: search radio-browser.info, add, rename, drag to sort,
remove, then "Aufs Radio übertragen". Search and logos are loaded by the browser, not by the radio.

API used by the page:

| Request | Body / response |
|---------|-----------------|
| `GET /api/status` | JSON: `station`, `playing`, `ma`, `volume`, `muted`, `ha`, `bass`, `mid`, `treble`, `dim` |
| `GET /api/stations` | one `name<TAB>stream-url<TAB>logo-url` line per station |
| `POST /api/stations` | same text format; replaces the list |
| `POST /api/play` | station index |
| `POST /api/volume` | 0–100 |
| `POST /api/stop` | – |
| `POST /api/setting` | `key=value` lines; keys `bass`, `mid`, `treble` (dB), `dim` (seconds) |

## Home Assistant

Optional. The radio does not reboot without an API connection; the info page and the web page show
whether Home Assistant is connected. Actions:

- `esphome.<name>_set_stations` with `names`, `urls`, `logos` (lists of equal length)
- `esphome.<name>_play_station` with `nummer` (1-based)

## Components

| Component | Purpose |
|-----------|---------|
| `equalizer` | Speaker platform with biquad filters between the media player and the I²S speaker. |
| `station_logos` | Looks up station logos (radio-browser.info), resizes them via wsrv.nl, keeps them in PSRAM. |
| `radio_ui` | C++ helpers for the LVGL carousel. |
| `radio_web` | The web page and its API on ESPHome's web server. |

## Memory

The module has only 2 MB PSRAM. `psram: ignore_not_found` must stay at its default; with `false`
ESPHome enables network buffers sized for 8 MB modules and allocations start failing while streaming.
The Sendspin buffer is limited to 300 kB for the same reason.
