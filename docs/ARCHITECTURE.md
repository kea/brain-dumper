# Architecture and field notes

This document covers how Brain Dumper is put together and, in the second half,
the hardware traps it fell into. The traps are the part worth reading: each one
cost real debugging time and none of them is obvious from the datasheets.

For what the device *does* and how to configure it, see the
[README](../README.md).

## Tasks

Everything is FreeRTOS tasks talking through queues and notifications. There is
no global lock other than LVGL's.

| Task | Core | Prio | Job |
|---|---|---|---|
| `buttons` | any | 6 | debounce, classify click / double / long, post to a queue |
| `app` | 1 | 5 | state machine, screen rendering, sleep policy |
| `lvgl` | 1 | 4 | `lv_timer_handler()`, and the panel flush inside it |
| `stt_sync` | any | 3 | clock sync and the transcription queue |
| `rec` / `play` | 0 | 7 | audio streaming to and from the card |
| `httpd` | any | 5 | the LAN web server, one request at a time |

Audio sits at the highest priority on the other core: a dropped I2S buffer is
an audible defect in a recording that cannot be re-taken, while a late screen
refresh is invisible on e-paper.

LVGL is not thread-safe. Everything touching an `lv_obj_t` runs between
`ui_lock()` and `ui_unlock()`, including the flush callback, which is called
from inside `lv_timer_handler()` on the `lvgl` task — **not** from whoever
called `ui_render()`. That asymmetry matters; see *Sleeping mid-refresh* below.

## Boot paths

There are two, decided by why the chip woke up:

**RTC alarm** → `headless_sync()` in `main.c`. Mount the card, bring up Wi-Fi,
drain the transcription queue against a four-minute budget, re-arm or stop the
countdown, and go straight back to deep sleep. The panel is never powered: the
whole point is that a queued note is transcribed overnight without lighting up
a screen nobody is looking at.

**Anything else** → the full UI: storage, timezone, RTC into system time,
panel, LVGL, sensors, audio, note catalogue, Wi-Fi (non-blocking), then
`app_start()`.

## The life of a note

1. **BOOT long** anywhere → `rec` task streams 16 kHz/16-bit/mono PCM to
   `notes/NNNN.wav`. The SHTC3 is read once at the start; temperature and
   humidity go into the note's metadata.
2. **BOOT long** again → stop. Under 700 ms the note is discarded.
3. Tag screen: Inbox / Idea / Todo / Lavoro / Personale.
4. The note enters the queue and `stt_sync` is notified.
5. `stt_sync` POSTs a multipart body — `model`, `response_format=json`,
   optional `language`, then the WAV — to `stt_url`, streaming the file in 4 KB
   chunks with an exact `Content-Length`. Chunked transfer-encoding upsets some
   of these servers, so the length is computed up front from the file size.
6. On success the transcript is written to `NNNN.txt` and the first words
   become the note title. On a *network* failure the note stays queued; on a
   *server* failure it is marked failed, because retrying forever against a
   server that dislikes the file is a battery leak.

Each note is three self-describing files. There is no index: `notes_init()`
rebuilds the catalogue by scanning the directory. A corrupt index is a class of
bug that cannot happen if there is no index.

## Display pipeline

```
model → ui_render() → LVGL widgets → lv_timer_handler() → flush_cb()
                                                            ↓
                                          epd_blit() → epd_flush() → panel
```

LVGL renders in **I1** (1 bit per pixel) into a 5008-byte buffer: 5000 bytes of
pixels plus an 8-byte palette header. The layout is byte-identical to the
panel's own — row-major, 25 bytes per row, MSB leftmost, set bit means white —
so the flush callback is a `memcpy` past the header, not a per-pixel conversion.
The vendor examples allocate an 80 KB RGB565 buffer for the same screen.

`ui_render()` rebuilds the entire body on every model change. That looks
wasteful until you price it: an e-paper refresh costs ~400 ms no matter how
little changed, so the CPU time to recreate a dozen widgets is noise, and it
removes every stale-widget bug at once.

Refreshes are **partial** by default and **full** every 12 frames or on any
screen change — the moment ghosting is most visible is when the layout changes
under it, and that is where the two seconds are best spent.

## Timekeeping

The PCF85063 holds UTC. `localtime()` applies the `timezone` string on the way
out. The RTC is the authority at boot; SNTP corrects it when a network appears.

`maybe_sync_time()` in `app.c` owns the policy and runs on every `stt_sync`
pass, independent of whether there is anything to transcribe:

- RTC reports no valid time → wait up to 15 s for the network, then sync.
- RTC is valid → sync only if already connected, then rest for six hours.
- Sync failed → try again on the next pass.

