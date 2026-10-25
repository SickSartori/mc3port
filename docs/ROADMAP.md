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
| 3 | Grafica: emulazione della GPU NV2A (registri, push buffer, interrupt) con il Direct3D originale del gioco; poi rendering su D3D11/Vulkan | in corso: video, menu (titolo, profilo, carriera) e città 3D illuminata visibili su D3D11; mancano texgen sferici/cube map, bump mapping, punti e linee, luci a due facce, culling; lento nei menu (build Debug) |
| 4 | DirectSound + XACT -> XAudio2, XInput, salvataggi su disco, XOnline disattivato | audio fatto (DirectSound sostituito, XACT e Bink girano sopra, suono su XAudio2, musica WMA in gara); input fatto (XInput, vibrazione, tastiera); salvataggio del profilo su `xbox_hdd`; da provare con un controller vero |
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
- **Vblank** a 60 Hz verso l'ISR del driver; la finestra Win32 mostra il front buffer (`PCRTC_START`).
- **Rendering** (`nv2a_kelvin.cpp` -> `renderer_d3d11.cpp`): i vertici passano dal vertex program NV2A interpretato
  sulla CPU (`nv2a_vsh.cpp`) oppure dalla pipeline fixed-function (matrice composita, offset del viewport, texgen
  lineari, matrici delle texture, 8 luci infinite/locali/spot con color material, speculare separato o sommato);
  il fattore di nebbia si calcola per vertice da modo e parametri (`0x9C0`) per entrambi i percorsi; i register combiner diventano un pixel shader HLSL generato
  (`nv2a_combiner.cpp`); le texture swizzled, lineari e DXT sono decodificate da `nv2a_texture.cpp`. Le texture
  lineari usano coordinate in texel (come le rectangle texture) e vengono normalizzate nello shader. Ogni superficie
  di rendering è una render target D3D11; il blit 2D (`0x9F`/`0x62`) copia fra render target.
- **Marcatore di avanzamento**: D3D mette nel push buffer `SET_MONOCHROME_COLOR0` dell'oggetto pattern (`0x44`), che
  scrive `PGRAPH 0x400B10`, e lo legge per sapere fin dove è arrivata la GPU. Senza, il D3D del gioco si blocca a
  caso (su un contatore a 5 bit), anche durante i video.
- **Rilevatore di blocchi**: se il push buffer resta fermo 10 s, il log riporta dove sono tutti i thread del gioco
  (indirizzi del gioco e funzioni del port, con i simboli del PDB) e gli ultimi metodi software della GPU.
- **Core**: il codice del gioco (thread, ISR, DPC) gira su un core solo come sull'Xbox; la GPU emulata gira sugli
  altri core, come un processore a parte. Per questo vanno emulati i punti in cui il driver ferma la GPU:
  `PGRAPH_FIFO` (`0x400720`) a 0 mentre il gestore d'interrupt legge `TRAPPED_DATA`, e i DPC eseguiti subito dopo
  l'ISR. Senza, la GPU sovrascriveva il metodo software `NOP(5)` e il gioco aspettava per sempre.
- **Velocità**: con la build Release i menu 3D vanno a 28-50 fps (in Debug 2 fps). Le texture si rileggono solo se
  la loro memoria è stata scritta (write watch di Windows sulla RAM emulata); le render target non si ricreano più
  quando cambia il clip (le superfici swizzled hanno la dimensione del formato). Il log riporta ogni 10 s fps, draw
  e tempo per vertici/texture/D3D.

- **Menu**: l'interfaccia (file `flash\*.xbck`) è disegnata in una render target 512x512 swizzled e poi incollata in
  prospettiva sulla scena 3D con blend additivo e la nebbia nel final combiner: senza fattore di nebbia corretto
  restava invisibile.

- **Costanti condivise**: matrice composita, posizione dell'occhio, scala e spostamento del viewport stanno nella
  stessa memoria delle costanti dei vertex program (`NV_IGRAPH_XF_XFCTX_*`, slot `0x00`, `0x38`, `0x3A`, `0x3B`).
  I vertex shader di D3D finiscono con la trasformazione in coordinate dello schermo che usa `c[-38]`/`c[-37]`:
  senza, le auto (disegnate con vertex program) collassavano in un punto ed erano invisibili.

