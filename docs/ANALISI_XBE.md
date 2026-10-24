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