A successful sync writes back to the RTC, which is also what clears the
oscillator-stopped flag. See *A clock held hostage* below for why this needed
its own errand.

## Power

Three software-controlled rails: the panel and the audio codec are active-low
enables, and VBAT is an active-high latch the firmware holds to keep itself
alive. Dropping that latch is how the device powers itself off — and why
"power off" on USB degrades into deep sleep: the host is still feeding the rail.

Going to sleep, in order: render the farewell frame and **wait for it to reach
the glass**, arm or stop the RTC countdown, stop Wi-Fi, put the panel in its
own deep sleep with a proper power-down sequence, cut the panel and audio
rails, hold the enable pads, then `esp_deep_sleep_start()`. Wake sources are
BOOT, PWR and the RTC alarm line, all ext1 active-low.

## The web server

`components/web` puts an `esp_http_server` on port 80 whenever the radio has an
address, and takes it down again when the association drops. It is armed from
`app_main()` on the UI boot path only: `headless_sync()` exists to drain the
queue and go back to sleep, not to answer a browser nobody is holding.

The whole front end is one HTML file embedded in the binary. It fetches nothing
from the internet — no font, no script, no stylesheet — because the phone
reading it is on a network whose route to the internet is not this project's
business, and may not exist.

Three constraints shaped the rest of it.

**The catalogue moves under the reader.** `notes_at()` and `notes_by_id()` hand
back pointers into the array, and a delete `memmove()`s everything after the
hole. That is safe for the app task, which owns both the UI and every delete it
performs; it is not safe for a second task. The server uses `notes_get()` and
`notes_snapshot()` instead, which copy under the catalogue's own lock. The list
route allocates a snapshot in PSRAM, because 512 notes of JSON is far more than
one buffer's worth and the array must not be walked while a socket blocks.

**The card belongs to the recorder first.** Reading a 19 MB WAV over a one-bit
SDMMC bus while the recorder is writing to the same card is how an I2S buffer
gets dropped, and a dropped buffer is an audible hole in a recording nobody can
take again. Every route that touches the card answers 503 while audio is
running — and there is nothing to serve at all while the card is on loan to USB
transfer, which `storage_mounted()` already reports.

**Files need a real `Content-Length`.** `httpd_resp_send_chunk()` always
declares `Transfer-Encoding: chunked`, and a chunked `audio/wav` is a file
Safari will play from the start and refuse to seek in. Files therefore go out
through `httpd_send()` with a header block written by hand, which also makes
`Range` requests answerable — the page's scrub bar is a range request, so the
two requirements are the same requirement.

Sleep is the last piece. `web_idle_ms()` reports how long ago the last request
was served, and `maybe_sleep()` holds the device up for two minutes after it.
The page deliberately does not poll: a tab left open on a bedside table would
otherwise keep the radio alive all night, which is exactly the battery this
device spends its design budget protecting. Refreshing is a tap on the status
bar. `web_change_seq()` runs the other way, so a note deleted from a phone
leaves the device's own list on the next frame instead of sitting there as a
row that opens onto nothing.

There is no authentication, and adding one would be the wrong shape: a device
with two buttons cannot enrol a credential, and a shared secret in `config.ini`
is a plaintext password on a removable card. The honest control is the one that
exists — `web_enable=0`.

## USB

The ESP32-S3 has **one** USB PHY, shared between the built-in Serial/JTAG
(console, flashing) and the OTG controller that TinyUSB drives (mass storage).
They cannot both have it. Entering transfer mode therefore takes the console
away, and leaving it requires a reboot — plus handing the PHY back explicitly,
which is the subject of the next section.

---

# Field notes

Eight things that were not obvious. Each is a real failure that was observed,
diagnosed and fixed.

## The panel turned black on the way to sleep

**Symptom.** Entering deep sleep, the screen refreshed to a patchy, uneven
black instead of keeping its last frame.

**Cause.** Two of them, stacked. `panel_init_partial()` leaves the SSD1681 with
its clock and analog block enabled, so the charge pump was still holding ±15 V
on the source drivers when `board_power_epd(false)` removed VCC. Those
residual voltages then bled through the glass unevenly. On top of that, the
control pins were still driven high while the rail collapsed, back-feeding the
panel through its ESD diodes.

**Fix.** `epd_sleep()` now issues an explicit power-off (`0x22` with `0x83`,
then `0x20`) before the deep-sleep command, waits 100 ms for the internal rails
to bleed off, and floats RST/DC/CS before the caller cuts the supply.

## Sleeping mid-refresh

**Symptom.** Related to the above, and a second contributor to it.

