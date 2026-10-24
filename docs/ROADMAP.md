# Roadmap: port nativo di Midnight Club 3: DUB Edition Remix

Obiettivo: far girare la versione **Xbox originale** (PAL, XDK 5849) in modo nativo su **PC** e su **Xbox 360**,
senza l'emulatore di retrocompatibilità della 360 (che rompe la modalità Arcade e ha problemi di caricamento).

Il repository non contiene né conterrà mai file del gioco: ogni utente usa la propria copia del disco.

## Architettura

```
src/core   portabile (nessun header di piattaforma): lettura ISO XDVDFS, parser XBE, tabella export del kernel
src/pc     Windows x86: launcher + runtime che esegue il codice x86 del gioco direttamente
src/x360   (futuro) Xbox 360: codice ricompilato x86 -> C -> PowerPC, stesso kernel/HLE
```

Le parti costose (kernel Xbox, Direct3D 8, DirectSound/XACT, input, salvataggi) vanno scritte una volta sola
dietro interfacce astratte, in modo che PC e 360 cambino solo il backend.

## PC

Il codice del gioco è x86 a 32 bit, come il PC: non va tradotto, va solo "ospitato".

| Fase | Contenuto | Stato |
|---|---|---|
| 0 | Analisi disco/XBE, struttura progetto, build CMake | fatto |
| 1 | Launcher a 0x10000, mappatura XBE, patch `fs:`, thunk del kernel con stub che registrano la prima chiamata mancante | fatto |
| 2 | Kernel: memoria, thread, sincronizzazione, file (`D:\` letto direttamente dalla ISO), tempo | fatto per l'avvio: il gioco arriva all'inizializzazione grafica. Restano 23 funzioni mai chiamate finora (driver `Io*`, crittografia `Xc*` per salvataggi e online) |
| 3 | Direct3D 8 -> D3D11/Vulkan: individuare le funzioni D3D/XGRAPHICS (firme XbSymbolDatabase), texture swizzle, vertex shader Xbox, combiner NV2A | da fare |
| 4 | DirectSound + XACT -> XAudio2, XInput, salvataggi su disco, XOnline disattivato | da fare |
| 5 | Miglioramenti: risoluzione, widescreen, 60 fps, caricamenti | da fare |

Rischio principale: la fase 3. Il gioco scrive direttamente nel push buffer della GPU NV2A tramite la libreria D3D
linkata staticamente, quindi va intercettata a livello di API (sezione `D3D` dell'XBE) e non a livello hardware.

Punto di partenza della fase 3: la prima istruzione che tocca la GPU è a `0x41BBC7` (sezione `D3D`), una lettura
del registro NV2A `0xFD001804`, chiamata da `0x41C11F` / `0x41611B`. Le funzioni D3D vanno sostituite prima che
il gioco arrivi lì.

## Xbox 360

La 360 ha una CPU PowerPC big-endian: il codice x86 non può girare così com'è.

1. **Ricompilatore statico x86 -> C** (`tools/recomp`, futuro): disassembla tutto il codice del gioco
   (le librerie XDK no, quelle le sostituisce l'HLE), ricostruisce funzioni e jump table, emette C che opera su un
   contesto CPU emulato. La memoria del gioco resta little-endian; gli accessi usano le istruzioni byte-reversed
   del PowerPC (`lwbrx`/`stwbrx`), quindi i dati del disco non vanno convertiti.
2. **Validazione sul PC**: lo stesso C ricompilato gira anche su PC e si confronta con il loader nativo
   (tracce di chiamate al kernel e frame renderizzati). Così il ricompilatore si mette a punto senza la console.
3. **Backend 360**: grafica Xenos, audio, input e file tramite toolchain homebrew (libxenon/Free60).
   Serve una console **RGH/JTAG**: senza modifica la 360 non esegue codice non firmato.

Da verificare con un prototipo: precisione x87 (il gioco usa la FPU x87, da emulare con double), prestazioni
(Xenon 3,2 GHz in-order contro Pentium III 733 MHz, con il costo di flag e byte-swap) e la quantità di codice
raggiungibile solo tramite puntatori a funzione.

## Fatti già verificati sull'XBE

Vedi [ANALISI_XBE.md](ANALISI_XBE.md).
