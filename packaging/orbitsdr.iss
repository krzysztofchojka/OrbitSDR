; Inno Setup script for OrbitSDR (Windows x64 or ARM64)
; Build from the repository root after scripts/build_mingw.sh has produced orbitsdr.exe and the DLLs:
;   ISCC.exe /DAppVersion=1.0.0 packaging\orbitsdr.iss
;   ISCC.exe /DAppVersion=1.0.0 /DArch=arm64 packaging\orbitsdr.iss   (Windows on ARM)
; All paths below are relative to THIS file (packaging\), hence the "..\".

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
; Tag used in the output file name (e.g. v0.2.0-beta.1)
#ifndef FileTag
  #define FileTag "dev"
#endif
; x64 or arm64 (the build in the repository root must be for the same one)
#ifndef Arch
  #define Arch "x64"
#endif
#if Arch == "arm64"
  #define ArchAllowed "arm64"
#else
  #define ArchAllowed "x64compatible"
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
SetupIconFile=..\assets\icon.ico
LicenseFile=..\LICENSE
OutputDir=..\installer_output
OutputBaseFilename=OrbitSDR_{#FileTag}_windows_{#Arch}_setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed={#ArchAllowed}
ArchitecturesInstallIn64BitMode={#ArchAllowed}
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
Source: "..\assets\bandplan.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\assets\pipelines\*.json"; DestDir: "{app}\pipelines"; Flags: ignoreversion
Source: "..\assets\orbitsdr.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\assets\icon.png"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\OrbitSDR";           Filename: "{app}\orbitsdr.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall OrbitSDR"; Filename: "{uninstallexe}"
Name: "{autodesktop}\OrbitSDR";     Filename: "{app}\orbitsdr.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\orbitsdr.exe"; Description: "{cm:LaunchProgram,OrbitSDR}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
