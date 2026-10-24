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
| 4 | DirectSound + XACT -> XAudio2, XInput, salvataggi su disco, XOnline disattivato | audio fatto (DirectSound sostituito, XACT e Bink girano sopra, suono su XAudio2); input da fare |
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

### Audio

DirectSound è sostituito alla sua interfaccia (`dsound_hle.cpp`): i 57 punti d'ingresso usati da gioco, XACT, Bink e
XPP vengono rediretti a funzioni con lo stesso prototipo. XACT e Bink restano quelli originali e girano sopra.

- **Mixer software** (`audio_mixer.cpp`): emula le voci dell'APU leggendo i campioni direttamente dalla memoria del
  gioco (i ring buffer di Bink funzionano così come sono), con regioni di play/loop, pitch Xbox (4096 per ottava,
  0 = 48 kHz), volumi in centesimi di dB, mixbin verso lo stereo, PCM 8/16 bit e ADPCM Xbox, pacchetti degli stream
  con eventi e callback (consegnate in `DirectSoundDoWork`).
- **Uscita**: XAudio2, stereo 48 kHz a blocchi da 10 ms. Il log riporta ogni 10 s quanto audio è stato prodotto.
- **Non ancora emulati**: effetti DSP (riverbero, il file `dsstdfx.bin` viene accettato e ignorato), LFO, inviluppi,
  filtri, headroom, audio 3D. Quattro funzioni DirectSound non sono ancora identificate con certezza: accettano gli
  argomenti, riescono e lo registrano nel log.

### Input e rete provvisori

- **Input**: `XInitDevices` e `XGetDeviceEnumerationStatus` sono sostituite (`hle_patches.cpp`): nessun controller
  collegato. L'input vero arriverà sostituendo `XGetDevices`/`XInputOpen`/`XInputGetState` con XInput del PC.
- **Rete**: la scheda di rete (`0xFEF00000`) è memoria semplice e il PHY risponde "cavo scollegato"; la cifratura
  DES del traffico è un passaggio trasparente. Xbox Live e System Link non sono supportati.

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