**Cause.** `ui_render()` does not draw; it queues. The actual refresh happens
later on the `lvgl` task. The sleep path rendered a farewell frame, waited a
fixed 1200 ms and pulled the plug — but a full de-ghosting refresh takes ~2 s,
so the panel was cut off mid-waveform, with two tasks on the same SPI bus.

**Fix.** `ui_port` counts frames that actually reached the glass.
`ui_frame_count()` is sampled before the render and `ui_wait_frame()` blocks
until that frame lands, so the timing follows the panel instead of a guess.

## Floating pads in deep sleep

**Cause.** An unheld GPIO goes high-impedance the moment the digital domain
powers down. The panel and audio rail enables were left floating for the whole
sleep, which lets a supply drift back up under a controller nobody is driving.

**Fix.** `gpio_hold_en()` on both enables plus `gpio_deep_sleep_hold_en()`
before sleeping, and the matching `hold_dis` calls in `board_init()` — without
those, `gpio_set_level()` on a held pad is silently ignored after wake.

## The USB port that only a battery pull could fix

**Symptom.** After using transfer mode and pressing BOOT to leave, the device
vanished from USB entirely — not as a disk, not as a serial port. Only
disconnecting the battery brought it back.

**Cause.** Installing TinyUSB routes the shared PHY to the OTG core by writing
`sw_usb_phy_sel` in `RTC_CNTL_USB_CONF`. That register lives in the **RTC
domain, which a software reset does not clear**. So `esp_restart()` came back
with the Serial/JTAG console mapped to an external PHY this board does not
have, while the OTG core sat uninitialised: D+/D− driven by nobody.

**Fix.** `usb_msc_stop()` uninstalls TinyUSB and calls
`usb_serial_jtag_ll_phy_enable_external(false)` before restarting. The same
call runs at the top of `board_init()` as a safety net, so a crash in transfer
mode costs a reboot instead of a battery pull.

## The port that disappeared every two minutes

**Symptom.** During development the board kept vanishing from `/dev/ttyACM0`.

**Cause.** Not a fault: deep sleep powers down the USB Serial/JTAG peripheral,
and VBUS is not a wake source. A sleeping board stays asleep with the cable
plugged in, and re-enumerates only when someone presses a button.

**Fix.** `maybe_sleep()` refuses to sleep while on external power.
`board_usb_powered()` combines two partial signals: `usb_serial_jtag_is_connected()`,
which counts SOF packets and so detects a host exactly, and the battery voltage
sitting above the charge-termination threshold, which catches a plain charger.

## Truncation that wasn't

**Symptom.** Note list rows overlapped, and worse once transcripts made the
titles longer.

**Cause.** `LV_LABEL_LONG_MODE_DOTS` truncates to the label's **size**, and
only the width had been set. With its height free to grow, the label wrapped to
two or three lines and spilled over the rows below instead of ellipsising.

**Fix.** Set the label height to `ROW_H` as well. Give DOTS a closed box and it
behaves.

## Two fonts, one of them blind

**Symptom.** Accented characters rendered on the note detail screen but not in
the list.

**Cause.** The bundled Montserrat faces cover ASCII only. The list correctly
skipped the ASCII-folding fallback when a card font was present — and then drew
the text with Montserrat anyway.

**Fix.** A second `lv_tiny_ttf` instance from the same in-PSRAM font data,
sized for list rows, used wherever transcript-derived text is shown. Because
`font.ttf` is whatever the user dropped on the card, the code checks the
resulting `line_height` against the row height and steps down a size rather
than letting rows collide.

## A clock held hostage

**Symptom.** The RTC reported `time LOST` at every single boot.

**Cause.** Not the backup cell. `net_time_sync()` was called from inside
`if (!net_is_connected())`, itself after a `notes_next_pending() == 0 →
continue` guard. With an empty queue the loop bailed out first; and with a
queue, `net_start()` had already run at boot, so the radio was *already*
connected and the branch never executed. The time was therefore never synced,
the RTC never written, and since only a write clears the oscillator-stopped
flag, it stayed marked lost forever.

**Fix.** The clock got its own errand, `maybe_sync_time()`, which runs before
the queue check and no longer depends on there being anything to send.

---

## Natural extensions

- Tag-based filtering in the note list — the field is already stored.
- Local LLM summarisation or to-do extraction, reusing the STT client pattern.
- Segmented uploads for very long notes, so one recording cannot monopolise the
  server.
- Wall-clock RTC alarms instead of an interval countdown; the chip supports it
  and the driver already exposes the flags.
- Fully event-driven LVGL refresh — the task still polls with a timeout.
