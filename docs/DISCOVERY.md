# Discovery: cosa c'è negli esempi Waveshare e cosa ne ho preso

Repo esaminato: `waveshareteam/ESP32-S3-ePaper-1.54`, cartella
`02_Example/ESP-IDF/V2` (13 esempi). Scheda di riferimento: **V2 non-touch**,
modulo **ESP32-S3-PICO-1-N8R8**.

## 1. Mappa degli esempi

| Esempio | Cosa contiene di utile | Verdetto |
|---|---|---|
| `01_ADC_Test` | `adc_bsp`: ADC1_CH3 (GPIO4) + calibrazione curve-fitting, divisore 1:2 | **Riusato** (logica in `components/board/board.c`) |
| `02_I2C_PCF85063` | Usa `SensorLib` (libreria Arduino-oriented, ~40 file, driver touch inclusi) | **Scartato**: driver riscritto in 180 righe |
| `03_I2C_SHTC3` | Idem, via SensorLib | **Scartato** in favore di `11` |
| `04_SD_Card` | `sdcard_bsp`: SDMMC slot 1 a 1 bit, pin 39/40/41 | **Riusato** (schema di mount) |
| `05/06_WIFI_*` | Boilerplate STA/AP standard IDF | Riscritto (event-driven, retry, PS) |
| `07_BATT_PWR_Test` | `board_power_bsp`: rail EPD/Audio/VBAT + `EnableDeepLowPowerMode()` | **Riusato**: maschera ext1 e hold RTC del pin VBAT |
| `08_Audio_Test` | `codec_board/board_cfg.txt`: **la sola fonte del pinout I2S** | **Riusato il pinout**, scartato `codec_board` |
| `09_LVGL_V8_Test` | LVGL 8 + GUI Guider | Scartato (V8) |
| `10_LVGL_V9_Test` | Integrazione LVGL 9: framebuffer RGB565 200×200×2 = **80 KB** convertito pixel-per-pixel a 1bpp ad ogni flush | **Scartato l'approccio**, vedi §3 |
| `11_FactoryProgram` | `port_bsp/`: display, power, i2c, sdcard, shtc3, adc, codec, lvgl. **Il riferimento migliore del repo** | **Base principale** |
| `12_RTC_Sleep_Test` | Deep sleep + sveglia RTC, `get_wakeup_gpio()` | **Riusato** lo schema di risveglio |
| `13_FT6336_Test` | Touch controller | **Non applicabile** (versione non-touch) |

## 2. Pinout consolidato

Da `11_FactoryProgram/components/port_bsp/epaper_config.h` +
`08_Audio_Test/components/codec_board/board_cfg.txt`, riga `Board: S3_ePaper_1_54`.
Trasferito integralmente in `components/board/include/board_config.h`.

| Blocco | Pin |
|---|---|
| e-Paper SPI2 | BUSY 8, RST 9, DC 10, CS 11, SCK 12, MOSI 13 |
| Rail di potenza | EPD_PWR 6 (attivo basso), AUDIO_PWR 42 (attivo basso), VBAT 17 (attivo alto) |
| Pulsanti | BOOT 0, PWR 18 (entrambi attivi bassi) |
| LED | 3 (attivo basso) |
| I2C | SDA 47, SCL 48 — RTC 0x51, SHTC3 0x70, ES8311 0x18 |
| RTC INT | 5 (sorgente di risveglio ext1) |
| microSD (SDMMC 1-bit) | CLK 39, CMD 41, D0 40 |
| I2S / ES8311 | MCLK 14, BCLK 15, WS 38, DOUT 45, DIN 16, PA 46 |
| Batteria | ADC1_CH3 (GPIO4), abilitato da GPIO17 |

Note emerse dalla lettura:

- **La microSD ha solo 3 fili**: niente D1/D2/D3, niente card-detect. Modalità
  1-bit obbligatoria; l'inserimento a caldo non è rilevabile.
- **L'ES8311 è un codec mono** in configurazione `in_out`: microfono e speaker
  condividono la stessa porta I2S, quindi registrazione e riproduzione sono
  mutuamente esclusive *a livello hardware*, non per scelta di design.
- **GPIO 45 e 46 sono strapping pin** dell'ESP32-S3 e la scheda li usa comunque
  per I2S DOUT e abilitazione PA. È il design del costruttore, non un errore.
- `sdkconfig.defaults` degli esempi dichiara **4 MB di flash**: sbagliato per un
  N8R8. Corretto a 8 MB con una tabella partizioni dedicata.

## 3. La decisione sul layer grafico

L'integrazione LVGL di Waveshare (`10_LVGL_V9_Test/main.cpp`, `11/port_lvgl.cpp`)
alloca uno o due framebuffer RGB565 da 80 KB in PSRAM e, nella `flush_cb`,
scorre 40 000 pixel convertendoli uno a uno a bianco/nero.

Su un pannello 200×200 monocromatico è spreco puro. LVGL 9 supporta
nativamente **`LV_COLOR_FORMAT_I1`**, e il suo layout — righe consecutive,
25 byte per riga, MSB = pixel più a sinistra, bit a 1 = luminanza > 127 — è
**identico byte per byte** al framebuffer che vuole l'SSD1681.

Risultato: `components/ui_port/ui_port.c` usa un buffer da 5 008 byte
(5 000 di pixel + 8 di palette) e la `flush_cb` è una `memcpy`.

| | Waveshare | Qui |
|---|---|---|
| Framebuffer LVGL | 80 KB (×2 in `11`) | 5 008 B |
| Costo per flush | 40 000 iterazioni | 1 `memcpy` |
| Dove vive | PSRAM | RAM interna |

LVGL resta comunque la scelta giusta rispetto a una mini-GUI custom, per un
motivo solo: il **font engine**. Le trascrizioni sono testo libero in italiano
che va mandato a capo, impaginato e scorso. Scrivere a mano un renderer di
font bitmap con word-wrap costa più di quanto costi LVGL, che qui pesa 5 KB di
RAM.

Unico limite reale: i font Montserrat inclusi in LVGL coprono `0x20-0x7F` più
grado e bullet — **niente lettere accentate**. Gestito così: se sulla SD esiste
`/sdcard/font.ttf` viene caricato con Tiny TTF e il testo è UTF-8 completo;
altrimenti gli accenti vengono ripiegati su ASCII (`perché` → `perche'`) **solo
in visualizzazione**, mentre il `.txt` sulla scheda resta sempre UTF-8 integro.

## 4. Cosa è stato riscritto e perché

| Componente | Origine | Perché non ho riusato il codice |
|---|---|---|
| `epd` | `port_display.cpp` | Portato a C, aggiunti sleep/wake del pannello, refresh completo periodico anti-ghosting, bounce buffer per le LUT (che stanno in flash e il DMA SPI non può leggerle) |
| `pcf85063` | componente `waveshare/pcf85063a` | Serviva il **countdown timer** a 1/60 Hz per le sveglie periodiche, che il componente non espone |
| `shtc3` | `port_shtc3.cpp` | Logica identica, ripulita e con verifica dell'ID; mantenuto l'offset di autoriscaldamento di 4 °C del firmware di fabbrica |
| `buttons` | `espressif/button` | L'API cambia fra major version; 150 righe di debounce eliminano il rischio e danno controllo sulle soglie |
| `audio` | `codec_board` + `port_codec.cpp` | `codec_board` fa il parsing di un file di testo per scoprire pin che qui sono costanti note. Init diretto di `esp_codec_dev` + ES8311 |
| `ui_port` | `port_lvgl.cpp` | Vedi §3 |
