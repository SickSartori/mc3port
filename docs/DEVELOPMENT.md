# Sviluppo

Note tecniche per chi compila e modifica mc3port (in italiano). La guida per i giocatori è il [README](../README.md) in inglese.

Stato delle fasi e note tecniche: [TECHNICAL_NOTES.md](TECHNICAL_NOTES.md). Dati ricavati dall'eseguibile: [ANALISI_XBE.md](ANALISI_XBE.md).

## Come funziona (PC)

- `mc3.exe` è un launcher minuscolo linkato all'indirizzo `0x10000`, lo stesso dell'XBE: così Windows riserva
  l'intervallo di memoria che il gioco si aspetta.
- `mc3runtime.dll` legge `default.xbe` direttamente dalla ISO, lo copia sopra il launcher, sostituisce gli accessi
  `fs:` del kernel Xbox, collega le 154 funzioni del kernel e salta all'entry point del gioco.
- Le funzioni del kernel non ancora scritte fermano il gioco e scrivono in `mc3port.log` quale funzione serve,
  da dove è stata chiamata e con quali argomenti.

## Requisiti

Visual Studio 2022 o più recente con il carico di lavoro **"Sviluppo di applicazioni desktop con C++"**, comprensivo di:

- strumenti di compilazione MSVC x64/x86
- Windows 11 SDK
- strumenti CMake C++ per Windows

Python 3 solo per gli strumenti in `tools/`.

## Compilazione

Da una "Developer PowerShell for VS", nella cartella del progetto (il preset usa il Visual Studio installato):

```
cmake --preset pc-x86
cmake --build --preset pc-x86-debug
```

Pacchetto della release (zip e SHA256SUMS in `dist/`): `powershell -File tools/package.ps1`.

## Branch e rilasci

- `main`: solo versioni pubblicate. Ogni commit su `main` corrisponde a un tag `vx.y.z` e a una release.
- `develop`: il lavoro di tutti i giorni. I commit hanno il titolo `Phase N · Area: Title` e un corpo breve.
- Modifiche più grandi: un branch `feature/<nome>` da `develop`, unito in `develop` quando è provato.

Il workflow `release.yml` compila a ogni push su `main` e `develop` e a ogni pull request, e tiene lo zip come
artifact: una build rotta si vede subito.

Pubblicare una versione: su `develop` aggiornare `project(... VERSION x.y.z)` in `CMakeLists.txt` e aggiungere la
sezione `## x.y.z` in `CHANGELOG.md`; unire `develop` in `main` e inviare il tag `vx.y.z`. Il workflow compila e
pubblica la release con le note prese dal CHANGELOG (`tools/release_notes.ps1`).

## Struttura del repository

| Cartella | Contenuto |
|---|---|
| `src/core` | parti portabili: lettura ISO (XDVDFS), parser XBE, tabella export del kernel |
| `src/pc` | runtime Windows x86: kernel emulato, GPU NV2A su D3D11, audio, input, launcher |
| `third_party` | Dear ImGui e i font Barlow, con le loro licenze |
| `res` | risorse Win32: icona, musica del launcher |
| `installer` | script Inno Setup |
| `tools` | script di build, pacchetto, note di rilascio e analisi dell'XBE |
| `docs` | questo documento, le note tecniche, l'analisi dell'XBE, logo e screenshot |
| `.github` | workflow di build e release, modelli per le issue |

## Avvio

```
build\pc-x86\Debug\mc3.exe "percorso\del\gioco.iso"
```

Con un'ISO come argomento il launcher viene saltato (anche con `MC3_SKIP_LAUNCHER=1`). Senza argomenti si apre il launcher, che usa l'ISO ricordata in `mc3port.ini`, o la prima `.iso` accanto a `mc3.exe` o nella cartella corrente, o la fa scegliere.

