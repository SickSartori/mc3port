# Analisi di default.xbe (PAL, Redump CRC e0cad5e2)

Ricavata con `tools/xiso_inspect.py` direttamente dalla ISO.

## Intestazione

| Campo | Valore |
|---|---|
| Titolo | Midnight Club 3: DUB edition |
| Title ID | `0x54540079` |
| Regione | `0x4` (PAL), retail |
| Base / dimensione immagine | `0x10000` / `0x57A280` |
| Entry point | `0x1299F` |
| Thunk del kernel | `0x4476E0` (154 import) |
| Stack commit | `0x20000` |
| PDB | `C:\soft\mc35\src\mcproduct\xbox_final\mc.exe` |
| XDK | 5849 per tutte le librerie: XAPILIB, XBOXKRNL, LIBCPMT, LIBCMT, D3DX8, XGRAPHC, DSOUND, XONLINES, XACTENG, XVOICE, VOICMAIL, D3D8 |

Sezioni di codice di libreria riconoscibili: `D3D`, `D3DX`, `XGRPH`, `DSOUND`, `XACTENG`, `XNET`, `XONLINE`,
`WMADEC`, `XPP`, `BINK*`, `DOLBY`. Il codice del gioco vero e proprio è in `.text` (`0x12000`, ~3,4 MB).

## Accessi a `fs:` (KPCR Xbox contro TEB Windows)

| Offset | Significato su Xbox | Siti | Gestione |
|---|---|---|---|
| `0x00` | NT_TIB.ExceptionList (SEH) | ~390 | uguale su Windows, nessuna patch |
| `0x04` | NT_TIB.StackBase = area TLS | 6 | patch -> KPCR emulato |
| `0x20` | KPCR.Prcb | 14 | patch |
| `0x24` | KPCR.Irql (`movzx` byte) | 12 | patch |
| `0x28` | KPRCB.CurrentThread | 9 | patch |
| `0x58` | KPRCB.DpcRoutineActive | 6 | patch |

## TLS

- Directory TLS: nessun dato iniziale, `0x90` byte azzerati, indice a `0x4CB678`.
- Il gioco legge `[fs:[0x04] + indice*4]`: su Xbox lo StackBase punta all'area TLS, il cui primo DWORD
  punta ai dati.
- L'avvio dei thread XAPI usa `KTHREAD+0x28` (TlsData) e scrive `[TlsData] = TlsData + 4`.
- Vengono letti campi di KTHREAD almeno fino a `+0x12C`.

## Contenuto del disco (4,6 GB)

| Cosa | Dimensione |
|---|---|
| `video/*.bik` (106 filmati Bink) | 1,6 GB |
| `assets_x.dat*` (6 archivi) | 1,2 GB |
| `resources/` (città e veicoli, `.xbpf`) | 641 MB |
| `flash/*.xbck` (menu Flash) | 641 MB |
| `audio/` (banchi XACT `.xwb`) | 486 MB |

## Avvio verificato sul PC (fase 2)

Fatti emersi facendo girare il gioco, che il kernel emulato deve rispettare:

- **TLS sotto lo StackBase.** La XAPI imposta l'indice TLS a `-(dimensione area / 4)` (`-37` per un'area di
  `0x94` byte). Lo StackBase del thread punta quindi subito *dopo* l'area TLS, e `[StackBase - 0x94]` contiene
  il puntatore ai dati.
- **Avvio dei thread.** `CreateThread` della XAPI passa a `PsCreateSystemThreadEx` la propria
  `XapiThreadStartup` (`0x1359C`) come `SystemRoutine`, che il kernel chiama con `(StartRoutine, StartContext)`.
- **KTHREAD.** `+0x44` è `ApcState.Process` (puntatore a KPROCESS; il gioco scrive `ThreadQuantum` a `+0x14`),
  `+0x12C` è l'ID del thread.
- **Intestazione del kernel.** La XAPI legge l'intestazione PE di `xboxkrnl.exe` a `0x80010000` cercando una
  sezione `INIT`.
- **Percorsi.** I nomi tipo `T:\...` arrivano relativi all'handle `-3` (`ObDosDevicesDirectory`, cioè `\??\`).
- **Partizione cache.** La XAPI legge e riscrive il database delle cache a `Partition0` offset `0x800`, poi
  interroga `Partition5` con `IOCTL_DISK_GET_DRIVE_GEOMETRY` e `IOCTL_DISK_GET_PARTITION_INFO` e la monta come `Z:`.
