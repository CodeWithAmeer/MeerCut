; MeerCut installer (Inno Setup 6). Built by scripts\package.ps1, which passes:
;   /DAppVersion=1.0.0 /DNumericVersion=1.0.0 /DStageDir=<staged program folder> /DOutputDir=... /DOutputName=...
; Per-user by default (no administrator rights; %LOCALAPPDATA%\Programs\MeerCut), all users when chosen in the dialog.
; Uninstalling removes the program folder and its shortcuts only: projects, autosaves, presets, settings, AI models and
; the render cache (%LOCALAPPDATA%\MeerCut, %APPDATA%\MeerCut, HKCU\Software\MeerCut) stay where they are.
; MeerCut was called Lumacut until 2026-10-28: it is a product entry of its own (its own AppId - a Lumacut install is
; neither reused nor removed), and MeerCut.exe takes over Lumacut's data folders and settings at its first start.

#ifndef AppVersion
  #error AppVersion must be defined (run scripts\package.ps1)
#endif

[Setup]
AppId={{A446A8A6-B377-464E-94FE-9DDA14B7242C}
AppName=MeerCut
AppVersion={#AppVersion}
AppVerName=MeerCut {#AppVersion}
VersionInfoVersion={#NumericVersion}
AppPublisher=CodeWithAmeer
AppPublisherURL=https://github.com/CodeWithAmeer
AppSupportURL=https://github.com/CodeWithAmeer
AppUpdatesURL=https://github.com/CodeWithAmeer
AppCopyright=Copyright (C) 2026 CodeWithAmeer. GPL-3.0-or-later.
DefaultDirName={autopf}\MeerCut
DefaultGroupName=MeerCut
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.17763
LicenseFile={#StageDir}\licenses\LICENSE.txt
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
SetupIconFile=..\apps\editor\meercut.ico
UninstallDisplayIcon={app}\MeerCut.exe
UninstallDisplayName=MeerCut {#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=yes
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assoc"; Description: "Open MeerCut projects (.mcproj, and Lumacut's .lcproj) with MeerCut"; GroupDescription: "File types:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\MeerCut"; Filename: "{app}\MeerCut.exe"
Name: "{group}\Uninstall MeerCut"; Filename: "{uninstallexe}"
Name: "{autodesktop}\MeerCut"; Filename: "{app}\MeerCut.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.mcproj"; ValueType: string; ValueName: ""; ValueData: "MeerCut.Project"; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.mcproj\OpenWithProgids"; ValueType: string; ValueName: "MeerCut.Project"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.lcproj"; ValueType: string; ValueName: ""; ValueData: "MeerCut.Project"; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.lcproj\OpenWithProgids"; ValueType: string; ValueName: "MeerCut.Project"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\MeerCut.Project"; ValueType: string; ValueName: ""; ValueData: "MeerCut project"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\MeerCut.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\MeerCut.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\MeerCut.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\MeerCut.exe"" ""%1"""; Tasks: assoc

[Run]
Filename: "{app}\MeerCut.exe"; Description: "{cm:LaunchProgram,MeerCut}"; Flags: nowait postinstall skipifsilent