I dati dell'utente stanno in `%LOCALAPPDATA%\mc3port` (`paths.cpp`): l'hard disk dell'Xbox (salvataggi in `UDATA`,
dati del titolo in `TDATA`, cache `Z:`) è la cartella `xbox_hdd`, il log di ogni avvio è `mc3port.log`, e lì
stanno anche `mc3port.ini` e `shader_cache`. Se accanto a `mc3.exe` c'è un file `portable.txt` o un `mc3port.ini`,
tutto resta invece accanto a `mc3.exe` (modalità portatile): è il caso delle build in `build\pc-x86\<config>`.

Controlli: i controller XInput (Xbox 360/One/Series) sono le porte 1-4; Bianco e Nero diventano LB e RB. Senza
controller, la porta 1 è la tastiera (solo quando la finestra del gioco è in primo piano):

| Tasto | Pulsante Xbox |
|---|---|
| Frecce | croce direzionale (mappa, idrauliche, musica) |
| Invio / Backspace | START (pausa) / BACK (navigatore) |
| Spazio / Esc / Z / C | A (freno a mano) / B (trasferimento di peso) / X (nitro) / Y (guarda indietro) |
| F | clic della levetta sinistra (abilità speciale: Zone, Agro, Roar) |
| W / S | grilletto destro (gas) / sinistro (freno) |
| A / D | levetta sinistra (sterzo) |
| I / J / K / L | levetta destra (telecamera) |
| Shift sinistro / Ctrl sinistro | Nero (telecamera) / Bianco (fari) |

I tasti della tastiera si cambiano nella pagina Controlli del launcher (voce `keyboard` di `mc3port.ini`).

Alt+Invio o F11: schermo intero (senza bordi) e ritorno alla finestra.
F10: accende e spegne gli effetti moderni, per confrontarli con l'immagine originale.

## Impostazioni

Al primo avvio viene creato `mc3port.ini` nella cartella dei dati, con una spiegazione per ogni voce: fotogrammi al
secondo, schermo panoramico 16:9 (`widescreen`), calcolo dei vertici sulla scheda video (`gpu_vertices`),
risoluzione interna (antialiasing), filtro anisotropico, effetti moderni (nitidezza, bagliore delle luci,
saturazione, contrasto, vignettatura, ombreggiatura ambientale e suo raggio). Le voci nuove mancanti in un file già
esistente valgono col loro valore predefinito.

Con `widescreen` il gioco crede di essere collegato a un televisore panoramico e usa la sua modalità 16:9: più
campo visivo ai lati, niente bande nere né immagine stirata. Con `gpu_vertices` i vertex program del gioco vengono
tradotti in shader: la prima volta si compilano in sottofondo (intanto li esegue il processore) e restano salvati
nella cartella `shader_cache`, nella cartella dei dati.

Il file si modifica con un editor di testo e vale dal
riavvio successivo. Ogni voce si può dare anche come variabile d'ambiente `MC3_<NOME>` (per esempio `MC3_BLOOM=0`),
che ha la precedenza sul file.

Variabili d'ambiente:

