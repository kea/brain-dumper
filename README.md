# Brain Dumper

A voice-note recorder with **local** transcription, built on the **Waveshare
ESP32-S3-ePaper-1.54 V2** (the *non*-touch variant, ESP32-S3-PICO-1-N8R8).

Hold a button and talk. The device writes a WAV to the microSD, and as soon as
it sees your home network it hands the file to a speech-to-text service running
on your own machine and stores the transcript next to the audio. No cloud, no
account, no API key leaving the house.

The whole interaction budget is **two buttons and a 200×200 one-bit screen**,
which is the constraint that shapes everything else in this project.

## Status

Running on hardware. Verified end to end: boot on 8 MB flash + 8 MB octal
PSRAM, e-paper rendering, microSD mount, RTC and ambient sensor, ES8311 codec,
Wi-Fi association, NTP time sync with RTC write-back, transcription against a
local server, USB mass-storage transfer and return, the web UI from a phone on
the local network, deep sleep and wake.

Firmware size is ~1.42 MB in a 6 MB application partition, so there is room to
grow.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3-PICO-1-N8R8 — 8 MB flash (QIO), 8 MB PSRAM (octal) |
| Display | 1.54" e-paper, 200×200, monochrome, SSD1681-class, no touch |
| Audio | ES8311 mono codec — microphone and speaker on a single I2S bus |
| Storage | microSD, SDMMC in 1-bit mode |
| Clock | PCF85063A RTC with backup cell |
| Ambient | SHTC3 temperature / humidity |
| Input | two buttons only: BOOT and PWR |
| Power | Li-ion cell with a software-held VBAT latch, USB-C charging |

Full pin map, I2C addresses and battery calibration constants live in
[`components/board/include/board_config.h`](components/board/include/board_config.h).

## Build and flash

```bash
. $IDF_PATH/export.sh          # ESP-IDF 5.4 or newer, developed on 5.5.4
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Console and logs travel over the chip's native USB Serial/JTAG, so a plain
USB-C cable is the whole toolchain. Note that the port **disappears while the
device sleeps** — that is the peripheral powering down, not a fault. On USB
power the idle sleep is suppressed precisely so this does not happen mid-flash.

## Configuration

Everything lives in `/sdcard/config.ini`. The device writes a template there on
first boot from the build-time defaults (`idf.py menuconfig` → *Brain Dumper*)
and **never overwrites an existing file**. A reference copy is in
[`sdcard/config.ini`](sdcard/config.ini).

| Key | Meaning |
|---|---|
| `wifi_ssid`, `wifi_pass` | home network; leave empty to stay offline |
| `stt_url` | OpenAI-compatible transcription endpoint |
| `stt_model` | model name passed to that endpoint |
| `stt_lang` | ISO 639-1 code, empty for autodetect |
| `stt_key` | bearer token, if your server wants one |
| `timezone` | POSIX TZ string, e.g. `CET-1CEST,M3.5.0,M10.5.0/3` |
| `ntp_server` | defaults to `pool.ntp.org` |
| `idle_sleep_s` | inactivity before deep sleep; `0` disables |
| `wake_interval_min` | minutes between unattended RTC wake-ups |
| `max_record_s` | hard cap on a single recording |
| `web_enable` | `0` turns off the web UI and its API |

To edit it without a card reader, use **Menu → USB transfer**: the microSD
appears on your PC as a disk. Eject it there, then press BOOT to reboot.

### Transcription backend

Any server speaking the OpenAI `/v1/audio/transcriptions` multipart API will
do — whisper.cpp's `server`, LocalAI, faster-whisper-server, or
[Speaches](https://github.com/speaches-ai/speaches), which is what this build
was tested against:

```bash
docker run -p 8000:8000 ghcr.io/speaches-ai/speaches:latest-cuda
```

```ini
stt_url=http://192.168.1.10:8000/v1/audio/transcriptions
stt_model=Systran/faster-whisper-medium
stt_lang=it
```

Audio is uploaded straight from the card in 4 KB chunks with an exact
`Content-Length`: a ten-minute note is ~19 MB and must never be buffered in
RAM. Switching servers is one line in `config.ini`, not a rebuild.

### Accented characters

The Montserrat faces bundled with LVGL cover ASCII only. Drop a TrueType file
at **`/sdcard/font.ttf`** (under 512 KB) and transcripts, note titles and list
rows are rendered in full UTF-8 instead. The reference build uses
**`DejaVuSansCondensed.ttf`** — see [Credits](#credits).

Without that file the display folds accents down to ASCII (`perché` →
`perche'`) as a rendering fallback only; the `.txt` on the card always keeps
the original UTF-8.

## Controls

Two buttons, six gestures. Long press is 700 ms, the double-click window is
400 ms.

| Gesture | Action |
|---|---|
| BOOT click (▲) | Select / confirm |
| BOOT double (▲▲) | Back |
| BOOT long (▲) | **Start / stop recording — from any screen** |
| PWR click (▼) | Next item |
| PWR double (▼▼) | Previous item |
| PWR long (▼) | Power off (Home) · Delete note (List / Detail) |

