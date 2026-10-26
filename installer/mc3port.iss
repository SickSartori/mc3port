; Inno Setup 6.6+ script for mc3port. Built by tools/package.ps1:
;   ISCC /DAppVersion=<x.y.z> /DSourceDir=<staged files> /DOutputDir=dist installer\mc3port.iss
; Installs for the current user (no administrator rights), so the launcher's updates run without a prompt.
; Settings, saves, log and shader cache live in %LOCALAPPDATA%\mc3port and survive uninstalling.

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
AppComments=Unofficial native Windows port of Midnight Club 3: DUB Edition Remix (Xbox, PAL). No game files included.
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
RestartApplications=no
OutputDir={#OutputDir}
OutputBaseFilename=mc3port-{#AppVersion}-windows-x86-setup
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"

[Messages]
english.WelcomeLabel2=This will install [name/ver], an unofficial native Windows port of Midnight Club 3: DUB Edition Remix.%n%nNo game files are included: on the first start you choose the disc image (ISO) of your own European Xbox copy.%n%nYour settings and saves are kept in your user folder and stay when you update or uninstall.
italian.WelcomeLabel2=Verrà installato [name/ver], un port nativo non ufficiale per Windows di Midnight Club 3: DUB Edition Remix.%n%nNessun file del gioco è incluso: al primo avvio scegli l'immagine del disco (ISO) della tua copia Xbox europea.%n%nImpostazioni e salvataggi restano nella tua cartella utente e non si perdono con aggiornamenti o disinstallazione.

[CustomMessages]
english.DesktopShortcut=Create a desktop shortcut
italian.DesktopShortcut=Crea un collegamento sul desktop
english.ShortcutName=Midnight Club 3 DUB Edition Remix (mc3port)
italian.ShortcutName=Midnight Club 3 DUB Edition Remix (mc3port)
english.StartNow=Start mc3port
italian.StartNow=Avvia mc3port

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopShortcut}"

[Files]
Source: "{#SourceDir}\mc3.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\mc3runtime.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{cm:ShortcutName}"; Filename: "{app}\mc3.exe"
Name: "{autodesktop}\{cm:ShortcutName}"; Filename: "{app}\mc3.exe"; Tasks: desktopicon

[Run]
; After a normal install, offered on the last page; after an update from the launcher (/UPDATE), always.
Filename: "{app}\mc3.exe"; Description: "{cm:StartNow}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\mc3.exe"; Flags: nowait; Check: IsLauncherUpdate

[Code]
function IsLauncherUpdate: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/UPDATE') = 0 then
      Result := True;
end;
