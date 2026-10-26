# mc3port

Port nativo di **Midnight Club 3: DUB Edition Remix** (versione Xbox originale, PAL) per **PC** e, in futuro,
**Xbox 360**, al posto della retrocompatibilità della 360 (modalità Arcade non funzionante, problemi di caricamento).

Il repository contiene solo codice: il gioco va fornito con la propria copia del disco (`.iso` XISO o Redump).

Piano e stato delle fasi: [docs/ROADMAP.md](docs/ROADMAP.md). Dati ricavati dall'eseguibile: [docs/ANALISI_XBE.md](docs/ANALISI_XBE.md).

## Come funziona (PC)

- `mc3.exe` è un launcher minuscolo linkato all'indirizzo `0x10000`, lo stesso dell'XBE: così Windows riserva
  l'intervallo di memoria che il gioco si aspetta.
- `mc3runtime.dll` legge `default.xbe` direttamente dalla ISO, lo copia sopra il launcher, sostituisce gli accessi
  `fs:` del kernel Xbox, collega le 154 funzioni del kernel e salta all'entry point del gioco.
- Le funzioni del kernel non ancora scritte fermano il gioco e scrivono in `mc3port.log` quale funzione serve,
  da dove è stata chiamata e con quali argomenti.

## Requisiti

Visual Studio 2026 con il carico di lavoro **"Sviluppo di applicazioni desktop con C++"**, comprensivo di:

- strumenti di compilazione MSVC x64/x86
- Windows 11 SDK
- strumenti CMake C++ per Windows

Python 3 solo per gli strumenti in `tools/`.

## Compilazione

Da un "Developer PowerShell for VS 2026", nella cartella del progetto:

```
cmake --preset pc-x86
cmake --build --preset pc-x86-debug
```

## Avvio

```
build\pc-x86\Debug\mc3.exe "percorso\del\gioco.iso"
```

Senza argomenti usa la prima `.iso` trovata accanto a `mc3.exe` o nella cartella corrente.

L'hard disk dell'Xbox (salvataggi in `UDATA`, dati del titolo in `TDATA`, cache `Z:`) è la cartella `xbox_hdd`
accanto a `mc3.exe`. Il log di ogni avvio è `mc3port.log`, nella stessa cartella.

Controlli: i controller XInput (Xbox 360/One/Series) sono le porte 1-4; Bianco e Nero diventano LB e RB. Senza
controller, la porta 1 è la tastiera (solo quando la finestra del gioco è in primo piano):

| Tasto | Pulsante Xbox |
|---|---|
| Frecce | croce direzionale |
| Invio / Backspace | START / BACK |
| Spazio / Esc / Z / C | A / B / X / Y |
| W / S | grilletto destro (gas) / sinistro (freno) |
| A / D | levetta sinistra (sterzo) |
| I / J / K / L | levetta destra |
| Shift sinistro / Ctrl sinistro | Nero / Bianco |

Variabili d'ambiente:

| Variabile | Effetto |
|---|---|
| `MC3_LANGUAGE` | lingua del sistema: 1 inglese, 3 tedesco, 4 francese, 5 spagnolo, 6 italiano (predefinito) |
| `MC3_MULTICORE=1` | usa tutti i core; per default il gioco gira su un core solo, come sull'Xbox |
| `MC3_NO_DIALOG=1` | niente finestra in caso di errore fatale (per i test automatici) |
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
