; Inno Setup 6.6+ script for mc3port. Built by tools/package.ps1:
;   ISCC /DAppVersion=<x.y.z> /DSourceDir=<staged files> /DOutputDir=dist installer\mc3port.iss
; Installs for the current user (no administrator rights), so the launcher's updates run without a prompt.
; Settings, saves, logs and shader cache live in Documents\mc3port and survive uninstalling.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{8C3E1B2A-5D4F-4E7A-9B61-3F2D7A0C5E91}
AppName=mc3port
AppVersion={#AppVersion}
AppVerName=mc3port {#AppVersion}
AppPublisher=SickSartori
AppPublisherURL=https://github.com/SickSartori/mc3port
AppSupportURL=https://github.com/SickSartori/mc3port/issues
AppUpdatesURL=https://github.com/SickSartori/mc3port/releases
AppComments=Unofficial native Windows port of Midnight Club 3: DUB Edition Remix (Xbox, Europe or USA). No game files included.
AppCopyright=MIT licence. Not affiliated with Rockstar Games, Take-Two Interactive or Microsoft.
VersionInfoVersion={#AppVersion}
VersionInfoDescription=mc3port setup
DefaultDirName={autopf}\mc3port
DefaultGroupName=mc3port
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x86compatible
DisableWelcomePage=no
SetupIconFile=..\res\mc3.ico
UninstallDisplayIcon={app}\mc3.exe
UninstallDisplayName=mc3port - Midnight Club 3: DUB Edition Remix
WizardStyle=modern dark
WizardImageFile=wizard-100.png,wizard-150.png,wizard-200.png,wizard-250.png
WizardSmallImageFile=wizard-small-100.png,wizard-small-150.png,wizard-small-200.png,wizard-small-250.png
ShowLanguageDialog=auto
CloseApplications=yes
; Windows caches the icons it has shown; after mc3.exe is replaced, the shortcuts kept the old one. This makes
; Setup tell the shell that associations changed, so it reloads them.
ChangesAssociations=yes
RestartApplications=no
OutputDir={#OutputDir}
OutputBaseFilename=mc3port-{#AppVersion}-windows-x86-setup
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"

[Messages]
english.WelcomeLabel2=This will install [name/ver], an unofficial native Windows port of Midnight Club 3: DUB Edition Remix.%n%nNo game files are included: on the first start you choose the disc image (ISO) of your own European or USA Xbox copy.%n%nYour settings and saves are kept in your user folder and stay when you update or uninstall.
italian.WelcomeLabel2=Verrà installato [name/ver], un port nativo non ufficiale per Windows di Midnight Club 3: DUB Edition Remix.%n%nNessun file del gioco è incluso: al primo avvio scegli l'immagine del disco (ISO) della tua copia Xbox europea o americana.%n%nImpostazioni e salvataggi restano nella tua cartella utente e non si perdono con aggiornamenti o disinstallazione.

[CustomMessages]
english.DesktopShortcut=Create a desktop shortcut
italian.DesktopShortcut=Crea un collegamento sul desktop
english.ShortcutName=Midnight Club 3 DUB Edition Remix (mc3port)
italian.ShortcutName=Midnight Club 3 DUB Edition Remix (mc3port)
english.StartNow=Start mc3port
italian.StartNow=Avvia mc3port

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopShortcut}"

[InstallDelete]
; Up to 0.3.0 the documents were next to mc3.exe; they are in docs now.
Type: files; Name: "{app}\README.md"
Type: files; Name: "{app}\CHANGELOG.md"
Type: files; Name: "{app}\LICENSE"
Type: files; Name: "{app}\THIRD_PARTY.md"
Type: filesandordirs; Name: "{app}\licenses"
; The shipped shaders are replaced whole on each update: an older renderer's are never read again.
Type: filesandordirs; Name: "{app}\shader_cache"

[Files]
Source: "{#SourceDir}\mc3.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\mc3runtime.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#SourceDir}\docs\README.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\CHANGELOG.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\LICENSE"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\THIRD_PARTY.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\licenses\*"; DestDir: "{app}\docs\licenses"; Flags: ignoreversion
Source: "{#SourceDir}\shader_cache\*"; DestDir: "{app}\shader_cache"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{cm:ShortcutName}"; Filename: "{app}\mc3.exe"
Name: "{autodesktop}\{cm:ShortcutName}"; Filename: "{app}\mc3.exe"; Tasks: desktopicon

[Run]
; Offered on the last page. No [Code] section and no automatic start after a silent install: with them, antivirus
; heuristics (Microsoft, Elastic, DeepInstinct on VirusTotal) took the installer for malware.
Filename: "{app}\mc3.exe"; Description: "{cm:StartNow}"; Flags: nowait postinstall skipifsilent
