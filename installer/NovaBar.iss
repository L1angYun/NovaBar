#ifndef AppVersion
  #error Pass AppVersion from build/installer.ps1.
#endif
#ifndef PublishDir
  #error Pass PublishDir from build/installer.ps1.
#endif
#ifndef BridgeName
  #error Pass BridgeName from build/installer.ps1.
#endif
#ifdef FixtureId
  #ifndef FixtureDataDir
    #error Pass the protected FixtureDataDir for native installer tests.
  #endif
  #define DataRoot FixtureDataDir
#else
  #define DataRoot "{localappdata}\NovaBar"
#endif

[Setup]
#ifdef FixtureId
AppId=NovaBar.Installer.Fixture.{#FixtureId}
AppName=NovaBar Installer Fixture
DefaultDirName={tmp}\NovaBar-Installer-Fixture-{#FixtureId}
UninstallDisplayName=NovaBar Installer Fixture {#FixtureId}
#else
AppId={{AC89FB71-DA06-4CB7-B391-12931BE58E50}
AppName=NovaBar
DefaultDirName={localappdata}\Programs\NovaBar
UninstallDisplayName=NovaBar
SetupIconFile={#PublishDir}\Assets\NovaBar.ico
LicenseFile={#PublishDir}\LICENSE
#endif
AppVersion={#AppVersion}
AppVerName=NovaBar {#AppVersion}
AppPublisher=L1angYun
AppPublisherURL=https://github.com/L1angYun/NovaBar
AppSupportURL=https://github.com/L1angYun/NovaBar/issues
AppUpdatesURL=https://github.com/L1angYun/NovaBar/releases
DefaultGroupName=NovaBar
UsePreviousAppDir=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
SetupArchitecture=x64
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22621
CloseApplications=no
CloseApplicationsFilter=NovaBar.exe,NovaBar.Engine.exe,NovaBar.Capture.exe
RestartApplications=no
UninstallDisplayIcon={app}\NovaBar.exe
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}.0
OutputBaseFilename=NovaBar-{#AppVersion}-win-x64-Setup
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
UninstallLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PublishDir}\*"; DestDir: "{app}"; Excludes: "*.pdb,NovaBar.XamlBridge*.dll"; Flags: ignoreversion recursesubdirs createallsubdirs
; Every changed bridge uses a fresh identity. Never overwrite a mapped current DLL on repair.
Source: "{#PublishDir}\{#BridgeName}"; DestDir: "{app}"; Flags: onlyifdoesntexist
Source: "Stop-NovaBar.ps1"; Flags: dontcopy noencryption
Source: "Stop-NovaBar.ps1"; DestDir: "{app}\_installer"; Flags: ignoreversion
Source: "Resume-NovaBar.ps1"; DestDir: "{app}\_installer"; Flags: ignoreversion

#ifndef FixtureId
[Icons]
Name: "{userprograms}\NovaBar\NovaBar"; Filename: "{app}\NovaBar.exe"; WorkingDir: "{app}"
Name: "{userprograms}\NovaBar\{cm:UninstallProgram,NovaBar}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\NovaBar"; Filename: "{app}\NovaBar.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\NovaBar.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,NovaBar}"; Flags: nowait postinstall skipifsilent; Check: IsInteractiveInstall
#endif

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""{app}\_installer\Resume-NovaBar.ps1"" -InstallDirectory ""{app}"" -DataDirectory ""{#DataRoot}"" -StatePath ""{tmp}\NovaBar.UpdateState.json"""; Flags: runhidden waituntilterminated runasoriginaluser; Check: IsUpdaterInstall

[Code]
function IsUpdaterInstall: Boolean;
begin
  Result := ExpandConstant('{param:NOVABARUPDATE|0}') = '1';
end;

function IsInteractiveInstall: Boolean;
begin
  Result := not IsUpdaterInstall;
end;

function RestoreAndClose(const HelperPath: String): Boolean;
var
  ExitCode: Integer;
  Parameters: String;
begin
  Result := True;
  if not FileExists(ExpandConstant('{app}\NovaBar.Engine.exe')) then exit;
  Parameters := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    HelperPath + '" -InstallDirectory "' + ExpandConstant('{app}') + '" -DataDirectory "' + ExpandConstant('{#DataRoot}') + '"';
  if IsUpdaterInstall then
    Parameters := Parameters + ' -StatePath "' + ExpandConstant('{tmp}\NovaBar.UpdateState.json') + '"';
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters,
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  if Result then Result := ExitCode = 0;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not FileExists(ExpandConstant('{app}\NovaBar.Engine.exe')) then exit;
  ExtractTemporaryFile('Stop-NovaBar.ps1');
  if not RestoreAndClose(ExpandConstant('{tmp}\Stop-NovaBar.ps1')) then
    Result := 'NovaBar could not restore its appearance before the update. Please exit NovaBar and try again.';
end;

function InitializeUninstall: Boolean;
begin
  Result := RestoreAndClose(ExpandConstant('{app}\_installer\Stop-NovaBar.ps1'));
  if not Result then
    MsgBox('NovaBar could not restore its appearance. Please exit NovaBar and retry uninstalling.', mbError, MB_OK);
end;
