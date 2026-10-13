; SPDX-License-Identifier: MIT
;
; Inno Setup script for Selsearch on Windows (x64).
; Build: iscc /DAppVersion=0.1.0 /DSourceDir=C:\path\to\staging packaging\windows\selsearch.iss
; where the staging dir holds selsearchd.exe, selsearch.exe, selsearch-web.exe,
; onnxruntime.dll and everything windeployqt added (see .github/workflows/build.yml).
;
; Installs per user (no admin rights) into %LOCALAPPDATA%\Programs\Selsearch.
; "Start with Windows" is a setting inside Selsearch (Settings > Appearance);
; the uninstaller removes its registry value.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\staging"
#endif

[Setup]
AppId={{6B0F3B8E-2D7C-4E61-9C55-2F7A1C9E4D3B}
AppName=Selsearch
AppVersion={#AppVersion}
AppVerName=Selsearch {#AppVersion}
AppPublisher=Selsearch contributors
AppPublisherURL=https://github.com/svyyyaaat/selsearch
DefaultDirName={localappdata}\Programs\Selsearch
DefaultGroupName=Selsearch
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809 or later (Per-Monitor-V2 DPI awareness, DWM cloaking).
MinVersion=10.0.17763
LicenseFile=..\..\LICENSE
OutputDir=.
OutputBaseFilename=Selsearch-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\selsearchd.exe
UninstallDisplayName=Selsearch
; Stop a running selsearchd before replacing its files.
CloseApplications=force
RestartApplications=no

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Selsearch"; Filename: "{app}\selsearchd.exe"; Comment: "Screen search and launcher"

[Registry]
; Created by Selsearch itself when "Start with Windows" is on; only removed here.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Selsearch"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\selsearchd.exe"; Description: "Start Selsearch now"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\selsearch.exe"; Parameters: "quit"; Flags: runhidden; RunOnceId: "StopSelsearch"
