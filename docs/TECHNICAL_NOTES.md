# Note tecniche: port nativo di Midnight Club 3: DUB Edition Remix

Obiettivo: far girare la versione **Xbox originale** (PAL, XDK 5849) in modo nativo su **PC**.

Il repository non contiene né conterrà mai file del gioco: ogni utente usa la propria copia del disco.

## Architettura

```
src/core   portabile (nessun header di piattaforma): lettura ISO XDVDFS, parser XBE, tabella export del kernel
src/pc     Windows x86: launcher + runtime che esegue il codice x86 del gioco direttamente
```

Le parti costose (kernel Xbox, Direct3D 8, DirectSound/XACT, input, salvataggi) sono scritte una volta sola, dietro
interfacce astratte, con il backend Windows separato.

## PC

Il codice del gioco è x86 a 32 bit, come il PC: non va tradotto, va solo "ospitato".

| Fase | Contenuto | Stato |
|---|---|---|
| 0 | Analisi disco/XBE, struttura progetto, build CMake | fatto |
| 1 | Launcher a 0x10000, mappatura XBE, patch `fs:`, thunk del kernel con stub che registrano la prima chiamata mancante | fatto |
| 2 | Kernel: memoria, thread, sincronizzazione, file (`D:\` letto direttamente dalla ISO), tempo | fatto |
| 3 | Grafica: emulazione della GPU NV2A (registri, push buffer, interrupt) con il Direct3D originale del gioco, rendering su D3D11 | fatto |
| 4 | DirectSound + XACT -> XAudio2, XInput, salvataggi su disco, XOnline disattivato | fatto (DirectSound sostituito, XACT e Bink girano sopra, suono su XAudio2; XInput, vibrazione, tastiera; salvataggi su `xbox_hdd`) |
| 5 | Miglioramenti e distribuzione: risoluzione interna, 16:9, 60 fps, vertici sulla GPU, effetti, launcher, installer | fatto (versione 0.1.1) |
| 6 | Menu in gioco (F1, Back+Start) con pausa del gioco, comandi da console nel launcher, velocità reale a 60 fps (passo di tempo dai fotogrammi mostrati), motion blur corretto e regolabile | fatto (versione 0.2.0) |

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
- **DISPATCH_LEVEL**: sull'Xbox nessun altro thread gira mentre uno è a DISPATCH_LEVEL o sopra (ISR, DPC, codice
  che alza l'IRQL); il port lo emula con un lock, ma un thread del gioco a PASSIVE_LEVEL poteva girare lo stesso
  quando il proprietario del lock si fermava su un lock dell'host (l'ISR in `KeInsertQueueDpc` dietro un thread
  normale interrotto con il lock del dispatcher in mano). Ora un thread del gioco a DISPATCH_LEVEL, o con in mano il
  lock del dispatcher o delle voci audio (`GameLock`), gira alla priorità del thread degli interrupt, e i thread
  dell'host (timer, orologio, mixer) stanno fuori dal core del gioco. Il profilo conta i casi rimasti ("runs during
  another thread's DISPATCH_LEVEL"): da 8-18 ogni 10 s a 0-1 nella carriera; uno di quelli in XACT (`0x40F013`) è la
  causa probabile del crash casuale in `0x0040652C` (lista dei suoni con un elemento vuoto).
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
| Arcade | funziona: guida libera a San Diego e Tokyo |
| Arcade a 2 giocatori | funziona a schermo diviso (32 fps, la città è disegnata due volte); il riquadro nero dietro ogni auto è lo specchietto retrovisore, che come nell'originale mostra solo i fari delle auto dietro |
| Rete | corretto senza connessione: "Il cavo di rete non è connesso" |
| Sfida di Tokyo | funziona (provata anche a mano, con il fiume percorribile) |
| Editor di gare | funziona: si entra, la telecamera si muove, il menu dell'editor si apre (provato anche a mano) |
| Profili | creazione, scelta e caricamento all'avvio funzionano |

Con l'orologio a 733 MHz (26 ottobre) la navigazione automatica dei menu è affidabile: un tocco conta una volta.
Il profilo di prova "Giocatore 2" è a Tokyo; Sfida di Tokyo è la seconda voce del menu principale, Arcade la
terza, Editor di gare la quinta (con conferma "Sì").

- **Coordinate e clip**: i vertici arrivano in pixel della superficie intera e il renderer li converte con la
  dimensione vera della render target (larghezza dal pitch); il clip della superficie è uno scissor. Il radar
  restringe il clip a 256 pixel: prima la sua mappa finiva ingrandita al centro.
- **Stencil** (`0x32C`, `0x360`-`0x378`) e **pulizie per canale** (`CLEAR_SURFACE` 0x10/0x20/0x40/0x80): l'HUD pulisce
  solo l'alfa e lo usa come maschera (`DST_ALPHA`) per il cerchio del radar.
- La mappa grande e trasparente al centro è quella del gioco: si apre e si chiude con la croce direzionale; BACK
  apre il "Sistema di navigazione".

- **Culling** (`0x308`, `0x39C`, `0x3A0`): il verso delle facce si conserva fino a D3D11 (le coordinate restano in
  pixel con y verso il basso), quindi CW/CCW passano così come sono.
- **Fusione "con segno"** (`F005` REVSUBTRACT_SIGNED, `F006` ADD_SIGNED): il gioco conta le facce dei volumi di
  luce dei fari nell'alfa (prima sottrae le facce anteriori, poi somma quelle posteriori). Su UNORM la sottrazione
  si fermava a zero e restava un'ellisse bianca piena; la superficie che li usa diventa R16G16B16A16_FLOAT.
- **Velocità**: vertici in un buffer ad anello e conversione pixel->clip nel vertex shader (menu 42-60 fps, città
  47-52 fps).

### Audio

DirectSound è sostituito alla sua interfaccia (`dsound_hle.cpp`): i 57 punti d'ingresso usati da gioco, XACT, Bink e
XPP vengono rediretti a funzioni con lo stesso prototipo. XACT e Bink restano quelli originali e girano sopra.

- **Cursori**: il cursore di scrittura sta 32 campioni (un frame dell'APU) davanti a quello di riproduzione. Con
  10 ms di anticipo Bink, all'avvio dei video, riempiva di silenzio in un ciclo che non finiva più.
- **Funzioni corrette** (ottobre 2026): `0x3C28F7`/`0x3C2938` sono `SetMixBins` (Bink passa un `DSMIXBINS`), non
  `SetFormat`; il vero `SetFormat` è `0x3C2F5C`/`0x3C2F98` (interna `0x2AD9`, XACT imposta lì i formati mono). Con
  i formati sbagliati video ed effetti suonavano come rumore.
- **Mixbin** (numerazione dell'header Xbox): 0-5 altoparlanti (FL, FR, C, LFE, BL, BR), 6-9 XTLK (crosstalk:
  FL, FR, BL, BR), 10 mandata al riverbero I3DL2, 11-30 mandate agli effetti, 31 submix. XACT instrada ogni voce
  nuova a XTLK + centro + LFE + I3DL2 e poi la reinstrada a 0-5; in città nessuna voce resta su 6 e oltre.
- **Uscita**: i mixbin ridotti a stereo (anteriori 1, centrale e posteriori 0,5, XTLK come i loro altoparlanti;
  LFE, riverbero e mandate esclusi), volume generale 0,3 e limitatore con 2 ms di anticipo e 150 ms di rilascio;
  interpolazione lineare nel ricampionamento e rampa del volume in ogni blocco da 10 ms (niente click quando il
  gioco cambia volume e tono del motore). Una voce senza mixbin prende quelli del suo primo `SetMixBinVolumes`.
  Il gioco mette a 0 l'headroom di tutti i mixbin e delle voci, e non chiede mai volumi sopra 0 dB.
- **Come XACT suona un effetto**: lo avvia in loop e 10 ms dopo chiama `StopEx(ENVELOPE | RELEASEWAVEFORM)`: la
  voce esce dal loop, suona fino alla fine del campione e sfuma con il rilascio dell'inviluppo. L'inviluppo lo
  imposta prima con `SetEG`: `dwEG` 1 (`DSEG_AMPLITUDE`; 0 è il "multi" di tono e filtro), modo 1, attacco 0-23 e
  rilascio 32 blocchi da 512 campioni (0,34 s), sustain 255. Fino alla 0.3.0 il port leggeva l'inviluppo dal
  generatore 0: nessun effetto aveva il suo rilascio, tutti finivano 15 ms dopo l'avvio (motore, vento, gomme,
  urti). Una voce con l'inviluppo spento (modo 0) non sfuma: esce dal loop e suona fino in fondo.
- **Stop e stream** (ottobre 2026): stato degli stream con `DSSTREAMSTATUS_PLAYING`, `Discontinuity` e flush
  asincrono; `FlushEx` con ENVELOPE sfuma lo stream. Prima la musica del menu continuava sotto quella della città.
  Dettagli in [ANALISI_XBE.md](ANALISI_XBE.md).
- **Diagnostica**: la riga `audio: last 10 s` del log riporta audio prodotto, picco, campioni abbassati dal
  limitatore, voci, pacchetti, buchi di XAudio2 e attesa massima sul lock del mixer (in città: zero buchi, al
  massimo 0,2 ms). `MC3_AUDIO_DUMP`, `MC3_AUDIO_VOICES` e `MC3_AUDIO_TRACE` con gli script `tools/audio_*.py`
  (vedi [DEVELOPMENT.md](DEVELOPMENT.md)). Misurato così: ADPCM decodificato come vgmstream (64 campioni per
  blocco, ultimo nibble scartato), spettro pulito (la musica cala di 41 dB sopra i 20 kHz), nessun taglio brusco.
- **Audio 3D**: non serve, MC3 non lo usa (vedi ANALISI_XBE.md). Alcuni suoni del motore
  sono suonati da due coppie di voci identiche (stesso campione, stessa posizione), probabilmente per scelta di XACT.
- **Mixer software** (`audio_mixer.cpp`): emula le voci dell'APU leggendo i campioni direttamente dalla memoria del
  gioco (i ring buffer di Bink funzionano così come sono), con regioni di play/loop, pitch Xbox (4096 per ottava,
  0 = 48 kHz), volumi in centesimi di dB, mixbin verso lo stereo, PCM 8/16 bit e ADPCM Xbox, pacchetti degli stream
  con eventi e callback (consegnate in `DirectSoundDoWork`).
- **Uscita**: XAudio2, stereo 48 kHz a blocchi da 10 ms. Il log riporta ogni 10 s quanto audio è stato prodotto.
- **Emulato anche**: il filtro DLS2 passa-basso per voce (`SetFilter`, un filtro a variabili di stato a 48 kHz;
  XACT lo usa su pochi suoni, come l'acqua del fiume a Tokyo).
- **Non emulati**: effetti DSP (riverbero, il file `dsstdfx.bin` viene accettato e ignorato), LFO, inviluppo
  multiplo (tono e filtro), equalizzatore parametrico, headroom (il gioco lo mette a 0), audio 3D (non usato da MC3).
  In città il gioco non chiama LFO né inviluppo multiplo. Quattro funzioni DirectSound non sono identificate con
  certezza: accettano gli argomenti, riescono e lo registrano nel log.

### Input e rete

- **Input**: `XInitDevices` non avvia lo stack USB (`hle_patches.cpp`); le sette funzioni XPP che il gioco usa per i
  controller sono redirette a XInput del PC (`input_hle.cpp`), con vibrazione, collegamento a caldo e tastiera come
  controller 1. Memory unit e cuffie Xbox Live restano assenti.
- **Orologio a 733 MHz**: il gioco converte `rdtsc` in tempo supponendo la CPU dell'Xbox; sul PC il contatore va a
  qualche GHz e il tempo del gioco scorreva 5-6 volte più veloce (ripetizione dei tasti nei menu velocissima, il
  controller sembrava "troppo sensibile"). I quattro punti che leggono `rdtsc` per misurare il tempo sono rediretti
  a un contatore a 733 MHz ricavato da `QueryPerformanceCounter` (`hle_patches.cpp`).
- **Rete**: la scheda di rete (`0xFEF00000`) è memoria semplice e il PHY risponde "cavo scollegato"; la cifratura
  DES del traffico è un passaggio trasparente. Xbox Live e System Link non sono supportati.

### Wine (Linux)

Sperimentale. Provato con Wine 11.0 (WineHQ) in WSL2 su Ubuntu 24.04, dove Mesa a 32 bit non ha il driver `d3d12` e
wined3d disegna con `llvmpipe` sul processore: funzionano il launcher, il controllo del disco, il kernel, i thread, il
push buffer, l'audio XAudio2, i video iniziali e i menu fino alla carriera. Non provati: le prestazioni con un driver
grafico vero, Proton e DXVK, i controller (WSL non vede i pad), lo Steam Deck, l'installer e l'aggiornamento.

Differenze da Wine trovate e gestite:
- Senza le librerie Linux a 32 bit di OpenGL ed EGL (`libgl1:i386`, `libegl1:i386`) Wine non ha Direct3D 11:
  `host: no DXGI factory` nel log, il launcher passa alle finestre di dialogo e la finestra del gioco resta nera.
- Il compilatore HLSL di Wine (vkd3d-shader) non conosce `isnan`: gli shader dei vertici usano `is_nan`, `is_inf` e
  `is_finite`, che leggono i bit del numero (`kVertexDataShader`).
- `ExitProcess` a gioco avviato lasciava il processo appeso: Wine ferma gli altri thread in modo asincrono mentre
  esegue il distacco delle DLL, e un thread del gioco restava a girare sulle eccezioni. Tutte le uscite passano da
  `end_process` (`TerminateProcess`).

Provato prima, il 30 ottobre 2026, con Wine 9.0 (pacchetto 0.2.2, senza disco): il launcher parte e si
disegna. Verificato con un programma di prova a 32 bit: la memoria fisica a `0x80000000` con write watch, i registri
NV2A a `0xFD000000` intercettati come access violation e le istruzioni `in`/`out`, `cli`/`sti`, `hlt` si comportano
come su Windows. Due differenze gestite dal port: `wbinvd`, `invd`, `rdmsr` e `wrmsr` arrivano come access violation
all'indirizzo `0xFFFFFFFF` (il gestore delle eccezioni le passa all'emulatore delle istruzioni privilegiate); senza un
dispositivo audio l'XAudio2 di Wine va in crash dentro `CreateMasteringVoice` invece di restituire un errore, perciò
il port controlla prima con `IMMDeviceEnumerator` che esista un'uscita. Il log riporta la versione di Wine e del
sistema ospite (`host: Wine ...`).

## Fatti già verificati sull'XBE

Vedi [ANALISI_XBE.md](ANALISI_XBE.md).