Recording is deliberately one gesture away from everywhere: a voice-note device
that makes you navigate first has already lost the thought you wanted to catch.
A note shorter than 700 ms is discarded as a mis-touch.

## From your phone

While the device is awake and on your network it serves a small page at
**`http://<its-ip>/`**. The address is on the device itself, under *Menu →
Info*.

The page is the list of notes, each with its transcript, a player, a WAV and a
TXT download, and a delete. The header is the device's own status bar — clock,
battery, signal, free space, ambient reading, transcription queue — so opening
the page answers *is it alive and has it done its work* before you read a word.

Two things follow from what this device is, and both are deliberate:

- **It only answers while the device is awake.** On battery the radio goes down
  with the rest of the board after `idle_sleep_s`, and only a button on the
  device brings it back. Requests hold sleep off for two minutes each, so a
  page you are actively reading stays reachable; a tab left open in a pocket
  does not. The page never polls for the same reason — refreshing is a tap on
  the status bar.
- **There is no password.** Anyone who can reach the device on your network can
  read and delete every note on the card. That is the same trust boundary as
  the microSD itself, but it is a real one: `web_enable=0` in `config.ini` if
  you would rather not extend it.

Whatever is not the page is a small JSON API, which is the same thing a script
would use:

| | |
|---|---|
| `GET /api/status` | battery, radio, card, queue, sensors, uptime |
| `GET /api/notes` | the catalogue, newest first |
| `GET /api/notes/<id>` | one entry |
| `GET /api/notes/<id>/text` | the transcript, UTF-8 |
| `GET /api/notes/<id>/audio` | the WAV, with byte ranges so it seeks |
| `DELETE /api/notes/<id>` | removes all three files |

Add `?dl=1` to either file route to get it as a download rather than inline.
Anything that touches the card answers **503** while the device is recording or
playing, and while the card is on loan to USB transfer.

## On the card

```
/sdcard/
├── config.ini          # all configuration, editable from a PC
├── font.ttf            # optional, enables full UTF-8 on screen
└── notes/
    ├── 0001.wav        # 16 kHz / 16-bit / mono PCM
    ├── 0001.txt        # transcript, UTF-8
    └── 0001.ini        # id, created, duration_ms, temp_c, humidity, stt, tag, title
```

There is no central index. The catalogue is rebuilt by scanning the directory
at boot, so a note copied to a PC describes itself and there is no index file
that can rot out of sync with reality.

## Power behaviour

- **Idle sleep** after `idle_sleep_s` in Home, suspended whenever the device is
  on USB power. Wake sources are BOOT, PWR and the RTC alarm line.
- **The screen keeps the last frame** it was given — the title alone, so a
  sleeping device never shows a stale clock or a button hint it will not honour.
- **Unattended sync**: if notes are queued, the PCF85063 wakes the board every
  `wake_interval_min` minutes to drain the queue *without ever powering the
  panel*, then goes straight back to sleep. A note recorded on a walk is
  readable by morning.
- **Real power off** (PWR long in Home, or *Settings → Power off*) releases the
  VBAT latch. On USB the rail is fed by the host, so it degrades to deep sleep.

## Repository layout

```
main/          app.c   state machine, menu actions, transcription worker
               ui.c    LVGL rendering, one function per screen
components/
  board/       pins, power rails, battery ADC, deep sleep, buttons
  epd/         SSD1681 driver: partial/full refresh, panel sleep
  ui_port/     LVGL 9 bound to the panel in I1 (1 bpp)
  periph/      PCF85063 (RTC + countdown), SHTC3
  storage/     SD mount, config.ini parser, note catalogue
  audio/       ES8311 capture to WAV, playback, VU metering
  net/         Wi-Fi STA, SNTP, STT client
  web/         LAN HTTP server, JSON API, the page embedded in flash
  usb_msc/     USB mass-storage transfer mode
```

Further reading:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how the pieces fit, and the
  hardware traps this project fell into, with the fixes.
- [`docs/PLAN.md`](docs/PLAN.md), [`docs/DISCOVERY.md`](docs/DISCOVERY.md) —
  original development notes and vendor-example survey. **In Italian.**

## Credits

**Original idea and case design** — ["I Built a Minimalist AI Note
Device"](https://youtu.be/3t0k7E7WiOQ). This firmware is an independent
ESP-IDF implementation of that concept.

**Screen font** — [DejaVu Sans
Condensed](https://sourceforge.net/projects/dejavu/) (`DejaVuSansCondensed.ttf`),
copied to the card as `font.ttf`. The DejaVu family is released under a
permissive free licence derived from the Bitstream Vera fonts. It is not
redistributed in this repository; download it from the project page above.

**Board bring-up** — pin map and panel initialisation sequences derive from the
vendor examples at
[waveshareteam/ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54),
`02_Example/ESP-IDF/V2` (chiefly `11_FactoryProgram` and `04_SD_Card`).

Built on [ESP-IDF](https://github.com/espressif/esp-idf) and
[LVGL 9](https://github.com/lvgl/lvgl).
