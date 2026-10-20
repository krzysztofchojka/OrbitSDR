; Inno Setup script for OrbitSDR (Windows x64)
; Build from the repository root after build_mingw.sh has produced orbitsdr.exe and the DLLs:
;   ISCC.exe /DAppVersion=1.0.0 installer\orbitsdr.iss
; All paths below are relative to THIS file (installer\), hence the "..\".

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
; Keep this GUID constant forever: it lets new versions upgrade old installs in place.
AppId={{B0A4C7E2-5B7B-4C1F-9E3A-6D1F2A8C4E10}
AppName=OrbitSDR
AppVersion={#AppVersion}
AppPublisher=OrbitSDR
AppPublisherURL=https://github.com/krzysztofchojka/OrbitSDR
AppSupportURL=https://github.com/krzysztofchojka/OrbitSDR/issues
DefaultDirName={autopf}\OrbitSDR
DefaultGroupName=OrbitSDR
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\orbitsdr.exe
SetupIconFile=..\icon.ico
LicenseFile=..\LICENSE
OutputDir=..\installer_output
OutputBaseFilename=OrbitSDR-Setup-{#AppVersion}-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Installs per-user by default (no admin prompt); the user can choose "for all users" in the dialog.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\orbitsdr.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\*.dll";           DestDir: "{app}"; Flags: ignoreversion
Source: "..\bandplan.json";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\icon.png";        DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\OrbitSDR";           Filename: "{app}\orbitsdr.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall OrbitSDR"; Filename: "{uninstallexe}"
Name: "{autodesktop}\OrbitSDR";     Filename: "{app}\orbitsdr.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\orbitsdr.exe"; Description: "{cm:LaunchProgram,OrbitSDR}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
