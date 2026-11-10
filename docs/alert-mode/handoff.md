# Handoff: Alarm-tilstand ("rød skærm") på PineCYD

Skrevet 10. nov. 2026. **Implementeret og kompileret** 10. nov. 2026, endnu ikke afprøvet på hardware. Se "Alert mode" i `firmware/fase2-ember-design/README.md`.

> Repoet er **offentligt**. Firmware og docs skal være generiske: ingen IP-adresser, værtsnavne, enhedsnavne eller andre detaljer fra et bestemt hjem. Alt hjemmespecifikt hører til i indstillingerne og i den server, der leverer alarmerne.

## Formål

PineCYD står et sted, hvor ejeren ser den jævnligt. Den skal fortsætte med at vise det, den gør i dag:
- Pinecil-dashboard, når loddekolben er forbundet.
- Ellers ur med Claude-forbrugszonen.

**Kun når der er noget, ejeren skal forholde sig til**, bliver skærmen **rød** med en kort tekst. Alarmerne kommer fra en ekstern overvågningsserver på LAN'et. Det er ejerens egen, men den er ikke en del af dette repo.

Det følger repoets princip "tre måder at bruge den på": funktionen er valgfri og helt usynlig, hvis den ikke er sat op.

## Indstilling

- Nyt felt i indstillingsformularen (`http://pinecyd.local/`): **Alarm-URL**, fx `http://<server>/api/alert`.
- **Tomt felt** betyder, at funktionen er slået fra. Så er der ingen polling, ingen fejlvisning og ingen ændring i forhold til i dag.
## Kontrakt (server → PineCYD)

```
GET  <Alarm-URL>
  200 {"alert": false}
  200 {"alert": true, "id": "a-123", "level": "crit",
       "text": "Backup er 2 døgn gammel", "since": "2026-11-10T19:02:00Z"}
```
- **PineCYD viser kun, den kvitterer ikke.** Der er ingen touch og ingen knap (besluttet 10. nov. 2026). Projektet har ingen touch-driver, og flash-pladsen er knap. Kvittering og skjul sker på serveren: når ejeren kvitterer dér, svarer serveren `alert: false` for den alarm. Firmwaren skal hverken have en ack-URL eller en lokal skjul-timer.
- `text` er højst 40 tegn, UTF-8, og **kan indeholde æøå**. De indbyggede Montserrat-fonte har dem ikke. Lav derfor én ny font i én størrelse med ASCII plus æøåÆØÅ (kun de nødvendige glyffer, så flash-forbruget holdes nede). Mål flash-forbruget undervejs: firmwaren fylder ca. 91 % af OTA-slottet (10. nov. 2026).
- `level` er i første omgang altid `crit`. Andre værdier ignoreres og behandles som `alert: false`.
- Plain HTTP på LAN uden auth. Svaret indeholder intet følsomt, og PineCYD gemmer ingen tokens. Det er samme princip som usage-bridgen.

## Adfærd

| Situation | Visning |
|---|---|
| Alarm-URL tom | Som i dag |
| `alert: false` | Som i dag |
| `alert: true`, Pinecil **ikke** forbundet | **Hele skærmen rød**: advarselsikon, `text`, "siden HH:MM" (lokal tid fra `since`) og uret i lille størrelse |
| `alert: true`, Pinecil **forbundet** | Kun en **rød bjælke** i toppen med `text`, så lodde-data stadig kan ses. En fuld rød skærm midt i en lodning er netop det, man ikke vil have (bekræftet). |
| Alarm-URL sat, og serveren har svaret **mindst én gang siden opstart**, men svarer ikke i 10 minutter (timeout, HTTP-fejl eller ugyldig JSON) | Rød skærm med teksten "Statusserver svarer ikke". Det er en dødmandsknap: hvis serveren er nede, er der ingen andre til at sige det. Forsvinder af sig selv, når serveren svarer igen. Har serveren aldrig svaret siden opstart, vises intet. Ellers ville skærmen lyse rødt efter hver strømafbrydelse, hvor PineCYD starter før serveren. |

- Poll-interval: 30 sekunder. Alarmen er ikke tidskritisk på sekunder.
- Hold dig til grafikreglerne fra `docs/claude-usage-zone/handoff.md`: 320×240, RGB565, ingen blur, skygge eller glød, og **ingen blink**. Rød er statisk.
- Er der en RGB-LED på kortet, må den gerne lyse **konstant** rødt under alarm, ikke blinke.
- Brug den eksisterende ember-palet. Den røde baggrund skal give god kontrast til teksten (WCAG AA).

## Test uden server

Lav et lille mock-script under `tools/` (fx Python `http.server`), der kan skifte mellem tilstandene. Afprøv mindst:
1. tom URL
2. `alert: false`
3. alarm uden Pinecil
4. alarm med Pinecil forbundet
5. alarm, der forsvinder igen (`alert: false`), fordi den er kvitteret på serveren
6. server nede i over 10 minutter efter at have svaret
7. server nede fra opstart (skal **ikke** give rød skærm)
8. ugyldig JSON
9. tekst med æøå og 40 tegn

## Beslutninger (10. nov. 2026)

- Ingen touch og ingen knap. Kvittering sker på serveren.
- Rød bjælke under lodning.
- "Statusserver svarer ikke" vises efter 10 minutter, men kun hvis serveren har svaret siden opstart.
