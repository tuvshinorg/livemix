; LiveMix Windows installer (Inno Setup 6)
;
; Built by package-livemix.ps1, which stages the release rundir and passes:
;   /DAppVersion=1.0.0  /DSourceDir=<staged rundir>  /DOutputDir=<folder>  /DRepoRoot=<repository>
;
; The in-app updater (frontend/utility/LiveMixUpdater.cpp) runs this installer with
; /SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS and expects LiveMix to start
; again afterwards, which the second [Run] entry does.

#ifndef AppVersion
  #error AppVersion must be defined
#endif
#ifndef SourceDir
  #error SourceDir must be defined
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef RepoRoot
  #define RepoRoot "..\..\.."
#endif

#define AppExe "bin\64bit\obs64.exe"
#define AppExeDir "bin\64bit"

[Setup]
AppId={{8F4C2E71-5B3A-4D9E-9C61-2A7F0B4E1D35}
AppName=LiveMix
AppVersion={#AppVersion}
AppVerName=LiveMix {#AppVersion}
AppPublisher=LiveMix
AppPublisherURL=https://github.com/tuvshinorg/livemix
AppSupportURL=https://github.com/tuvshinorg/livemix/issues
AppUpdatesURL=https://github.com/tuvshinorg/livemix/releases
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName=LiveMix
VersionInfoDescription=LiveMix installer
DefaultDirName={autopf}\LiveMix
DisableProgramGroupPage=yes
LicenseFile={#RepoRoot}\COPYING
OutputDir={#OutputDir}
OutputBaseFilename=LiveMix-{#AppVersion}-Setup
SetupIconFile={#RepoRoot}\frontend\cmake\windows\obs-studio.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName=LiveMix
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
; Remove the previous version's program files so no stale plugin or DLL survives an update.
; Settings and documents live in the user's profile and are not touched.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\core"
Type: filesandordirs; Name: "{app}\data"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\LiveMix"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\{#AppExeDir}"
Name: "{autodesktop}\LiveMix"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\{#AppExeDir}"; Tasks: desktopicon

[Run]
; Interactive install: offer to start LiveMix on the last page
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\{#AppExeDir}"; Description: "{cm:LaunchProgram,LiveMix}"; Flags: nowait postinstall skipifsilent
; Silent update from inside LiveMix: start the new version straight away
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\{#AppExeDir}"; Flags: nowait runasoriginaluser skipifnotsilent
