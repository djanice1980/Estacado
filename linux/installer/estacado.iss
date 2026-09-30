; Inno Setup script for the Estacado package (build/package).
; Built by linux/build-installer.sh; PackageDir, RedistDir, OutputDir, AppVersion
; are passed with /D.

#ifndef AppVersion
  #define AppVersion "0.9.0"
#endif

[Setup]
AppId={{6F2B1C0E-6E1D-4B3A-9D8C-5E7A2D4C1B90}
AppName=The Darkness (Estacado)
AppVersion={#AppVersion}
AppPublisher=Estacado (unofficial fan port)
AppComments=Unofficial PC port of The Darkness (Xbox 360). Game files not included.
; Program Files, read-only for the game: it keeps settings and logs in
; %LOCALAPPDATA%\The Darkness and saves in Saved Games\The Darkness
; (runtime/runtime_user_paths.h).
PrivilegesRequired=admin
DefaultDirName={autopf}\The Darkness (Estacado)
DefaultGroupName=The Darkness (Estacado)
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=TheDarkness-Estacado-{#AppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\TheDarknessSettings.exe
LicenseFile={#PackageDir}\licenses\LICENSE.txt

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
; The package as built. Machine-specific and writable files (the game location
; file, settings, saves, logs, shader caches) and the game itself are left out.
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; \
  Excludes: "\game,\game\*,\logs,\logs\*,\runtime_data,\runtime_data\*,\vkd3d-proton.cache*,\TheDarkness.pc.toml,\TheDarkness.mods.toml,\TheDarkness.game.toml,\portable.txt,\launcher_debug.txt"
Source: "{#RedistDir}\VC_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: VCRedistNeeded

[Icons]
Name: "{group}\The Darkness"; Filename: "{app}\TheDarknessSettings.exe"; WorkingDir: "{app}"
Name: "{group}\Saved games"; Filename: "{%USERPROFILE}\Saved Games\The Darkness"
Name: "{group}\Settings and logs"; Filename: "{localappdata}\The Darkness"
Name: "{group}\Uninstall The Darkness (Estacado)"; Filename: "{uninstallexe}"
Name: "{userdesktop}\The Darkness"; Filename: "{app}\TheDarknessSettings.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Dirs]
; Created for the shortcuts; the game creates them too. Never removed.
Name: "{%USERPROFILE}\Saved Games\The Darkness"; Flags: uninsneveruninstall
Name: "{localappdata}\The Darkness"; Flags: uninsneveruninstall

[Run]
Filename: "{tmp}\VC_redist.x64.exe"; Parameters: "/install /passive /norestart"; \
  StatusMsg: "Installing the Microsoft Visual C++ runtime..."; Flags: waituntilterminated; \
  Check: VCRedistNeeded
Filename: "{app}\TheDarknessSettings.exe"; Description: "Start the launcher"; \
  Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
const
  PF_AVX2_INSTRUCTIONS_AVAILABLE = 40;

function IsProcessorFeaturePresent(Feature: DWORD): BOOL;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';

// The build needs MSVC runtime 14.44 or newer (Visual Studio 2022 17.14).
function VCRedistNeeded: Boolean;
var
  Installed, Major, Minor: Cardinal;
  Key: String;
begin
  Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  Result := True;
  if RegQueryDWordValue(HKLM64, Key, 'Installed', Installed) and (Installed = 1) and
     RegQueryDWordValue(HKLM64, Key, 'Major', Major) and
     RegQueryDWordValue(HKLM64, Key, 'Minor', Minor) then
    Result := (Major < 14) or ((Major = 14) and (Minor < 44));
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  if not IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) then
    Result := MsgBox('This processor does not report AVX2, which the port requires ' +
      '(Intel Haswell / AMD Zen or newer). The game will likely not start.' + #13#10#13#10 +
      'Install anyway?', mbConfirmation, MB_YESNO) = IDYES;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      'Game files are not included. In the launcher, use "Choose game folder..." ' +
      '(the folder with default.xex) or "Choose disc image..." (your .iso).' + #13#10#13#10 +
      'Saves: ' + ExpandConstant('{%USERPROFILE}') + '\Saved Games\The Darkness' + #13#10 +
      'Settings and logs: ' + ExpandConstant('{localappdata}') + '\The Darkness';
end;
