# Brain Dumper

Registratore di note vocali con trascrizione locale, per **Waveshare
ESP32-S3-ePaper-1.54 V2** (versione *non* touch, modulo ESP32-S3-PICO-1-N8R8).

Registri tenendo premuto un tasto, il dispositivo salva un WAV sulla microSD e —
appena vede la rete di casa — lo manda a un servizio speech-to-text locale e ne
conserva la trascrizione accanto all'audio. Niente cloud, niente account.

- `docs/DISCOVERY.md` — cosa c'è negli esempi Waveshare, cosa ho riusato e perché
- `docs/PLAN.md` — funzionalità, architettura e piano di collaudo

## Hardware

| | |
|---|---|
| MCU | ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB PSRAM ottale) |
| Display | e-Paper 1.54" 200×200 monocromatico (SSD1681), non touch |
| Audio | ES8311 codec mono: microfono + speaker su un solo I2S |
| Storage | microSD, SDMMC a 1 bit |
| RTC | PCF85063A con batteria tampone |
| Ambiente | SHTC3 (temperatura / umidità) |
| Ingressi | due soli pulsanti: BOOT e PWR |

Pinout completo in `components/board/include/board_config.h`.

## Build

```bash
. $IDF_PATH/export.sh          # ESP-IDF 5.4 o superiore, testato su 5.5.4
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Console e log viaggiano sull'USB Serial/JTAG nativo: basta il cavo USB-C.

## Configurazione

Tutto sta in `/sdcard/config.ini`, che il dispositivo crea al primo avvio con i
default di `idf.py menuconfig → Brain Dumper`. Un modello è in
[`sdcard/config.ini`](sdcard/config.ini).

Serve almeno un servizio STT locale con API OpenAI-compatibile:

```bash
docker run -p 8000:8000 fedirz/faster-whisper-server:latest-cpu
```

poi in `config.ini`:

```ini
wifi_ssid=CasaMia
wifi_pass=...
stt_url=http://192.168.1.10:8000/v1/audio/transcriptions
stt_lang=it
```

### Accenti

I font Montserrat inclusi in LVGL si fermano all'ASCII. Copiando un
**`/sdcard/font.ttf`** (DejaVuSans, Noto Sans — sotto i 512 KB) le trascrizioni
vengono rese in UTF-8 completo. Senza, gli accenti sono ripiegati su ASCII solo
a schermo: il file `.txt` sulla scheda resta sempre integro.

## Comandi

| Gesto | Azione |
|---|---|
| BOOT click (▲) | Seleziona / conferma |
| BOOT doppio (▲▲) | Indietro |
| BOOT lungo (▲) | Avvia / ferma registrazione, da qualunque schermata |
| PWR click (▼) | Successivo |
| PWR doppio (▼▼) | Precedente |
| PWR lungo (▼) | Spegni (Home) · Elimina (Elenco / Dettaglio note) |

## Stato

Compila e gira sulla scheda: RTC, SHTC3 ed ES8311 rispondono. **La microSD non
viene rilevata** e anche l'esempio Waveshare fallisce allo stesso modo — la
diagnosi completa è in `docs/PLAN.md` §3.

## Crediti

Pinout e sequenze di inizializzazione del pannello derivano dagli esempi di
[waveshareteam/ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54),
cartella `02_Example/ESP-IDF/V2` (in particolare `11_FactoryProgram`).
