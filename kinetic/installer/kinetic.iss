; Inno Setup script for KINETIC. Built by .github/workflows/windows-release.yml.
#ifndef SourceDir
  #define SourceDir "..\dist\KINETIC"
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{6F3B2C1A-9E4D-4B7A-A1C5-3D2E1F000001}
AppName=KINETIC
AppVersion=0.1.0
AppPublisher=KINETIC
DefaultDirName={autopf}\KINETIC
DefaultGroupName=KINETIC
OutputDir={#OutDir}
OutputBaseFilename=KINETIC-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\kinetic.exe
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\KINETIC"; Filename: "{app}\kinetic.exe"
Name: "{group}\Uninstall KINETIC"; Filename: "{uninstallexe}"
Name: "{autodesktop}\KINETIC"; Filename: "{app}\kinetic.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.kinetic"; ValueType: string; ValueName: ""; ValueData: "KINETIC.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\KINETIC.Project"; ValueType: string; ValueName: ""; ValueData: "KINETIC Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\KINETIC.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\kinetic.exe"" ""%1"""

[Run]
Filename: "{app}\kinetic.exe"; Description: "Launch KINETIC"; Flags: nowait postinstall skipifsilent
