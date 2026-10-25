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
- **Input (XPP)**: `XInitDevices` `0x43F0E7` (chiamata tramite `0x4409F0`), `XGetDeviceEnumerationStatus`
  `0x4411A2`, `XGetDevices` `0x4409F5`, `XGetDeviceChanges` `0x440A17`, `XInputOpen` `0x441D4D`, `XInputClose`
  `0x441DA3`, `XInputGetCapabilities` `0x441DAF` (il gioco riconosce il volante dal SubType `0x10`),
  `XInputGetState` `0x441F87`, `XInputSetState` `0x441FFA` (il gioco aspetta `ERROR_IO_PENDING` e rilegge lo stato
  nell'intestazione del feedback). Un tipo di dispositivo è `{collegati, cambiati, collegati prima}`: gamepad
  `0x43E994`, memory unit `0x43E918`, cuffie `0x43E884`/`0x43E890`. Inizializzazione a `0x84B40`, controllo dei
  cambiamenti a ogni frame a `0x84C00`, lettura dello stato a `0x848F0`.
- **Errore fatale del gioco**: `0x19070` (`exit(1)` con messaggio).
- **Avanzamento della GPU**: D3D legge `PGRAPH 0x400B10` (`PATT_COLOR0`) a `0x41629B` e ci attende a `0x416300`
  (bit 2-6 = contatore); il valore arriva dal metodo `0x310` dell'oggetto `0x44` sul subchannel 5.
- **Sequenza a schermo**: `rockstar.bik`, `sdlogo.bik`, `mc3intro.bik` (video lineari `X8R8G8B8` 640x480, coordinate
  in texel), poi `flash\legal_it_x.xbck` e `flash\bg_savegame.xbck` disegnati in fixed-function, con un blit 2D fra i
  due buffer. Con START: titolo ("Premi START per iniziare") sulla città 3D, controllo dei salvataggi su `U:\`,
  nome del profilo, `flash\bg_main.xbck`, `flash\bg_career.xbck`, `flash\transition.xbck` e il video della carriera
  `video\Story\Sgameint.bik`.
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
| `0x3C28F7`/`0x3C2F78` | `SetFormat`/`SetBufferData` | aggiorna il formato / imposta dati e dimensione |

Elenco completo, compresi gli stream e le quattro funzioni non identificate, in `src/pc/dsound_hle.cpp`.

DirectSound originale, senza APU, restituisce `DSERR_NODRIVER` (`0x88780078`); XACT allora fallisce e crasha nella
propria pulizia (`0x408FFC`).

## Rete

- XNet scrive i registri della scheda di rete a `0xFEF00000` e collega l'interrupt di livello 4 (ISR `0x3D437B`).
- All'avvio deriva chiavi con `XcHMAC`, `XcDESKeyParity`, `XcKeyTable`, `XcBlockCrypt*`.
