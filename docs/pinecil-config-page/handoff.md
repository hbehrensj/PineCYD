# Handoff: Pinecil-konfigurationsside til PineCYD

## Overview
PineCYD (ESP32 "Cheap Yellow Display", `firmware/fase2-ember-design`) viser i dag kun live
data fra en tilkoblet Pinecil V2 (tip-temp, setpoint, watt osv. - alt read-only, se
`README.md`'s GATT-tabel). Denne opgave tilføjer en **web-side**, `pinecyd.local`, hvorfra
Pinecillens egne indstillinger kan læses og ændres, mens den er BLE-forbundet til PineCYD.

**Dette er ikke en LVGL-skærm** som resten af projektet - det er endnu en rute på den
ESP32 `WebServer` der allerede kører (se `settingsServer` i `main.cpp`, i dag kun brugt til
Claude-bridge-adresse + tidszone på `/`). Denne handoff er input til en designsamtale, ikke
en oversættelse af en eksisterende mockup - der findes ingen visuel reference for denne
side endnu, i modsætning til de to tidligere handoffs i `docs/`.

**Eksplicit non-goal:** hurtige tip-temp-opdateringer er ikke vigtige på denne side (i
modsætning til hoved-dashboardet). Værdier hentes/vises når siden loader eller gemmes -
ingen live-polling-loop nødvendig her.

## Afhængighed
Kræver at WiFi og BLE kan køre samtidig (planlagt, ikke lavet endnu) - i dag slukker WiFi
præcis når en Pinecil forbinder (se WiFi on/off-toggle i `loop()`). Denne side er kun
reachable mens begge dele er oppe samtidig.

## Ude af scope (bevidst udeladt)
- **BluetoothLE-indstillingen selv (index 37).** Den har 3 tilstande (0=slukket, 1=tændt
  read-write, 2=tændt read-only) og styrer om denne sides skriv-kald overhovedet virker.
  At udstille den her er en hønen-og-ægget-risiko: sætter man den til 2 fra siden, mister
  siden sin egen skriveadgang på næste kald. Kun ændrbar fra Pinecillens eget fysiske menu
  (Advanced > BluetoothLE) - ikke noget denne side kan eller bør løse.
- **Factory reset** (en separat "RESET"-kommando i firmwaren, ikke en almindelig
  indstilling - se nedenfor) - wiper **alle** 56 indstillinger på én gang. Anbefaling:
  udelad helt fra v1, eller gem bag en tydeligt adskilt, dobbelt-bekræftet flow hvis den
  overhovedet skal med - din beslutning, ikke forudsat her.

## Hvad der allerede findes at genbruge (nuværende præcedens)
Den eksisterende `/`-side (`handleSettingsRoot()` i `main.cpp`) er bevidst spartansk: ét
inlinet HTML-string, ingen CSS-farver/branding, `<input>`-felter, `sans-serif`, `max-width:
420px`. Ingen Ember-farvepalet er brugt der i dag. Samme mønster (enkelt embedded
HTML-string, POST til en `/save`-lignende rute, eksplicit `Connection: close`-header pga.
en tidligere live-fundet reload-bug) bør genbruges for den nye side - tilføj en ny rute på
samme `settingsServer`, ikke en ny server-instans.

**Åbent designspørgsmål:** skal denne side være lige så bar-bones som den eksisterende,
eller skal den bruge Ember-paletten (tabel nedenfor) for visuel sammenhæng med
ur-skærmen? Begge er gyldige - dette er ikke forudbestemt af noget i koden i dag.

| Token | Hex | Bruges i dag til |
|---|---|---|
| `ember-bg` (`COLOR_BLACK`) | `#0D0D0D` | Skærmbaggrund (LVGL) |
| `ember-surface` | `#241C14` | Usage-bar track (tom) |
| `ember-core` | `#FF6A2B` | Usage-bar fill, normal |
| `ember-alert` | `#FF3B2F` | Usage-bar fill, kritisk/over-pace |
| `ember-ash` | `#8A7A6A` | Sekundær tekst |
| `ember-text` (`COLOR_WHITE`-ish) | `#F2E7D8` | Primær tekst/tal |

Bemærk: en tidligere gradient (`ember-hot` #FFB03A som fill-endpoint) blev forsøgt og
**droppet** 2026-07-15 pga. uløst rendering-banding på den rigtige skærm - ikke noget at
genindføre her.

## Hardware/tekniske begrænsninger
- **Flash er stramt** (~89% af OTA-partitionen brugt pr. denne session, se `README.md`) -
  undgå store CSS/JS-frameworks, webfonte eller billeder. Minimal inline CSS er fint.
- **Ingen HTTPS** - almindelig HTTP på LAN, samme som den eksisterende side.
- **Indstillinger ligger kun i RAM indtil et eksplicit SAVE-kald** (se protokolafsnit
  nedenfor) - siden skal have en tydelig, eksplicit "Gem"-handling. Der er ingen
  auto-save-per-felt, for det ville stille kunne miste ændringer ved næste
  strømafbrydelse/reboot uden varsel.
- Skal virke fra både telefon- og laptop-browser rettet mod `pinecyd.local` - ingen
  responsivt framework antaget, men layoutet må ikke knække ved smalle bredder.
- Kun tilgængelig mens en Pinecil er BLE-forbundet - siden skal have en tydelig
  "ikke forbundet"-tilstand (fx en besked + evt. auto-reload), ikke en tom/broken formular.

## Protokol-mekanik (vigtigt for interaktionsdesignet)
Bekræftet direkte fra `Ralim/IronOS`-kildekoden (`ble_characteristics.h`,
`ble_handlers.cpp`, `ble_peripheral.c`), ikke antaget:
- Hver indstilling har sin egen BLE-karakteristik, adresseret `f6d70000 + index`, læst og
  skrevet som en rå **uint16**. Der er ingen samlet "hent alle indstillinger"-karakteristik
  - PineCYD skal læse/skrive én ad gangen (som det allerede gør for de 6
  dashboard-karakteristika i dag).
  - Enum-agtige indstillinger (fx `OrientationMode`, `AutoStartMode`, `LockingMode`) skal
  vises som labels/dropdowns i UI'et, ikke som rå tal.
- **Gem er en separat handling:** skriv `1` til en dedikeret SAVE-karakteristik
  (`f6d7FFFF`) for at persistere RAM-værdierne til flash. Uden dette kald overlever
  ændringer ikke en genstart.
- Der findes også en RESET-karakteristik (`f6d7FFFE`, skriv `1` = factory reset alt) - se
  "ude af scope" ovenfor.
- Skriv afvises **stille** (BLE-fejlkode, ikke en synlig fejl på Pinecillen) hvis
  `BluetoothLE`-indstillingen (#37, ikke en del af denne side) står på read-only (2). Siden
  bør have en fejltilstand for "gem fejlede" der forklarer dette, ikke bare tie stille.

## Indstillinger til siden (alle 41 der faktisk er BLE-tilgængelige, undtagen #37)

Kun 41 af firmwarets 56 definerede indstillinger har rent faktisk en registreret
BLE-karakteristik (bekræftet i `ble_peripheral.c`, ikke kun enum-listen) - resten
(14 "profile mode"-indstillinger #39-52, som Pinecil V2-firmwaren ikke engang er
kompileret med, samt #55 knap-ombytning) findes slet ikke over BLE og kan ikke være med,
uanset ønske.

### Lodning/setpoint
| # | Navn | Range (raw) | Default | Noter |
|---|---|---|---|---|
| 0 | SolderingTemp | 10-450°C (50-850°F) | 320°C | Selve setpoint - det eneste feltet der allerede vises read-only på hoveddashboardet |
| 22 | BoostTemp | 250-450°C (480-850°F) | 420°C | Boost-mode target |
| 15 | TemperatureInF | 0/1 | 0 (°C) | Toggle - påvirker hvordan andre temp-felter bør vises/indtastes |
| 27 | TempChangeShortStep | 1-50°/°F | 1 | Knap-increment, kort tryk |
| 26 | TempChangeLongStep | 5-90°/°F | 10 | Knap-increment, langt tryk |
| 25 | ReverseButtonTempChangeEnabled | 0/1 | 0 | Byt om på +/- knapperne |
| 10 | AutoStartMode | 0-3 | varies | 0=ingen, 1=solder, 2=sleep-temp, 3=zero-power - vis som dropdown med labels |
| 17 | LockingMode | 0-2 | 0 | 0=fra, 1=kun boost, 2=fuld - dropdown med labels |
| 24 | PowerLimit | 0-120W, trin 5 | 0 (ingen grænse) | Direkte watt, ingen skalering |

### Søvn/idle
| # | Navn | Range (raw) | Default | Noter |
|---|---|---|---|---|
| 1 | SleepTemp | 10-450°C (50-850°F) | 150°C | Temp i sleep-mode |
| 2 | SleepTime | 0-15 | 5 | **Skal verificeres før implementering:** delt enum-kommentar siger "minutter", men Pinecil V2's egen `configuration.h` siger raw-værdi × 10 = sekunder (default 5 → 50 sekunder) - modstridende kilder, samme type x10-skalerings-fælde projektet har ramt før (se `dc_input`/`est_watts`). Test på rigtig hardware før UI-enheden låses fast. |
| 11 | ShutdownTime | 0-60 | 10 | Minutter til fuld nedlukning |
| 7 | Sensitivity | 0-9 | 7 | Bevægelsessensor (accelerometer) |
| 28 | HallEffectSensitivity | 0-9 | 7 | Magnetsensor til søvn - ulineær skala internt, vis som simpelt 0-9 slider, ikke den underliggende tærskel-tabel |
| 53 | HallEffectSleepTime | 0-12 | 0 | Sekunder/5 til søvn via hall-sensor |
| 12 | CoolingTempBlink | 0/1 | varies | Blink temp-visning under nedkøling til <50°C |

### Strømkilde
| # | Navn | Range (raw) | Default | Noter |
|---|---|---|---|---|
| 3 | MinDCVoltageCells | 0-4 | 0 | **Bekræftet fra kilden:** 0=DC (fast 9V gulv), 1=3S, 2=4S, 3=5S, 4=6S battericeller - vis som dropdown med disse labels, ikke rå tal |
| 4 | MinVoltageCells | 24-38 | 33 | Min. spænding pr. celle × 10 (2.4-3.8V) - kombineres med #3 til en reel afskæringsspænding, se kildekommentar i `Settings.cpp:lookupVoltageLevel()` |
| 5 | QCIdealVoltage | 90-220, trin 2 | 90 | x10 volt (9.0-22.0V) - matcher kommentaren "(9,12,20V)"; Pinecil V2 understøtter 20V-tier |
| 32 | PDNegTimeout | 0-50 | 20 | PD-forhandlingstimeout, ×100ms |
| 38 | USBPDMode | 0-2 | varies | 0=kun fast PDO, 1=PPS+EPR+ekstra effekt, 2=PPS+EPR sikker - dropdown med labels |

### Skærm/UI (på selve Pinecillens display, ikke PineCYD's)
| # | Navn | Range (raw) | Default | Noter |
|---|---|---|---|---|
| 6 | OrientationMode | 0-2 | varies | 0=højre, 1=venstre, 2=auto - dropdown |
| 34 | OLEDBrightness | 1-101, trin 25 | 26 | |
| 33 | OLEDInversion | 0/1 | 0 | Inverter farver |
| 35 | LOGOTime | 0-6 | 1 | Boot-logo varighed |
| 16 | DescriptionScrollSpeed | 0/1 | 0 | |
| 13 | DetailedIDLE | 0/1 | varies | Udførligt idle-skærmbillede |
| 14 | DetailedSoldering | 0/1 | varies | Udførligt lodde-skærmbillede |
| 8 | AnimationLoop | 0/1 | varies | |
| 9 | AnimationSpeed | 0-3 | varies | |
| 31 | UILanguage | 16-bit sprogkode | dansk/engelsk? | Rå kode, ikke et menneskeligt navn ud af boksen - kræver et opslag hvis den skal vises pænt |

### Keep-awake pulse (forhindrer PD-genforhandling ved lang idle)
| # | Navn | Range (raw) | Default | Noter |
|---|---|---|---|---|
| 18 | KeepAwakePulse | 0-100, trin 1 | 0 eller 5 | ×0.1W (10 = 1W) |
| 19 | KeepAwakePulseWait | 1-9 | 4 | ×2.5s (4 = 10s) |
| 20 | KeepAwakePulseDuration | 1-9 | 1 | ×250ms (1 = 250ms) |

### Avanceret/kalibrering - anbefales en tydeligt adskilt "Advanced"-sektion med advarsel
| # | Navn | Range (raw) | Default | Noter |
|---|---|---|---|---|
| 21 | VoltageDiv | 360-900 | 630 | Hardware-kalibreringsfaktor for spændingsmåling (fra skematik) - **ikke** noget en almindelig bruger bør skrue på, kan gøre spændingsvisningen forkert hvis ændret uden grund |
| 23 | CalibrationOffset | 100-2500 | 900 | Tip-specifik ADC-offset (µV) - kalibreringsværdi, samme forbehold som ovenfor |
| 36 | CalibrateCJC | 0/1 | 0 | Flag: udløs CJC-kalibrering ved næste boot - engangshandling, ikke en vedvarende indstilling |
| 54 | SolderingTipType | 0 (kun værdi på denne hardware) | 0 | **Reelt ikke en indstilling på Pinecil V2** - firmwaren er bygget med auto-tip-detektion (`AUTO_TIP_SELECTION`), så range kollapser til ét gyldigt tal. Inkluderet fordi du bad om alle undtagen BLE, men der er intet at vælge mellem - overvej at vise som deaktiveret/read-only felt frem for en fungerende kontrol |

### Diagnostik (teknisk BLE-tilgængelig, men næppe en "indstilling" en bruger vil ændre)
| # | Navn | Range (raw) | Default | Noter |
|---|---|---|---|---|
| 29 | AccelMissingWarningCounter | 0-9 | 0 | Tæller for hvor mange gange enheden har advaret om manglende accelerometer |
| 30 | PDMissingWarningCounter | 0-9 | 0 | Samme, for manglende PD-interface |

Disse to er teknisk læse-/skrivbare over BLE (bekræftet), men er interne tællere, ikke
brugerpræferencer - anbefaling er at udelade dem fra UI'et, men de er nævnt her for
fuldstændighedens skyld siden du bad om "alle undtagen BLE".

## Åbne spørgsmål til designsamtalen
1. Ember-branding eller bar-bones (som den eksisterende `/`-side)?
2. Hvordan skal enum-felter (dropdowns) og de kalibrerings-tunge felter visuelt adskilles
   fra de "almindelige" indstillinger, så man ikke ved et uheld roder med kalibrering?
3. Skal `SleepTime`s enhed afklares/testes på hardware før UI'et låser en label fast
   (sekunder vs. minutter)?
4. Skal siden vise en samlet "ændringer ikke gemt endnu"-tilstand, eller gemme/skrive
   felt-for-felt med det samme (og kun bruge SAVE-kaldet som en eksplicit "persistér til
   flash"-knap bagefter)?
5. Skal Factory Reset være med i det hele taget (se "ude af scope")?
