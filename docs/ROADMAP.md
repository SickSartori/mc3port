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
| 3 | Grafica: emulazione della GPU NV2A (registri, push buffer, interrupt) con il Direct3D originale del gioco; poi rendering su D3D11/Vulkan | in corso: `CreateDevice` riesce, il push buffer scorre, mancano disegno e presentazione |
| 4 | DirectSound + XACT -> XAudio2, XInput, salvataggi su disco, XOnline disattivato | da fare |
| 5 | Miglioramenti: risoluzione, widescreen, 60 fps, caricamenti | da fare |

### Grafica: perché si emula la GPU e non Direct3D

La libreria D3D dell'Xbox scrive gran parte dei comandi direttamente nel push buffer, anche con codice inline nel
gioco: sostituire le funzioni D3D lascerebbe fuori quel codice. Si emula invece la GPU (come xemu): il D3D originale
gira invariato e `src/pc/nv2a.cpp` fa la parte dell'hardware.

- **Memoria fisica**: 64 MB a `0x80000000` come sull'Xbox (`physical_memory.cpp`); gli indirizzi GPU tornano.
- **Registri** a `0xFD000000`: non mappati; ogni accesso genera un'eccezione che `mmio.cpp` decodifica (mov, movzx,
  test, cmp, or, and) e ripete sul modello dei registri (interrupt write-1-to-clear, timer, FIFO sempre inattiva,
  PUT che sveglia il thread GPU). La RAM d'istanza (`0xFD700000`) è memoria normale.
- **Push buffer**: salti, chiamate, metodi; oggetti risolti tramite RAMHT; semafori e reference per i fence.
- **Vblank** a 60 Hz verso l'ISR del driver; finestra Win32 che mostrerà il front buffer (`PCRTC_START`).

Prossimi passi della fase 3: tradurre i metodi Kelvin (stati, vertex/pixel shader, texture swizzled, draw) in
chiamate D3D11 e presentare i frame. Fino ad allora la finestra resta nera.

### Input e audio provvisori

- **Input**: `XInitDevices` e `XGetDeviceEnumerationStatus` sono sostituite (`hle_patches.cpp`): nessun controller
  collegato. L'input vero arriverà sostituendo `XGetDevices`/`XInputOpen`/`XInputGetState` con XInput del PC.
- **Audio**: l'APU (`0xFE800000`) e l'AC97 (`0xFEC00000`) sono memoria semplice con due valori preimpostati, quanto
  basta per l'inizializzazione di DirectSound. Il motore XACT invece crasha (`0x408FFC`): è il prossimo blocco, e va
  risolto sostituendo DirectSound/XACT con XAudio2 (fase 4).

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
