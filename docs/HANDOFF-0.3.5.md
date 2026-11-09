# Handoff 0.3.5 — stato, prove e cose aperte (9 novembre 2026)

Scritto per riprendere il lavoro da un'altra sessione o da un altro PC. Tutto il codice citato è su `develop`
(`39e61cd` e precedenti), CI verde fino a `493d82b`; `39e61cd` da controllare. Nessun merge in `main`, nessun tag.

## 1. Commit della 0.3.5 su develop, in ordine

| Commit | Cosa | Verifica |
|---|---|---|
| `143d715` | Controller via SDL3 (PlayStation, Switch, ecc.), forma XInput mantenuta | CI; nessun pad fisico provato |
| `77f15d2` | Passo fisso mai sotto 1/60 s | superato da `493d82b` |
| `f25094c` | Versione 0.3.5, CHANGELOG | — |
| `368a0c1` | Guardia di memoria: una scrittura subito dopo un buffer committa la pagina (come le letture) | — |
| `5b393fa` | GPU dedicata sui portatili (DXGI 1.6, `MC3_GPU_PREFERENCE=0` per il default), riga `d3d:` nel profilo, core del thread GPU nelle statistiche | prove 15/16 |
| `1820c0d` | Disco **USA** accettato (hash in `game_disc.cpp`, tutte le 13 patch HLE si applicano), flip ritirati anche oltre il bersaglio (`0x41B1B4`), risoluzione automatica max 1080 su GPU integrate (<1 GB) | prova 18 |
| `c70294e` | **F9** scrive un secondo di draw nel log; ancore `@file` dello script minuscole; `MC3_STEP_FLOOR` | — |
| `4ae0c07` | Otto correzioni dall'audit (vedi §6) | ctest 11/11, prova 30 |
| `493d82b` | **Passo fisso a sessantesimi interi**: menu a 60 fps non più fermi su questo portatile | confermato a mano |
| `39e61cd` | **Present senza attesa dello schermo** (`ALLOW_TEARING`), riserve del gioco arrotondate al granulo 64 KB, guardia anche su pagina libera, riga `frame lights` nel dump F9 | prova F: città da 30,0 a 60,0 fps |

## 2. Ambiente di prova su questo portatile

