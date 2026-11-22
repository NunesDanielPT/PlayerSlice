; Instalador do PLAYERSLICE (Inno Setup 6)
#define AppName "PLAYERSLICE"
#ifndef AppVersion
  #define AppVersion "0.3.2"
#endif

[Setup]
AppId={{6B0C4C2E-5A51-4F0B-9E0C-2C7D9A1F5E11}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Daniel Nunes
AppPublisherURL=https://github.com/NunesDanielPT
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=..\dist
OutputBaseFilename=PLAYERSLICE-Setup
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\PLAYERSLICE.exe
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
WizardStyle=modern
WizardImageFile=wizard.bmp,wizard-2x.bmp
WizardSmallImageFile=small.bmp,small-2x.bmp
; perguntar sempre o idioma (Português / English)
ShowLanguageDialog=yes
UsePreviousLanguage=no
; fechar a app ao atualizar e voltar a abri-la
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "pt"; MessagesFile: "compiler:Languages\Portuguese.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\dist\PLAYERSLICE\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\PLAYERSLICE.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\PLAYERSLICE.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.psproj"; ValueType: string; ValueName: ""; ValueData: "PLAYERSLICE.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\PLAYERSLICE.Project"; ValueType: string; ValueName: ""; ValueData: "Projeto PLAYERSLICE"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\PLAYERSLICE.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\PLAYERSLICE.exe,0"
Root: HKA; Subkey: "Software\Classes\PLAYERSLICE.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\PLAYERSLICE.exe"" ""%1"""

[INI]
; idioma escolhido no instalador → idioma inicial da app
Filename: "{app}\playerslice.ini"; Section: "General"; Key: "Language"; String: "{language}"

[Run]
Filename: "{app}\PLAYERSLICE.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent runasoriginaluser
; atualização automática (modo silencioso): voltar a abrir a app
Filename: "{app}\PLAYERSLICE.exe"; Flags: nowait runasoriginaluser; Check: WizardSilent