Stato: si arriva alla prima gara e si guida (35 fps in città nella build Release).

Modalità provate (con lo script di input, 25 ottobre 2026):

| Modalità | Stato |
|---|---|
| Carriera | funziona: profilo, video, garage, acquisto dell'auto, prima gara; i progressi (auto comprata) si salvano e si ricaricano |
| Arcade | funziona: guida libera a San Diego (su Xbox 360 in retrocompatibilità non va) |
| Rete | corretto senza connessione: "Il cavo di rete non è connesso" |
| Sfida di Tokyo, Editor di gare | da provare (la navigazione automatica dei menu è instabile) |
| Profili | creazione, scelta e caricamento all'avvio funzionano |

Da verificare a mano: fine di una gara e salvataggio della carriera dopo una vittoria; velocità nei menu (scesa a
circa 24 fps dopo le correzioni del radar, da controllare).

- **Coordinate e clip**: i vertici arrivano in pixel della superficie intera e il renderer li converte con la
  dimensione vera della render target (larghezza dal pitch); il clip della superficie è uno scissor. Il radar
  restringe il clip a 256 pixel: prima la sua mappa finiva ingrandita al centro.
- **Stencil** (`0x32C`, `0x360`-`0x378`) e **pulizie per canale** (`CLEAR_SURFACE` 0x10/0x20/0x40/0x80): l'HUD pulisce
  solo l'alfa e lo usa come maschera (`DST_ALPHA`) per il cerchio del radar.
- La mappa grande e trasparente al centro è quella del gioco: si apre e si chiude con la croce direzionale; BACK
  apre il "Sistema di navigazione".

Da sistemare nelle migliorie grafiche (non bloccano il gioco): ellisse bianca sotto l'auto in gara (da indagare: il
buffer del bagliore `02725E80`/`0277FF00` sommato allo schermo e la luce dei fari, texture DXT5 `02852400`), bump
mapping (modalità texture `6`), cube map, punti e linee, culling; vertici ancora calcolati sulla CPU. Dopo il garage il gioco va in crash nel decoder della musica (WMA/XACT, `0x15979`), e un
thread aspetta su un handle di file, cosa che il kernel emulato non supporta ancora.

### Audio

DirectSound è sostituito alla sua interfaccia (`dsound_hle.cpp`): i 57 punti d'ingresso usati da gioco, XACT, Bink e
XPP vengono rediretti a funzioni con lo stesso prototipo. XACT e Bink restano quelli originali e girano sopra.

- **Cursori**: il cursore di scrittura sta 32 campioni (un frame dell'APU) davanti a quello di riproduzione. Con
  10 ms di anticipo Bink, all'avvio dei video, riempiva di silenzio in un ciclo che non finiva più.
- **Mixer software** (`audio_mixer.cpp`): emula le voci dell'APU leggendo i campioni direttamente dalla memoria del
  gioco (i ring buffer di Bink funzionano così come sono), con regioni di play/loop, pitch Xbox (4096 per ottava,
  0 = 48 kHz), volumi in centesimi di dB, mixbin verso lo stereo, PCM 8/16 bit e ADPCM Xbox, pacchetti degli stream
  con eventi e callback (consegnate in `DirectSoundDoWork`).
- **Uscita**: XAudio2, stereo 48 kHz a blocchi da 10 ms. Il log riporta ogni 10 s quanto audio è stato prodotto.
- **Non ancora emulati**: effetti DSP (riverbero, il file `dsstdfx.bin` viene accettato e ignorato), LFO, inviluppi,
  filtri, headroom, audio 3D. Quattro funzioni DirectSound non sono ancora identificate con certezza: accettano gli
  argomenti, riescono e lo registrano nel log.

### Input e rete provvisori

- **Input**: `XInitDevices` non avvia lo stack USB (`hle_patches.cpp`); le sette funzioni XPP che il gioco usa per i
  controller sono redirette a XInput del PC (`input_hle.cpp`), con vibrazione, collegamento a caldo e tastiera come
  controller 1. Memory unit e cuffie Xbox Live restano assenti.
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