| Variabile | Effetto |
|---|---|
| `MC3_<VOCE>` | qualunque voce di `mc3port.ini` (per esempio `MC3_RENDER_SCALE=1`, `MC3_AMBIENT_OCCLUSION=0`) |
| `MC3_DUMP_WINDOW=1` | con `MC3_DUMP_FRAMES`, salva anche l'immagine della finestra con gli effetti (`window-NNNNN.bmp`) |
| `MC3_AO_VIEW=1` | mostra solo l'ombreggiatura ambientale, in grigio, al posto della scena (per regolarla) |
| `MC3_LANGUAGE` | lingua del gioco: 1 inglese, 3 tedesco, 4 francese, 5 spagnolo, 6 italiano (altrimenti `game_language`, poi la lingua di Windows) |
| `MC3_MULTICORE=1` | usa tutti i core; per default il gioco gira su un core solo, come sull'Xbox |
| `MC3_NO_DIALOG=1` | niente finestre di dialogo e niente launcher (per i test automatici) |
| `MC3_SKIP_LAUNCHER=1` | parte direttamente il gioco |
| `MC3_LAUNCHER_AUTOPLAY=<s>` | il launcher preme GIOCA dopo `s` secondi (test) |
| `MC3_LAUNCHER_PAGE=settings\|controls\|about` | il launcher si apre su quella pagina (catture di prova) |
| `MC3_ANY_VERSION=1` | accetta anche dischi diversi dalla versione PAL supportata |
| `MC3_DUMP_FRAMES=<n>` | salva un frame presentato ogni `n` come `frame-NNNNN.bmp` nella cartella di lavoro |
| `MC3_DEBUG_DRAWS=<n>` | descrive nel log i primi `n` draw (vertici, combiner, texture, stati); con `MC3_DEBUG_DRAWS_FF=1` solo quelli fixed-function |
| `MC3_DUMP_TARGET=<indirizzo hex>` | con ogni frame salvato, anche la render target a quell'indirizzo fisico (`target-NNNNN.bmp`) |
| `MC3_DEBUG_FRAME_AT=<s>[+<durata>]` | una riga per draw (e per clear) durante 1 s, o `durata` secondi, a partire da `s` secondi dopo il primo draw (rettangolo a schermo, coordinate di texture, stati) |
| `MC3_DEBUG_PROBE=<x>,<y>` | con il precedente, registra il pixel (x, y) ogni volta che un draw lo cambia |
| `MC3_DUMP_TEXTURE=<indirizzo hex>` | salva le texture decodificate di quella pagina di 4 KB come `texture-<indirizzo>.bmp` (con l'alfa) |
| `MC3_GPU_STATS=1` | ogni 10 s, i metodi GPU più usati |
| `MC3_INPUT_SCRIPT=<s>:<pulsante>,<da>-<a>:<pulsante>,...` | preme (0,2 s) o tiene premuti pulsanti del controller 1 a quei secondi dall'avvio, per i test (`start back a b x y black white up down left right lt rt`, `sl`/`sr` per sterzare). Con `@<file>[#n]+<s>[~<durata>]:<pulsante>` il tempo conta da quando il gioco apre un file con quel nome (per esempio `@bg_main+5:down`). Con il prefisso `p2`...`p4` (`p2start`, `p2rt`) i pulsanti vanno ai controller 2-4, che compaiono collegati anche senza un controller vero; con un controller vero sulla porta 1 le pressioni si sommano alle sue |
| `MC3_DUMP_BUFFER=<byte>` | salva i dati grezzi dei buffer audio di quella dimensione in `buffer-<byte>-<indirizzo>.bin` (cartella di lavoro) |
| `MC3_TRACE_WAITS=1` | registra le attese del kernel che durano più di 5 s, con gli ultimi eventi segnalati |
| `MC3_AUDIO_DUMP=<s>` | i primi `s` secondi dell'audio in uscita in `audio.wav` (cartella di lavoro) |
| `MC3_AUDIO_VOICES=1` | ogni 10 s, l'elenco delle voci audio attive con formato, volume, tono, regioni, età e uscite |
| `MC3_SKIP=<texture>:<superficie>:<fusione>` / `MC3_DUMP_DRAW=...` | salta, o salva prima e dopo, i draw corrispondenti (per isolare un difetto grafico) |
| `MC3_WATCHDOG=<s>` | ogni `s` secondi, dove si trovano i thread del gioco (cambia la temporizzazione) |

## Strumenti

- `python -I tools/xiso_inspect.py <iso> [default.xbe di output]`: elenca i file del disco, analizza l'XBE ed eventualmente lo estrae.
- `python -I tools/check_xbe.py <default.xbe>`: verifica la tabella del kernel e la scansione `fs:` del loader sull'XBE reale.
- `python -I tools/xbe_to_pe.py <default.xbe> <out.exe>`: impacchetta l'XBE come PE per disassemblarlo agli indirizzi reali, per esempio con `dumpbin /disasm /range:0x13600,0x13700 out.exe`.
