# Funzionalità e piano di sviluppo

## 1. Cosa fa il dispositivo

Il vincolo che detta tutto il resto: **due pulsanti**. Non ce ne sono altri e
il display non è touch. L'interazione è quindi sei gesti in croce, e la
registrazione deve essere raggiungibile da ovunque con un gesto solo — un
registratore di note vocali che ti obbliga a navigare prima ha già perso il
pensiero che volevi catturare.

| Gesto | Azione |
|---|---|
| BOOT click (▲) | Seleziona / conferma |
| BOOT doppio (▲▲) | Indietro |
| BOOT lungo (▲) | **Avvia / ferma registrazione** (da qualunque schermata) |
| PWR click (▼) | Elemento successivo |
| PWR doppio (▼▼) | Elemento precedente |
| PWR lungo (▼) | Spegni (Home) · Elimina nota (Elenco / Dettaglio) |

### Ciclo di vita di una nota

1. **BOOT lungo** → si accende il LED, parte la registrazione. Sullo schermo
   durata, VU meter e barra di riempimento verso `max_record_s`.
2. **BOOT lungo** → stop. Sotto i 700 ms la nota viene scartata (falso tocco).
3. Schermata **etichetta**: Inbox / Idea / Todo / Lavoro / Personale.
4. La nota finisce in coda. Appena c'è rete, il worker la manda al servizio STT
   locale e ne salva la trascrizione.
5. In **Note** si sfoglia l'elenco: durata, titolo (le prime parole della
   trascrizione) e un marcatore di stato — `~` in attesa, `!` fallita.
6. Nel **dettaglio** si legge la trascrizione impaginata, si riascolta l'audio,
   si elimina.

### Sul filesystem

```
/sdcard/
├── config.ini          # tutta la configurazione, editabile da PC
├── font.ttf            # opzionale: abilita l'UTF-8 completo a schermo
└── notes/
    ├── 0001.wav        # 16 kHz / 16 bit / mono PCM
    ├── 0001.txt        # trascrizione UTF-8
    └── 0001.ini        # id, created, duration_ms, temp_c, humidity, stt, tag, title
```

Niente indice centrale: il catalogo si ricostruisce scandendo la cartella
all'avvio. Una nota copiata su PC si descrive da sola e non c'è un file di
indice che possa corrompersi.

### Trascrizione

`POST` multipart verso un endpoint **OpenAI-compatibile**
(`/v1/audio/transcriptions`), che è ciò che parlano whisper.cpp `server`,
Speaches / faster-whisper-server e LocalAI. Il WAV viene trasmesso in streaming
dalla SD a blocchi di 4 KB — una nota da dieci minuti sono 19 MB e non deve mai
finire in RAM. Cambiare server è una riga in `config.ini`, non una ricompilazione.

Se manca la rete la nota resta in coda. Tre occasioni per smaltirla:
a fine registrazione, dal menu **Sincronizza ora**, e — la più interessante —
il **risveglio RTC non presidiato**: il PCF85063 sveglia la scheda ogni
`wake_interval_min` minuti *solo se ci sono note in coda*, che si trascrivono
senza mai accendere il pannello, e si torna in deep sleep. Una nota registrata
durante una passeggiata è leggibile la mattina dopo.

### RTC e sensore ambientale

Il **PCF85063** dà il timestamp reale anche a un dispositivo che non ha mai
visto una rete, e sopravvive al cambio batteria; SNTP, quando c'è, lo riallinea.
Lo **SHTC3** viene letto all'inizio di ogni registrazione e temperatura/umidità
finiscono nei metadati: contesto che costa 25 ms e rende il catalogo cercabile.

### USB

Console e log sul **USB Serial/JTAG** nativo. Da menu, **Trasferimento USB**
espone la microSD come disco: si scaricano note e trascrizioni e si edita
`config.ini` senza software dedicato. L'ESP32-S3 ha un solo PHY USB condiviso
fra Serial/JTAG e OTG, quindi entrare in modalità trasferimento toglie la
console: è una porta a senso unico, e infatti uscirne riavvia.

### Energia

- Deep sleep dopo `idle_sleep_s` di inattività in Home; risveglio ext1 su
  BOOT, PWR e la linea di allarme RTC.
- Il pannello va in deep sleep del controller (~1 µA) prima di dormire.
- Spegnimento vero (`PWR` lungo in Home, o *Impostazioni → Spegni*) sgancia il
  latch VBAT. Sotto USB il rail è alimentato dall'host, quindi degrada a deep sleep.
- Refresh parziale di default, completo ogni 12 frame o ad ogni cambio schermata:
  è lì che il ghosting si nota di più, e sono i due secondi meglio spesi.

## 2. Architettura

```
main/          app.c       macchina a stati + worker di trascrizione
               ui.c        rendering LVGL (ricostruzione completa per schermata)
components/
  board/       pin, rail, ADC batteria, deep sleep, pulsanti
  epd/         SSD1681 200×200 1bpp, refresh parziale/completo, sleep
  ui_port/     LVGL 9 in I1: buffer 5 KB, flush = memcpy
  periph/      PCF85063 (RTC + countdown), SHTC3
  storage/     mount SD, config.ini, catalogo note
  audio/       ES8311: registrazione WAV su SD, riproduzione, VU
  net/         Wi-Fi STA + SNTP, client STT
  usb_msc/     modalità trasferimento
```

Il rendering ricostruisce l'intero corpo della schermata ad ogni cambio di
modello. Sembra spreco, ma un refresh e-paper costa ~400 ms qualunque cosa sia
cambiata: il tempo CPU per ricreare una dozzina di widget è rumore, e in cambio
sparisce un'intera classe di bug da widget stantii.

## 3. Stato attuale