- **Gestore di crash del gioco.** `0x212380` stampa lo stack e torna alla dashboard con
  `XLD_LAUNCH_DASHBOARD_ERROR` (passando da XOnline, `0x3EC96F`): un "riavvio rapido" con launch data di tipo 1
  significa che prima c'è stata un'eccezione.
- **Primo accesso alla GPU.** `0x41BBC7` legge `0xFD001804` (registri NV2A).

## Grafica, input e audio (fase 3)

- **Push buffer**: il canale 0 parte da GET = 0 (RAMFC azzerata); il driver scrive all'indirizzo fisico 0 un salto
  al vero push buffer (2 MB a `0x03DDF000`). L'oggetto DMA del push buffer ha base 0 e limite `0x07FFAFFF`.
- **Oggetti GPU** (RAMHT a RAMIN `0x10000`, 32 KB): `0x0D` Kelvin `0x97`, `0x0E` `0x39` (memoria-memoria),
  `0x10` `0x9F` (blit), `0x11` `0x62` (superfici 2D), `0x12` `0x44`, `0x19` `0x30`; DMA `0x08` = semafori a
  `0x03FDF000` (il fence che il driver aspetta), `0x09`/`0x0A` = tutta la RAM.
- **Attese del driver**: `NV_PFB_WBC` (`0x100410`) bit 16, `CACHE1_STATUS`/`RUNOUT_STATUS`/`DMA_PUSH`, PUT/GET della
  cache, il fence a `0x03FDF000`, `PGRAPH_INTR` (`0x400100`, se non viene azzerato in scrittura il driver ricorre
  all'infinito).
- **Modalità video**: `AvSendTVEncoderOption(…, 6)` deve restituire cavo, standard e frequenze
  (`0x00C00303` = SCART, PAL-I, 50 e 60 Hz); la tabella delle modalità è a `0x421118`. In EEPROM il PAL 60 Hz è il
  bit `0x00400000` di `XC_VIDEO_FLAGS`. Se non trova una modalità, `CreateDevice` fallisce e il gioco mostra
  "Could not start graphics" (messaggio ereditato dalla versione PC dell'engine) ed esce.
- **Tempo da `rdtsc`**: il gioco legge il contatore della CPU a `0x1A1BF0` (`rdtsc; ret`) e lo trasforma in
  millisecondi a `0x1A1C00` dividendo per `733333` (`0xB3095`); `0x20E140` restituisce la metà bassa; la
  `QueryPerformanceCounter` di XAPI (`0x32633B`) usa `rdtsc` e `QueryPerformanceFrequency` (`0x32634C`) restituisce
  la costante `733333333`; D3D ha `rdtsc; ret` a `0x41B040`. Gli altri `rdtsc` (`0x3B3C5F`-`0x3B4ED6`) misurano
  solo intervalli per statistiche. Il lettore dell'input (`0x848F0`) trasforma lo stato in una maschera di tasti
  (`[obj+0x24]`, precedente in `[obj+0x30]`) con soglia `0x4AFD88` per i tasti analogici.
- **Input (XPP)**: `XInitDevices` `0x43F0E7` (chiamata tramite `0x4409F0`), `XGetDeviceEnumerationStatus`
  `0x4411A2`, `XGetDevices` `0x4409F5`, `XGetDeviceChanges` `0x440A17`, `XInputOpen` `0x441D4D`, `XInputClose`
  `0x441DA3`, `XInputGetCapabilities` `0x441DAF` (il gioco riconosce il volante dal SubType `0x10`),
  `XInputGetState` `0x441F87`, `XInputSetState` `0x441FFA` (il gioco aspetta `ERROR_IO_PENDING` e rilegge lo stato
  nell'intestazione del feedback). Un tipo di dispositivo è `{collegati, cambiati, collegati prima}`: gamepad
  `0x43E994`, memory unit `0x43E918`, cuffie `0x43E884`/`0x43E890`. Inizializzazione a `0x84B40`, controllo dei
  cambiamenti a ogni frame a `0x84C00`, lettura dello stato a `0x848F0`.
- **Errore fatale del gioco**: `0x19070` (`exit(1)` con messaggio).
- **Interrupt di D3D**: l'ISR (`0x41B050`) spegne `PMC_INTR_EN` e accoda il DPC (`0x41BA00`), che legge
  `PMC_INTR_0` e chiama i gestori finché ce n'è (`0x41B7C0` PGRAPH, `0x41B250` vblank, `0x41B870` PFIFO). Il gestore
  PGRAPH mette `PGRAPH_FIFO` a 0, azzera l'interrupt, *poi* legge `TRAPPED_DATA` e chiama la tabella dei metodi
  software (`0x41B784`, valori 1-14); il valore 5 fa `KeSetEvent` sull'evento a `device+0x1DCC`, che D3D aspetta a
  `0x4166FD` quando il push buffer è pieno.
- **Avanzamento della GPU**: D3D legge `PGRAPH 0x400B10` (`PATT_COLOR0`) a `0x41629B` e ci attende a `0x416300`
  (bit 2-6 = contatore); il valore arriva dal metodo `0x310` dell'oggetto `0x44` sul subchannel 5.
- **Sequenza a schermo**: `rockstar.bik`, `sdlogo.bik`, `mc3intro.bik` (video lineari `X8R8G8B8` 640x480, coordinate
  in texel), poi `flash\legal_it_x.xbck` e `flash\bg_savegame.xbck` disegnati in fixed-function, con un blit 2D fra i
  due buffer. Con START: titolo ("Premi START per iniziare") sulla città 3D, controllo dei salvataggi su `U:\`,
  nome del profilo, `flash\bg_main.xbck`, `flash\bg_career.xbck`, `flash\transition.xbck` e il video della carriera
  `video\Story\Sgameint.bik`, il video tutorial `video\Tutorial\Tcustom.bik`, `flash\loadscreen_garage_s.xbck`,
  `flash\garage.xbck` e `flash\toplevel_x.xbck` (acquisto dell'auto, uscita dall'officina), il tutorial
  `video\Tutorial\Tordchek.bik`, `flash\loadscreen_career_filler_sNN.xbck`, `resources\city\hudmap.xbpf` e la gara.
- **File**: XAPI aspetta sugli handle dei file dopo le letture asincrone e passa l'oggetto file a
  `IoQueryVolumeInformation` (`XGetFilePhysicalSortKey`, `0x17A60`: file system "GDFX"/"FATX", poi `FsContext`).
  La cancellazione delle letture (`0x326855`) scorre la coda di `IdexChannelObject + 0x28`.
- **Controller in gara**: `0x2C9AA0` legge lo stato del controller del giocatore e chiama `XInputGetCapabilities`
  con un buffer di 28 byte (`XINPUT_CAPABILITIES`: SubType, gamepad in ingresso, solo i motori in uscita).
- **Audio**: DirectSound aspetta `0xFE820010 ≥ 0x20` e il codec AC97 pronto (`0xFEC0012C` bit 8); XACT si ferma a
  `0x408FFC`.

## DirectSound (DSOUND 5849)

Punti d'ingresso chiamati da fuori la libreria (scansione di tutte le `call`/`jmp` verso `0x3C0400`-`0x3CBEBC`).
I wrapper COM convertono l'interfaccia nell'oggetto interno: `this-8` per `IDirectSound`, `this-0x1C` per
`IDirectSoundBuffer`; gli stream usano `this+4` e i metodi `XMediaObject` (`Process`, `GetStatus`, `Flush`…) vanno
tramite vtable.

| Indirizzo | Funzione | Come è stata riconosciuta |
|---|---|---|
| `0x3C38D3` | `DirectSoundCreate` | 3 argomenti, chiamata all'inizio di XACT |
| `0x3C391A` | `DirectSoundCreateStream` | crea il device e chiama `CreateSoundStream` |
| `0x3C22EC` | `DirectSoundDoWork` | 0 argomenti, chiamata dal ciclo del gioco |
| `0x3C06F2` | `XAudioCalculatePitch` | 48000 Hz -> 0 (resta nativa) |
| `0x3C073C`/`0x3C0741` | `XAudioCreatePcmFormat`/`AdpcmFormat` | riempiono `WAVEFORMATEX` (restano native) |
| `0x3C2034` | `IDirectSound_SetMixBinHeadroom` | XACT la chiama per i 32 mixbin |
| `0x3C36C5`/`0x3C36E9` | `CreateSoundBuffer`/`CreateSoundStream` | Bink usa la prima, `0x3C391A` la seconda |
| `0x3C2054` | `GetOutputLevels` | copia 16 dword da memoria APU |
| `0x3C2210`/`0x3C06D7` | `Lock`/`Unlock` | 8 argomenti / 5 argomenti senza effetto |
| `0x3C2138`/`0x3C215C`/`0x3C2174` | `Play`/`Stop`/`StopEx` | flag di play; `StopEx` con timestamp |
| `0x3C21D4`/`0x3C21F4`/`0x3C21B8` | `GetCurrentPosition`/`SetCurrentPosition`/`GetStatus` | Bink, cursori e stato |
| `0x3C2913`/`0x3C2198` | `SetPlayRegion`/`SetLoopRegion` | controlli su dimensione buffer / regione di play |
| `0x3C28BF`/`0x3C2090`/`0x3C2074` | `SetFrequency`/`SetPitch`/`SetVolume` | `XAudioCalculatePitch`; volume meno headroom |
| `0x3C2240`/`0x3C22D4` | `GetVoiceProperties` (buffer/stream) | legge i volumi dai registri della voce APU; XACT legge `DSVOICEPROPS` (`0x40F566`) e crasha se la struttura resta vuota |
| `0x3C211C` | `SetMixBinVolumes` | coppie (mixbin, volume) |
| `0x3C28F7`/`0x3C2938` | `SetMixBins` (buffer/stream) | Bink costruisce un `DSMIXBINS` sullo stack (`0x3AEF60`) |
| `0x3C2F5C`/`0x3C2F98` | `SetFormat` (buffer/stream) | riceve un `WAVEFORMATEX` (XACT: PCM mono 48 kHz) |
| `0x3C28DB`/`0x3C2933` | `SetOutputBuffer` | l'argomento è un'altra voce (`-0x1C`); MC3 passa sempre NULL |
| `0x3C2F78` | `SetBufferData` | imposta dati e dimensione |
| `0x3C20C8`/`0x3C22A3` | `SetEG` | `DSENVELOPEDESC`; XACT la chiama per quasi ogni effetto con `dwEG` 1 (ampiezza: nell'XDK 0 è il "multi" di pitch e filtro), modo 1, rilascio 32 blocchi da 512 campioni (0,34 s), sustain 255 |
| `0x3C22BC` | `IDirectSoundStream_FlushEx` | XACT ferma gli stream con `5` (ASYNC 1 + ENVELOPE 4), i buffer con `StopEx(3)` (`0x40F229`); il lettore musicale del gioco con `1` |

Elenco completo, compresi gli stream e le quattro funzioni non identificate, in `src/pc/dsound_hle.cpp`.

### Attesa di un fence di D3D

`0x4165A0` aspetta che la GPU arrivi a un valore: device in `[0x422D78]`, "consegnato" in `device+0x2C`, puntatore al contatore
della GPU in `device+0x30`. Confronta `consegnato - obiettivo` con `consegnato - fatto` senza segno (`0x4165BE` `jae`,
ciclo senza pause `0x4166D7`-`0x4166DF` `jb`), e "consegnato" lo legge una volta sola. Se un altro thread consegna altro lavoro
e la GPU supera la prima lettura, la differenza si riavvolge e il ciclo non finisce più (un core al 100%). Sulla Xbox la finestra
è di microsecondi; qui, con frame veloci, il gioco si fermava su "Checking saved games". Il confronto ora è con segno (`jge`,
`jl`, in `hle_patches.cpp`). Pista trovata nel progetto
mcdub3-nx (MIT), correzione scritta da noi.

### Schermate di caricamento

Il thread che apre `flash\loadscreen_*` continua a caricare; il thread avviato a `0x0001BBF0` (`0x1B9D0`) anima la schermata finché
il caricamento non è finito, senza che il gioco aspetti la sua uscita. `loading_screen_seconds` ferma quindi il primo, alla
successiva `NtCreateFile`: fermare il secondo congela la schermata all'inizio e non trattiene nulla alla fine.

### Come XACT ferma e controlla le voci

- **Stop**: `0x40F229` chiama `StopEx(ENVELOPE | RELEASEWAVEFORM)` sui buffer e `FlushEx(ASYNC | ENVELOPE)` sugli
  stream. Gli effetti partono in loop e finiscono solo con il rilascio dell'inviluppo: senza, un suono fermato
  suonava fino alla fine del campione (anche 14 s) e si sommava al successivo.
- **"Sta suonando?"** (`0x40F198`): per i buffer `GetStatus & 1`, per gli stream lo stato XMO `& 0x10000`
  (`DSSTREAMSTATUS_PLAYING`). Gli altri bit: `0x20000` in pausa, `0x40000` senza dati, `0x80000` rilascio in corso.
  Anche il lettore musicale del gioco (`0x2ED80F`) dopo il flush aspetta che `0x10000` si spenga.
- **Voci brevi in streaming** (voci dei pedoni, `dx.xwb`, 74 voci mono a 24000 Hz): XACT prepara due pacchetti leggendo dal
  disco. Una voce che sta in una sola lettura arriva subito a "fine dati" (`0x40B5A4`): marca il secondo pacchetto `0xDEAD` e chiede
  a `0x40603C` se c'è una lettura in corso. Sulla Xbox c'è, e il pacchetto parte dopo. Qui le letture finiscono subito:
  `0x40603C` azzera lo stato del pacchetto letto, risponde no e XACT toglie il bit 8 del suono, che non riceve più nulla.
  La domanda è saltata (`0x40B5B0`, in `hle_patches.cpp`). Il `Discontinuity` arriva quindi prima del primo pacchetto: lo stream
  resta `PLAYING` fino alla fine di quel pacchetto, altrimenti XACT lo dà per finito (`0x40F198`).
- **Stream fermato**: XACT continua a rimandare pacchetti (letti con polling dello stato, non con callback,
  `0x40B241`) finché non vede lo stream fermo; uno stream fermato da un flush asincrono restituisce quindi i pacchetti
  senza suonarli finché non viene preparato di nuovo (pausa, formato o mixbin).
- **Audio 3D**: il percorso c'è (`0x40F80F`: `0x3C2F9D` -> interna `0x2C7C`, posizione relativa all'ascoltatore
  `0x411238`; `0x3C22E3` -> `0x1627`, distanza, cono, volumi fronte/retro, doppler e angoli in un blocco di 0x38
  byte; `0x3C225C`/`0x3C22D9` -> `0x0C94`, lo copia nella voce; `0x0DC5` accende o spegne il 3D, bit 0 di
  `settings+0xB`). MC3 però non lo usa: nessun buffer `DSBCAPS_CTRL3D` e `0x0DC5` sempre con 0. XACT
  posiziona i suoni con i volumi dei mixbin.

DirectSound originale, senza APU, restituisce `DSERR_NODRIVER` (`0x88780078`); XACT allora fallisce e crasha nella
propria pulizia (`0x408FFC`).

## Carriera, codici e HUD

Indirizzi verificati sulla versione PAL, usati dalla pagina Cheat (`launcher_ui.cpp`, `hle_patches.cpp`) e dall'HUD di
mc3port (`hud.cpp`).

- **Soldi della carriera**: `int32` a `[[0x00557F0C] + 0xC] + 0xACC` (22000 in una carriera nuova). Il puntatore vale
  solo con una carriera caricata: `career_money()` controlla ogni passo con `VirtualQuery`.
- **Codici del gioco**: 15 stringhe codificate a `0x463D10`..`0x463DAC`. Il gioco codifica il testo scritto con
  `0x00192350` (`out = (c + 0x45 + i - 0x41) % 58 + 0x41`, `i` posizione del carattere) e lo confronta in
  `0x001923B0`. Codici ed effetti:

| Codice | Effetto (indirizzo scritto) |
|--------|-----------------------------|
| `roadtrip`, `crosscountry`, `urbansprawl` | `0x53F2EC`/`0x53F2ED` |
| `ontheroad` | `0x509F80` |
| `hyperagro` | `0x558818` |
| `dfens`, `allin`, `Rjnr` | `0x4B361C` = 1, 0, 2 |
| `getheadj`, `getheadk`, `getheadl`, `getheadm` | `0x4B39D8` = 0..3 |
| `trythisathome` | inverte `0x557F08` |
| `rimbuk`, `kubmir` | soldi +1 / -1 |

  Scrivere quegli indirizzi da fuori non basta: il gioco applica l'effetto solo quando il codice passa dal suo
  menu. Per questo la pagina Cheat cambia solo i soldi e per il resto elenca i codici.
- **Sblocchi**: la domanda "questo oggetto è sbloccato?" è il `thiscall` `0x00292CD0(categoria, oggetto, flag)`
  (primi byte `83 EC 14 8B 54 24 20`), con 33 categorie (0-21 e 100-111), non ancora mappate.
- **HUD**:
  - Il colore scelto nel negozio è nella texture del quadrante (gialla `02ADDC00`, personalizzata `032FF380`), non
    nel colore dei vertici (grigio 0,87) né in `combiner_factor0` (`FFFFFFFF`). Il pezzo del nitro (texture
    `033C0600`, 80x40, a 77,-47 dal tachimetro) porta invece il colore nei vertici.
  - I simboli dei tasti sono nell'atlante `03342700` (128x128, hash FNV `A30CEDEE3C0953C1`): A, B, X, L, R, il
    cerchio bianco (White, ~82,63-107,88) e il nero (Black, ~8,99-33,124). mc3port scrive LB e RB sui due cerchi
    quando decodifica la texture (`letter_shoulder_buttons` in `renderer_d3d11.cpp`), perché sui controller di oggi
    quei tasti sono LB e RB. Le versioni PlayStation sono atlanti a parte.
  - L'anello del radar è fatto di quarti ruotati delle texture `03390200` e `033B0380`; la maschera rotonda è la
    texture 8x8 `00BD4800`.

## Indirizzi raccolti dal progetto mcdub3-nx

Il progetto [mcdub3-nx](https://github.com/laureladastra/mcdub3-nx) (MIT) ricompila lo stesso XBE europeo: i suoi indirizzi
valgono qui. Quelli segnati "verificato" li abbiamo controllati nel nostro disassemblato; gli altri sono da confermare.

| Indirizzo | Cosa | Stato |
|---|---|---|
| `0x4165A0`, `0x422D78`, `0x422D80` | attesa di un fence di D3D, device, stato del push buffer | verificato (vedi sopra) |
| `0x1BBF0`, `0x1B9D0` | thread che anima la schermata di caricamento | verificato |
| `0x1B15D0` | thread "battito": sezione critica `0x527C68`, poi `Sleep(0)` (`0x1B164D`) | verificato; qui non gira a vuoto (0-1 `Sleep(0)` ogni 10 s) |
| `0x41B550` | callback del push buffer: `param & 0x1F` comando (1 = flip), `(param >> 5) & 7` intervallo (menu: 2) | da confermare |
| `0x41B170` | ritiro dei flip, restituisce quanti; device `+0x1C0` vblank, `+0x1C8` ultimo obiettivo | da confermare |
| `0x2D5400` | un passo del ciclo principale | da confermare |
| `0x4073D9`-`0x407A14` | 37 funzioni dell'API di XACT (le ultime dieci sono salti) | da confermare |
| `0x40DBEF` | scelta della variazione di un suono XACT | da confermare |
| `0x40B71F`, `0x40BE8F` | cambio di stato e stop di un suono XACT; stato a `snd+0x30` (5 suona, 7 fermo) | da confermare |
| `0x4078AD` -> `0x40EDB3` | preparazione di un suono; nel descrittore il bit 2 vuol dire "in streaming" | da confermare |
| `0x4260AD`, vtable `0x4A5594` | decoder WMA della musica (`mx.xwb`, 110 brani a 44,1 kHz) | da confermare |
| `0x3AB590`, `0x3ADA70`, `0x3AD2C0`, `0x3AD280`, `0x3AD8D0`, `0x3ACDF0` | Bink: Open, Close, DoFrame, CopyToBuffer, NextFrame, Wait | da confermare |
| `0x3B2F40`, `0x3AEDB0` | Bink: decodifica video e audio di un fotogramma | da confermare |
| `0x2E9AC0` | rilevamento MMX di Bink | da confermare |
| `0x2DB4C0` | `memmove` della CRT | da confermare |
| `0x20880`, `0x20B6F0`, `0x534770`, `0x4B2D48`-`0x4B2D60` | passo del tempo di gioco (vedi `hle_patches.cpp`); tetto del passo a `0x4B2D5C` | confermano la nostra mappa |
| `0x277EE0` | `mcPgCareer::ReportRaceResult`; ascoltatori di fine gara `0x2793C0`, `0x279430` | da confermare |
| `0x152420`, salto a `0x1525FC` | unica differenza di codice con la Remix americana (`ja` invece di `jae`) | da confermare: nessun disco americano per provarlo |

## Rete

- XNet scrive i registri della scheda di rete a `0xFEF00000` e collega l'interrupt di livello 4 (ISR `0x3D437B`).
- All'avvio deriva chiavi con `XcHMAC`, `XcDESKeyParity`, `XcKeyTable`, `XcBlockCrypt*`.
