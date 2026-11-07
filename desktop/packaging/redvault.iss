; Inno Setup script for the redvaultmessages desktop app. Build it with
; packaging\package.ps1, which stages dist\redvaultmessages first.
;
; A per-user install: no administrator rights needed, installed under
; %LOCALAPPDATA%\Programs\redvaultmessages, with a Start menu entry and an
; uninstaller. Uninstalling also removes this computer's redvaultmessages settings
; (last handle, app-lock PIN hash, preferences) and any leftover cache.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
; Never change the AppId: it's how Windows recognises upgrades of this app.
AppId={{D794C5BC-79DF-4347-A5CE-28CA72064A23}
AppName=redvaultmessages
AppVersion={#AppVersion}
AppVerName=redvaultmessages {#AppVersion}
AppPublisher=Redvault Systems
AppPublisherURL=https://www.redvaultsystems.com/
AppSupportURL=https://www.redvaultsystems.com/
DefaultDirName={autopf}\redvaultmessages
DefaultGroupName=redvaultmessages
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=..\dist
OutputBaseFilename=redvaultmessages-Setup-{#AppVersion}
SetupIconFile=..\app\icons\redvault.ico
UninstallDisplayIcon={app}\bin\redvaultmessages.exe
UninstallDisplayName=redvaultmessages
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Closes a running Redvault (via Restart Manager) before replacing its files.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\dist\redvaultmessages\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Upgrading from when the app was called "Redvault": the old exe and shortcuts.
Type: files; Name: "{app}\bin\Redvault.exe"
Type: files; Name: "{autoprograms}\Redvault.lnk"
Type: files; Name: "{autodesktop}\Redvault.lnk"

[Icons]
Name: "{autoprograms}\redvaultmessages"; Filename: "{app}\bin\redvaultmessages.exe"
Name: "{autodesktop}\redvaultmessages"; Filename: "{app}\bin\redvaultmessages.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\redvaultmessages.exe"; Description: "{cm:LaunchProgram,redvaultmessages}"; Flags: nowait postinstall skipifsilent
; After an automatic update (which runs this installer silently), start the new version.
Filename: "{app}\bin\redvaultmessages.exe"; Flags: nowait skipifnotsilent

[UninstallDelete]
; Decrypted-file cache (normally already empty: it's removed at sign-out).
Type: filesandordirs; Name: "{localappdata}\Redvault Systems\Redvault"

[Registry]
; QSettings lives here; remove it with the app.
Root: HKCU; Subkey: "Software\Redvault Systems\Redvault"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Redvault Systems"; Flags: uninsdeletekeyifempty
