; Script de instalador Inno Setup 6 para ZeroCam (Windows 11 x64)
#define MyAppName "ZeroCam"
#ifndef MyAppVersion
  #define MyAppVersion "1.3.4"
#endif
#define MyAppExeName "ZeroCam.exe"
#define SourceDir "..\dist\ZeroCam-Windows-x64"

[Setup]
AppId={{7E6B8D41-9A2B-4E1C-8A54-3F4C9E2B6102}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=ZeroCam
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
OutputDir=..\dist
OutputBaseFilename=ZeroCam-Setup-x64-v{#MyAppVersion}
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\windows\app\res\zerocam.ico
CloseApplications=yes
#ifdef EnableSign
SignTool=zc
SignedUninstaller=yes
#endif

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\ZeroCam.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ZeroCamSource.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ZeroCamDShow64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ZeroCamDShow32.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LEEME.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\adb\*"; DestDir: "{app}\adb"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--register-vcam"; StatusMsg: "Configurando cámara virtual y reglas de Firewall..."; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "Iniciar ZeroCam ahora"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--unregister-vcam"; RunOnceId: "ZeroCamUnregister"; Flags: runhidden waituntilterminated