Compila pulito su ESP-IDF 5.5.4 (1,22 MB di flash su 6 MB di partizione,
~8,4 MB di heap libero fra RAM interna e PSRAM). **Provato sulla scheda**:
il firmware arriva a regime senza reboot.

| Sottosistema | Stato |
|---|---|
| Boot, 8 MB flash QIO, 8 MB PSRAM ottale | ✅ verificato |
| Rail di potenza, pulsanti | ✅ inizializzati |
| PCF85063 (RTC) | ✅ risponde, `time valid` |
| SHTC3 | ✅ risponde, `id=0x0887` |
| ES8311 | ✅ `Work in Slave mode` |
| e-Paper | ⚠️ init completo in 2,2 s con BUSY che scende regolarmente — **l'immagine va guardata a occhio** |
| microSD | ❌ nessuna risposta a `send_op_cond` |
| Wi-Fi / STT | ⏳ non ancora provabile: serve `config.ini` sulla scheda |

### La microSD

`sdmmc_init_ocr: send_op_cond (1) returned 0x107` (`ESP_ERR_TIMEOUT`): la
scheda non risponde nemmeno al primo comando. Diagnosi fatta:

1. **Non è il pinout.** CLK 39, CMD 41, D0 40 corrispondono sia a
   `11_FactoryProgram` sia a `04_SD_Card`.
2. **Non è lo strapping JTAG.** L'ipotesi era che GPIO3, che pilota il LED,
   selezionasse il pad-JTAG su GPIO 39–42. `espefuse summary` dice
   `STRAP_JTAG_SEL = False`: il pad-JTAG non è selezionabile, quei pin sono
   GPIO liberi.
3. **Non è il cablaggio.** Con un pull-down interno attivo tutte e tre le linee
   restano alte: i pull-up esterni ci sono e sono più forti di 45 kΩ.
4. **Non è il firmware.** L'esempio `04_SD_Card` di Waveshare, compilato e
   flashato tale e quale su questa scheda, fallisce in modo identico
   (`card_host == NULL`).

Resta il lato fisico: scheda non inserita o non scattata in sede, contatti
sporchi, oppure una card che l'host non digerisce. Da provare, in ordine:
una microSD **SDHC da 4–32 GB formattata FAT32**, reinserita fino allo scatto.
Le SDXC oltre i 64 GB in exFAT non vengono montate, e le SDUC non sono
supportate affatto.

Il firmware ora sopravvive all'assenza della scheda: la Home dice
«Nessuna microSD» e *Impostazioni → microSD* rimonta a caldo, dato che la
scheda non espone una linea di card-detect.

### Fase 1 — Bring-up

1. ~~**Alimentazione.**~~ Fatto. Restano i **pulsanti**: verificare che i sei
   gesti arrivino.
2. **e-Paper.** È il punto con più rischio: verificare che la Home compaia
   dritta e non a specchio. Se esce **in negativo**, invertire in `epd_blit()`;
   se esce **ruotata o riflessa**, agire su `lv_display_set_rotation()` oppure
   sul data-entry mode `0x11` in `panel_init_full()`.
3. ~~**I2C.**~~ Fatto: RTC e SHTC3 rispondono entrambi.
4. **microSD.** Vedi §3: prima serve una scheda che risponda.
5. **Audio.** Il punto da verificare per primo è l'`esp_codec_dev`: `audio.c`
   apre il codec a **2 canali** (l'API rifiuta un numero dispari) e tiene lo
   slot sinistro. Se il microfono risultasse sul destro, cambiare l'indice in
   `record_task()`. Controllare che `no_dac_ref = true` impedisca davvero il
   rientro del DAC nella registrazione.
6. **Batteria.** Confrontare `board_battery_voltage()` con un multimetro e
   tarare `BD_BAT_FULL_V` / `BD_BAT_EMPTY_V`.

### Fase 2 — Catena di trascrizione

7. Alzare un server STT locale, per esempio
   `docker run -p 8000:8000 fedirz/faster-whisper-server:latest-cpu`,
   e metterne l'URL in `config.ini`.
8. Registrare, verificare l'upload nei log di `stt`, controllare che il `.txt`
   compaia e che il titolo derivato sia sensato.
9. Provare i percorsi di errore: server spento, server che risponde 500, rete
   che cade a metà upload. La nota deve restare in coda solo nel primo caso.
10. **Risveglio RTC non presidiato**: registrare offline, lasciar dormire,
    verificare che al risveglio la coda si smaltisca senza accendere il pannello.

### Fase 3 — Rifiniture

11. Mettere `font.ttf` (DejaVuSans o Noto Sans, sotto i 512 KB) sulla scheda e
    verificare gli accenti; senza font, controllare il ripiegamento ASCII.
12. Misurare il consumo in deep sleep e tarare `idle_sleep_s`.
13. Verificare la modalità trasferimento su Linux, macOS e Windows.
14. Tarare il guadagno microfono: l'accuratezza di Whisper crolla sia col
    segnale troppo basso sia in saturazione.

### Fase 4 — Estensioni naturali

- **Ricerca per tag** nell'elenco note (il campo `tag` c'è già).
- **Riassunto o estrazione di to-do** via LLM locale, riusando lo stesso
  pattern del client STT.
- **Note lunghe a segmenti**: spezzare i WAV oltre i N minuti e inviarli a
  pezzi, per non tenere occupato il server troppo a lungo.
- **Sveglia RTC su orario** invece che a intervallo (il chip supporta l'alarm
  su ora/minuto; il driver espone già i flag).
- **Refresh LVGL guidato dagli eventi**: oggi il task fa polling con timeout;
  legarlo interamente a `ui_notify()` toglierebbe qualche milliampere.
