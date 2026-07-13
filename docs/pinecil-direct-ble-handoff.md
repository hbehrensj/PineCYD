# Handoff: Pinecil direkte-BLE eksperiment (separat spor fra søsterprojektet)

> Status: **gennemført** - alle tre faser er kørt på rigtig hardware, og Fase 0/1 er siden
> foldet ind i Fase 2, som nu er den eneste aktive firmware i repoet. Se
> `firmware/fase2-ember-design/README.md` for status og fund. Dette dokument er bevaret som
> det oprindelige handoff/planlægningsgrundlag, ikke opdateret undervejs.

## Motivation

Søsterprojektets nuværende Pinecil-kæde (CYD → ESPHome native API → HA → IronOS-integrationen →
BLE → Pinecil) har en fast ~5 sekunders opdateringslatency, fordi IronOS-integrationen poller
kolben med det interval. Det er fint til "hvilken side skal vises" (M2's context-switching),
men opleves som for langsomt til at *følge tip-temperaturen live* mens man lodder - man vil
gerne se temperaturen reagere med det samme, ikke hvert 5. sekund.

Ideen: dropper HA/WiFi helt af regnestykket for Pinecil-siden specifikt. CYD'en får sin egen
direkte BLE-forbindelse til Pinecillen (rå PlatformIO-firmware, ikke ESPHome), og læser GATT-
characteristics direkte - ingen mellemled, ingen 5-sekunders-loft.

## Vigtigt: dette er IKKE en videreudvikling af søsterprojektet - det er et separat, parallelt projekt

Søsterprojektets egen dokumentation er eksplicit om at **HA-drevet kontekstskift mellem
Pinecil/Claude/idle-sider (styret via en HA-sensor der fortæller skærmen hvilken side den
skal vise) ER projektets kerneidé**. Et direkte-BLE-only design uden WiFi kan ikke lave det
kontekstskift (der er ingen HA-forbindelse til at levere Claude-usage-data eller til at
arbitrere mellem sider) - det bliver reelt **en dedikeret, enkeltformåls Pinecil-skærm**,
ikke en udvidelse af søsterprojektet. Fint formål i sig selv, men foreslå IKKE at genbruge
søsterprojektets repo/ESPHome-kode direkte til dette - det bør leve som sit eget projekt
(egen mappe/repo), først besluttet endeligt hvis Phase 0 nedenfor lykkes.

## Den kendte risiko (læs søsterprojektets egen dokumentation først)

Søsterprojektets arkitektur-beslutning om at holde CYD'en BLE-fri er ikke vilkårlig:

> "*BLE på CYD'en* (inkl. CYD som BT-proxy): LVGL + `esp32_ble_tracker`/`bluetooth_proxy` på
> no-PSRAM CYD fejler erfaringsmæssigt (watchdog-genstart under BT-init). IronOS er desuden
> det tungest mulige proxy-workload (konstant aktiv GATT-forbindelse, 60+ characteristics)."

Dette eksperiment genindfører præcis den kombination (LVGL + aktiv BLE GATT-klient til IronOS)
på det samme no-PSRAM ESP32 (~320KB SRAM, ingen PSRAM) - blot uden ESPHome's abstraktion oveni.
Det er ikke bare et teoretisk forbehold: hele dagens session på søsterprojektet brugte
betydelig tid på skærm/render-relaterede problemer (buffer_size, LVGL-redraw,
MADCTL/color_order) selv **uden**
en BLE-forbindelse aktiv samtidig - boardet er allerede tæt på sine grænser med "bare" LVGL.

## Foreslået tilgang: fasedelt, med en billig de-risking først

**Fase 0 - BLE-only proof of concept (ingen LVGL, ingen skærm-logik overhovedet).**
Minimal PlatformIO-firmware der udelukkende: initialiserer BLE, forbinder til Pinecillen,
læser 2-3 characteristics (fx tip-temp), og logger værdierne over seriel/UART. Mål: bekræft
eller afkræft at en stabil, vedvarende GATT-forbindelse til IronOS overhovedet er mulig på
dette board uden watchdog-genstarter, **før** der investeres i LVGL/UI ovenpå. Kør den i
mindst 30-60 minutter sammenhængende for at udelukke langsomme hukommelseslæk/fragmentering.

**Fase 1 (kun hvis Fase 0 er stabil) - minimal skærm.**
Tilføj en meget simpel visning (et par labels, ingen fuld dashboard-layout endnu) og gentag
stabilitetstesten. Dette isolerer om det er *BLE alene* eller *BLE + LVGL sammen* der er
problemet, hvis noget går galt.

**Fase 2 (kun hvis Fase 1 er stabil) - portér det eksisterende design.**
Layout/farver/typografi fra søsterprojektets eget design-handoff-dokument ("Ember"-tema,
skærm P2) kan genbruges som visuel reference - det er stadig gyldigt design, uanset
firmware-arkitektur. Det er kun *implementeringen* (ESPHome/LVGL-YAML → rå LVGL i C++)
der skal skrives om, ikke selve designet.

## Tekniske anbefalinger

- **NimBLE-Arduino** frem for standard ESP32 Arduino BLE-stakken (Bluedroid-baseret) - NimBLE
  har markant lavere RAM-forbrug, hvilket er den kritiske ressource her (ingen PSRAM).
- Overvej at bruge `esp-idf` framework direkte (ligesom ESPHome gør) frem for Arduino-lag,
  hvis RAM-pres bliver et problem i Fase 0/1 - samme lektion som CLAUDE.md §4.1 allerede
  noterer for selve displayet.
- Log fri heap løbende (samme vane som søsterprojektets egen `debug:`-baserede
  heap-logging) fra dag 1 i Fase 0 - det er den mest sandsynlige fejlindikator hvis noget
  går galt.

## Åbne spørgsmål (skal afklares, ikke antaget)

1. **Pinecil/IronOS's BLE GATT service- og characteristic-UUID'er** er ikke slået op eller
   bekræftet i dette dokument - de skal findes i Ralim/IronOS's egen kildekode eller
   dokumentation (fx `BluetoothServices.hpp`/tilsvarende i IronOS-repoet), evt. suppleret med
   at inspicere hvad HA's eksisterende IronOS-integration selv bruger. Antag intet UUID uden
   at have set det i en autoritativ kilde.
2. **Tillader IronOS flere samtidige BLE-forbindelser?** Hvis kolben allerede er forbundet til
   HA's eksisterende BT-proxy, kan CYD'en så *også* forbinde direkte samtidig, eller
   udelukker de hinanden (kun én aktiv central-forbindelse ad gangen er almindeligt for BLE
   peripherals)? Afgør om dette kræver at man **erstatter** HA-forbindelsen, ikke supplerer den.
3. **Reel opdateringsrate:** er IronOS's GATT-notifications hurtigere end HA's 5s-polling i
   praksis, eller er 5s faktisk kolbens egen opdateringsrate (i så fald løser direkte BLE
   ikke noget)? Test dette tidligt - før man bygger noget som helst.
4. Skal setpoint-styring (skrive til kolben) også flyttes til denne direkte forbindelse, eller
   forblive et HA/`number.set_value`-anliggende? (Selvstændigt spørgsmål, da IronOS af
   sikkerhedsgrunde uanset hvad ikke tillader at vække/tænde kolben via BLE, jf. CLAUDE.md §5.1.)

## Success-kriterium for at fortsætte forbi Fase 0

Stabil BLE-forbindelse (ingen watchdog-genstarter, ingen mærkbar heap-nedgang) i mindst en
time sammenhængende, **og** en bekræftet opdateringsrate der rent faktisk er hurtigere end de
nuværende 5 sekunder. Hvis enten stabiliteten eller latency-gevinsten udebliver, er den
nuværende HA-baserede arkitektur sandsynligvis stadig det bedre valg, og eksperimentet bør
lukkes ned frem for at fortsætte investere i det.