- i7-1360P (4 P + 8 E core), Intel Iris Xe + NVIDIA RTX A500 (schermo sull'Intel), Windows 11.
- ISO: `D:\GIT\mc3port\game\Midnight Club 3 - DUB Edition Remix (USA).iso` (gitignored). Dati in `Documenti\mc3port`
  (log in `logs\`, salvataggi in `xbox_hdd\UDATA\54540079`; profilo "Giocatore 1" con carriera iniziata e neon acquistato).
- Build locale: VS 2026 Insiders, `cmake.exe` in `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`,
  preset `pc-x86`, generatore "Visual Studio 18 2026", SDL3 da `FETCHCONTENT_SOURCE_DIR_SDL3=build/sdl3/SDL3-3.4.20`.
  Il link del DLL fallisce finché un `mc3.exe` è aperto. `ctest` solo da PowerShell (il `tar` di Git Bash non è quello di Windows).
- Avvio senza mani, da una cartella di lavoro qualsiasi:

  ```
  MC3_NO_DIALOG=1 MC3_SKIP_INTRO=1 MC3_PAUSE_IN_BACKGROUND=0 MC3_PROFILE=1 MC3_RENDER_SCALE=1.5
  MC3_INPUT_SCRIPT="8:start,@bg_savegame+8:a,@bg_savegame+12:a,@bg_savegame+16:a,@bg_savegame+20:a,@bg_main+5:a,@bg_career+4:a,@tnavsys+2:a,@tnavsys+4:a,@loadscreen_career_filler+15~60:rt"
  build/pc-x86/Release/mc3.exe "<iso>"
  ```

  Con il profilo esistente arriva in città in ~75 s e tiene l'acceleratore 60 s. Senza profilo: `@bg_savegame+5:a,+9:start,+13:a`
  crea "Giocatore 1"; poi `@bg_career+4:a`, filmati saltati con `@sgameint+4:start,+7:a,@tcustom+4:start,+7:a`, garage
  (`@garage`, `@toplevel_x`, negozio vernici `@ps_vp_d_scion_tc_05`). Chiudere con `taskkill /IM mc3.exe /F`, non con il
  `kill` di MSYS (chiude la finestra e lascia una falsa eccezione nel log).
- Senza `MC3_PAUSE_IN_BACKGROUND=0` una finestra avviata da uno shell in background non ha il focus e il port apre il menu
  F1 e mette in pausa. `MC3_DUMP_FRAMES=<n>` salva un BMP ogni n present (conversione con PowerShell System.Drawing).
- Strumenti aggiunti in questa tornata: **F9** (un secondo di `frame rect`/`frame draw`/`frame lights` nel log),
  `MC3_STEP_FLOOR=<s>` (alza il passo fisso), riga `d3d:` al secondo con `MC3_PROFILE=1` (conteggio vblank di D3D,
  bersagli e coda dei flip), `on CPU n` nelle statistiche del renderer, `MC3_WATCHDOG=<s>` (dove sono i thread),
  `MC3_TRACE_WAITS=1`, `MC3_TRACE_SAVES=1`.

## 3. Menu fermi/lenti a 60 fps (#6, #12): cosa si sa

- Riproducibile su questo portatile: a 60 fps i menu restavano indietro di secondi, F1 (pausa+ripresa) li sbloccava;
  a 30 fps tutto normale. Sul Ryzen 9 + RTX 3080 del proprietario non succede: lì i frame sono tutti a 2 blank
  (99 %) e il passo fisso resta 0,0167; qui i frame oscillano fra 1 e 3 blank e il passo seguiva la media di 8 frame
  cambiando ogni frame fra 0,0167 e 0,0229.
- Prove con `MC3_STEP_FLOOR`: 0,0172 → fermi; 0,025 → non confermato; 0,0334 → menu reattivi ma tutto il resto al doppio.
- Correzione `493d82b`: passo arrotondato a sessantesimi interi (`time_frame_shown`, `hle_patches.cpp`). Confermata a mano
  sui menu principali (prova 31).
- **Aperto**: dopo la correzione, in una partenza manuale (prova `neon2`) la schermata "Controllo dei salvataggi" è rimasta
  ferma a 60 fps con passo 0,0167 costante e nessuna riga `saves:` nel log, nessun tasto premuto. Gli avvii con lo
  script (che preme A a +8/+12/+16 s) la passano sempre. Ipotesi da verificare: quella schermata aspetta un input o un
  evento che la pressione sblocca, oppure il passo conta ancora. La prova `stall1` (watchdog ogni 2 s, `MC3_TRACE_SAVES=1`)
  era in corso al momento di questo handoff: leggere `watchdog:` per il thread a priorità 0 con frame `mc3_run+199C6C`.
- Dal disassemblato (`tools/xbe_to_pe.py` + `dumpbin`, file `xbe/game.asm`): l'interfaccia Flash avanza per conteggio di
  frame (`0x4B13B0` = 1 o 2 game frame per fotogramma Flash; vale 2 quando la modalità video `0x4B4E8C` == 1; la
  modalità la scrive `0x2D27C0` e `0x2D239E` la traduce nell'intervallo di presentazione D3D). I lettori di
  `steps_per_second` (`0x4B2D6C`) sono a `0x119D81`, `0x126F36`, `0x126FAA`, `0x12E59A`, `0x12E5D0`.

## 4. Frame rate (#6 e i cali intermittenti)

- Il renderer presentava con `Present(0,0)` su swap chain flip-model senza `ALLOW_TEARING`: un frame appena più lungo del
  refresh aspettava il successivo. In città su questo portatile: 30,0 fps esatti con GPU al 30-50 %, `Present` 500-700 ms/s,
  uguale a 1,5x, 1x e con gli effetti spenti (prove cityA/C/D). Con `39e61cd`: 60,0 fps, `Present` 6-7 ms/s (prova cityF).
- I cali a 20-30 s osservati nelle prove 3-13 (tutti i thread fermi, passo che cresce) hanno quasi certamente la stessa
  origine; da riverificare con una sessione lunga a 60 fps dopo `39e61cd`.
- Costo CPU per draw: con la cache degli stati non più svuotata a ogni draw (`4ae0c07`), "stati e texture" da 2,5-5 µs a
  1,4 µs per draw.

## 5. Crash del garage / cambio musica (#13)

- Riprodotto (prova cityB) entrando in carriera: `C0000005 at 0042D351`, lettura di `06BCF000`, prima pagina dopo un buffer
  di 0xF000 a `06BC0000`; la pagina dopo risultava **libera** (resto del granulo da 64 KB). Istruzione:
  `movd mm2, [esi+30h]` nel decoder WMA (sezione WMADEC), stessa catena dei rapporti #8/#10
  (`0045BF88 004390C8 0042AC9A 00428A81 004283EB 00426961 00425A22 00425FAF 0040B3A5 ...`). Il decoder legge e scrive
  qualche byte oltre i suoi buffer; su Xbox la pagina dopo era RAM.
- Correzione `39e61cd` + `368a0c1`: riserve a indirizzo scelto dal gioco arrotondate al granulo, guardia che committa la
  pagina dopo un buffer anche se libera (solo se segue memoria committata). Il dump del crash sta in
  `scratchpad/cityB/mc3port-crash-report.zip` di questa sessione e in `Documenti\mc3port\logs`.
- **Da fare**: ripetere il percorso di cityB più volte con la build nuova (lo script di §2 senza `@tnavsys`); cercare
  `memory: read/write past committed memory` nel log in corrispondenza dei cambi di musica.

## 6. Audit (rapporto completo in chat del 9/11, 20 punti)

Fatti (`4ae0c07`): timeout del wait-all (`kernel_dispatcher.cpp`), cache stati D3D (`renderer_over_shown_scene` senza
`ForgetBound`, aggiunto a clear-alfa e `upload_scaled`), filtro dei componenti di percorso (`kernel_io.cpp`), limiti nei
parser XBE/XDVDFS, combo Back+Start solo in `XInputGetState`, `mc3port.ini` scritto su `.tmp` e rinominato,
`xiso_inspect.py` rifiuta un percorso di uscita che inizia con `-`, `package.ps1` fallisce in CI senza Inno Setup.

Da fare (punti 9-20): URL dell'aggiornamento vincolati a `github.com/<repo>/releases/download/`; cache texture che cresce
(`g_textures` senza `erase`, chiave con palette per formati non paletted, `Enlarger::done_`); `CreateBuffer` senza
controllo (`renderer_d3d11.cpp` ~2580); overflow di `KeInterruptTime` dopo 25,6 h; azioni CI fissate per SHA e
`choco install innosetup --version`; testi "solo versione europea" residui (`installer/mc3port.iss`, `recomplauncher.json`,
`tools/release_notes.ps1`, `README.md` ~66-70, `tools/package_readme.txt`, `docs/DEVELOPMENT.md:38` → `pwsh`); test
mancanti (parser ini, combo, confronto versioni, `gather_vertex_data`, `wait_unpaused`); ordine dei controlli di rilascio;
scansione SDL ogni lettura (`gamepad.cpp`); ciclo di debug che legge 1,2 MB oltre una texture (`nv2a_kelvin.cpp` ~964);
pulizie (`tools/debug_run.py`, `kernel_xbe()`, `init_flags`, doppioni di costanti e helper).

## 7. Neon (#14)

- Riprodotto: StreetGlow acquistato sulla TC, nessun bagliore nel negozio né nel garage.
- Dump F9 con e senza neon (`run24`, finestre 1-25): nessuna texture esclusiva del neon, draw identici per superficie,
  quindi non è geometria. Il bloom va in un target 256x256 (`02A54500`) composto sul frame con `blend DST_ALPHA, ONE`.
- Ora il dump ha la riga `frame lights` (maschera luci, tipo, colore, posizione, raggio per draw illuminato): ripetere
  F9 con neon e con "Nessuno" nel negozio vernici e confrontare le luci. Il port implementa luci infinite, locali e
  spot sia su CPU (`light_vertex`) sia su GPU (HLSL in `renderer_d3d11.cpp`).

## 8. Altro

- Nota da mcdub3-nx (laureladastra, stessa Remix ricompilata): indirizzi del Remix per decoder WMA (`0x4260AD` crea,
  `0x425F87` Process), fence D3D `0x4165A0`, vblank D3D `sub_0041B250` (oggetto interrupt = device `[0x422D78]` + 0x1C28:
  contatore +0x1C0, prossimo bersaglio +0x1C4, ultimo +0x1C8, coda flip +0x174/+0x180 e +0x178/+0x184, testa +0x1BC),
  heartbeat `Sleep(0)` a `0x1B1654`. File scaricati in `build/mcdub3/` (non tracciato).
- Per il rilascio: merge `develop` → `main`, tag `v0.3.5`, poi inviare `mc3runtime.dll` a Microsoft come falso positivo
  prima dell'annuncio (#9). Dopo il rilascio: commentare #12 e #6 con il link, chiedere conferma ai segnalatori; #13 idem.
- Il file `--list` (copia del `default.xbe` scritta per errore nella radice) è stato cancellato; il tool ora lo impedisce.
