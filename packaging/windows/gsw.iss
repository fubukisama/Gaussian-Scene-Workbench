; GPL-3.0-or-later. Build through scripts/build_windows_installer.ps1.
#include "generated.iss"

[Setup]
AppId={#GswAppId}
AppName=Gaussian Splatting Workbench
AppVersion={#GswVersion}
AppVerName=Gaussian Splatting Workbench {#GswVersion}
AppPublisher=Gaussian Scene Workbench contributors
AppPublisherURL=https://github.com/fubukisama/Gaussian-Scene-Workbench
AppSupportURL=https://github.com/fubukisama/Gaussian-Scene-Workbench/issues
AppUpdatesURL=https://github.com/fubukisama/Gaussian-Scene-Workbench/releases
DefaultDirName={code:GetDefaultInstallDir}
UsePreviousAppDir=yes
DefaultGroupName=Gaussian Splatting Workbench
AllowNoIcons=yes
DisableProgramGroupPage=no
DisableDirPage=no
DisableWelcomePage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern dynamic
WizardSizePercent=120
SetupIconFile={#GswIcon}
UninstallDisplayIcon={app}\bin\Gaussian Scene Workbench.exe
UninstallDisplayName=Gaussian Splatting Workbench
LicenseFile={#GswPayload}\COPYING
OutputDir={#GswOutput}
OutputBaseFilename={#GswOutputName}
Compression=lzma2
SolidCompression=yes
LZMANumBlockThreads=2
CloseApplications=no
RestartApplications=no
RestartIfNeededByRun=no
SetupLogging=yes
VersionInfoVersion={#GswNumericVersion}

[Languages]
Name: "zh_CN"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "en_US"; MessagesFile: "compiler:Default.isl"
Name: "ja_JP"; MessagesFile: "compiler:Languages\Japanese.isl"

[CustomMessages]
#include "messages.iss"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#GswPayload}\gsw-owned-files.txt"; Flags: dontcopy
Source: "{#GswPayload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Gaussian Splatting Workbench"; Filename: "{app}\bin\Gaussian Scene Workbench.exe"; WorkingDir: "{app}"; AppUserModelID: "Fubukisama.GaussianSceneWorkbench.Native"
Name: "{autodesktop}\Gaussian Splatting Workbench"; Filename: "{app}\bin\Gaussian Scene Workbench.exe"; WorkingDir: "{app}"; Tasks: desktopicon; AppUserModelID: "Fubukisama.GaussianSceneWorkbench.Native"

; No [Run], [UninstallDelete], environment modifications or training launch.
; Inno's uninstall log removes installed files only, preserving user-added data.
; Branding changes labels only. Keep the existing AppId, executable and default
; installation directory so older registered installs remain upgradeable.
; Do not delete or rename existing user-customized legacy shortcuts.
[Code]
function GetDriveTypeW(lpRootPathName: String): LongWord;
  external 'GetDriveTypeW@kernel32.dll stdcall';
function SetupProcessId: LongWord;
  external 'GetCurrentProcessId@kernel32.dll stdcall';
function OpenExistingFileForWrite(FileName: String; Access, ShareMode, Security,
  Creation, Attributes, Template: LongWord): LongWord;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseFileProbe(Handle: LongWord): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function DirectoryIsEmpty(Path: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := True;
  if FindFirst(AddBackslash(Path) + '*', FindRec) then begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then begin
          Result := False;
          Break;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function CanWriteAt(Root: String): Boolean;
var
  Probe: String;
begin
  Probe := AddBackslash(Root) + '.gsw-setup-' + IntToStr(SetupProcessId) + '.tmp';
  Result := False;
  if FileExists(Probe) then Exit;
  if SaveStringToFile(Probe, 'GSW installation destination probe', False) then begin
    Result := DeleteFile(Probe);
  end;
end;

function CandidateOnDrive(Drive: String): String;
begin
  Result := '';
  if GetDriveTypeW(Drive) <> 3 then Exit;
  if DirExists(Drive + 'Apps') then begin
    if CanWriteAt(Drive + 'Apps') then Result := Drive + 'Apps\Gaussian Scene Workbench';
  end else if CanWriteAt(Drive) then
    Result := Drive + 'Apps\Gaussian Scene Workbench';
end;

function GetDefaultInstallDir(Param: String): String;
var
  I: Integer;
  Drive, SystemDrive: String;
begin
  SystemDrive := Uppercase(ExtractFileDrive(ExpandConstant('{sys}')));
  Result := '';
  if SystemDrive <> 'D:' then Result := CandidateOnDrive('D:\');
  if Result <> '' then Exit;
  for I := Ord('A') to Ord('Z') do begin
    Drive := Chr(I) + ':\';
    if (Uppercase(ExtractFileDrive(Drive)) <> SystemDrive) and (Drive <> 'D:\') then begin
      Result := CandidateOnDrive(Drive);
      if Result <> '' then Exit;
    end;
  end;
  Result := ExpandConstant('{localappdata}\Programs\Gaussian Scene Workbench');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Destination, Target, NativeTarget: String;
  Owned: TArrayOfString;
  I: Integer;
  Handle: LongWord;
  ExecutableListed: Boolean;
begin
  Result := '';
  Destination := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  if (CompareText(Destination, ExtractFileDrive(Destination) + '\') = 0) or
     (CompareText(Destination, ExpandConstant('{win}')) = 0) or
     (CompareText(Destination, GetEnv('USERPROFILE')) = 0) then begin
    Result := CustomMessage('GswUnsafeDirectory');
    Exit;
  end;
  if DirExists(Destination) and
     (GetIniString('Installation', 'AppId', '', Destination + '\gsw-installation.ini') <> '{#GswAppId}') then begin
    { Allow an empty destination but never overlay an unrelated directory. }
    if not DirectoryIsEmpty(Destination) then begin
      Result := CustomMessage('GswNonemptyDirectory');
      Exit;
    end;
  end;
  { Fail before copying anything; never terminate a running app/worker. The
    trusted embedded list excludes user-added models, projects and runtimes. }
  if DirExists(Destination) then begin
    ExtractTemporaryFile('gsw-owned-files.txt');
    if not LoadStringsFromFile(ExpandConstant('{tmp}\gsw-owned-files.txt'), Owned) then begin
      Result := CustomMessage('GswBusyInstall');
      Exit;
    end;
    if GetArrayLength(Owned) = 0 then begin
      Result := CustomMessage('GswBusyInstall');
      Exit;
    end;
    ExecutableListed := False;
    for I := 0 to GetArrayLength(Owned) - 1 do begin
      if (Owned[I] = '') or (Pos(#0, Owned[I]) > 0) or
         (Pos(':', Owned[I]) > 0) or (Copy(Owned[I], 1, 1) = '/') or
         (Copy(Owned[I], 1, 1) = '\') then begin
        Result := CustomMessage('GswBusyInstall');
        Exit;
      end;
      if Owned[I] = 'bin/Gaussian Scene Workbench.exe' then ExecutableListed := True;
      Target := AddBackslash(Destination) + Owned[I];
      StringChangeEx(Target, '/', '\', True);
      if FileExists(Target) then begin
        if Copy(Target, 1, 2) = '\\' then
          NativeTarget := '\\?\UNC\' + Copy(Target, 3, Length(Target))
        else
          NativeTarget := '\\?\' + Target;
        Handle := OpenExistingFileForWrite(NativeTarget, $C0000000, 0, 0, 3, $80, 0);
        if Handle = $FFFFFFFF then begin
          Log('Pre-copy access/lock check refused target: ' + Target);
          Result := CustomMessage('GswBusyInstall');
          Exit;
        end;
        CloseFileProbe(Handle);
      end;
    end;
    if not ExecutableListed then Result := CustomMessage('GswBusyInstall');
  end;
end;

procedure InitializeWizard;
var
  RuntimePage: TOutputMsgWizardPage;
begin
  RuntimePage := CreateOutputMsgPage(wpLicense, CustomMessage('GswRuntimeTitle'),
    CustomMessage('GswRuntimeSubtitle'), CustomMessage('GswRuntimeBody'));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption := CustomMessage('GswFinished');
end;
