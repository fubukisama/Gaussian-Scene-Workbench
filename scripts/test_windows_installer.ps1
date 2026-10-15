#requires -Version 7.2
# GPL-3.0-or-later. Isolated installed-setup acceptance; never launches GSW.
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$SetupPath,
  [Parameter(Mandatory=$true)][string]$TestRoot
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
  throw 'Actual installer acceptance requires Windows.'
}

# Event handlers execute as managed callbacks, not PowerShell runspace jobs.
# Reading/hashing is deliberately excluded from NotifyFilter (LastAccess).
if (-not ('GswInstallerMutationWatcher' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Threading;
public sealed class GswInstallerMutationWatcher : IDisposable {
  public sealed class Mutation {
    public string Kind {get;set;} public string Path {get;set;}
    public string OldPath {get;set;} public string AtUtc {get;set;}
  }
  public sealed class Evidence {
    public string Root {get;set;} public string StartedAtUtc {get;set;}
    public string StoppedAtUtc {get;set;} public long MutationCount {get;set;}
    public long ErrorCount {get;set;} public bool EvidenceTruncated {get;set;}
    public Mutation[] Events {get;set;} public string[] Errors {get;set;}
  }
  readonly FileSystemWatcher watcher;
  readonly ConcurrentQueue<Mutation> changes = new ConcurrentQueue<Mutation>();
  readonly ConcurrentQueue<string> errors = new ConcurrentQueue<string>();
  readonly string root;
  readonly string started = DateTime.UtcNow.ToString("o");
  long mutationCount, errorCount;
  bool disposed;
  public GswInstallerMutationWatcher(string directory) {
    root = System.IO.Path.GetFullPath(directory);
    if(!Directory.Exists(root)) throw new DirectoryNotFoundException(root);
    watcher = new FileSystemWatcher(root, "*");
    watcher.IncludeSubdirectories = true;
    watcher.InternalBufferSize = 65536;
    watcher.NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName |
      NotifyFilters.LastWrite | NotifyFilters.Size | NotifyFilters.CreationTime |
      NotifyFilters.Attributes | NotifyFilters.Security;
    watcher.Changed += (sender, change) => Record(change, null);
    watcher.Created += (sender, change) => Record(change, null);
    watcher.Deleted += (sender, change) => Record(change, null);
    watcher.Renamed += (sender, change) => Record(change, change.OldFullPath);
    watcher.Error += (sender, error) => {
      Interlocked.Increment(ref errorCount);
      Exception exception = error.GetException();
      errors.Enqueue(exception == null ? "Unknown watcher error" : exception.ToString());
    };
    watcher.EnableRaisingEvents = true;
  }
  void Record(FileSystemEventArgs change, string oldPath) {
    long count = Interlocked.Increment(ref mutationCount);
    if(count <= 10000) changes.Enqueue(new Mutation {Kind=change.ChangeType.ToString(),
      Path=change.FullPath, OldPath=oldPath, AtUtc=DateTime.UtcNow.ToString("o")});
  }
  public Evidence StopAndCollect(int quietMilliseconds, int maximumWaitMilliseconds) {
    var clock = Stopwatch.StartNew();
    long previous = Interlocked.Read(ref mutationCount) + Interlocked.Read(ref errorCount);
    long stableFrom = clock.ElapsedMilliseconds;
    while(clock.ElapsedMilliseconds < maximumWaitMilliseconds) {
      Thread.Sleep(25);
      long current = Interlocked.Read(ref mutationCount) + Interlocked.Read(ref errorCount);
      if(current != previous) { previous=current; stableFrom=clock.ElapsedMilliseconds; }
      else if(clock.ElapsedMilliseconds - stableFrom >= quietMilliseconds) break;
    }
    Dispose();
    // Drain already queued, short managed callbacks after cancelling native IO.
    Thread.Sleep(100);
    return new Evidence {Root=root, StartedAtUtc=started, StoppedAtUtc=DateTime.UtcNow.ToString("o"),
      MutationCount=Interlocked.Read(ref mutationCount), ErrorCount=Interlocked.Read(ref errorCount),
      EvidenceTruncated=Interlocked.Read(ref mutationCount)>10000,
      Events=changes.ToArray(), Errors=errors.ToArray()};
  }
  public void Dispose() {
    if(disposed) return;
    disposed=true;
    watcher.EnableRaisingEvents=false;
    watcher.Dispose();
  }
}
'@
}

# Read-only path probes explain Shell-link differences without changing the
# existing correct-target assertions (including 8.3 aliases and reparse paths).
if (-not ('GswInstallerPathProbe' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
public static class GswInstallerPathProbe {
  public sealed class Details {
    public string LongPath {get;set;} public string FinalPath {get;set;}
    public string Error {get;set;}
  }
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern uint GetLongPathNameW(string path, StringBuilder output, uint capacity);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern SafeFileHandle CreateFileW(string path, uint access, uint shareMode,
    IntPtr security, uint creation, uint attributes, IntPtr template);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle, StringBuilder output,
    uint capacity, uint flags);
  public static Details Read(string path) {
    var result = new Details();
    var buffer = new StringBuilder(32768);
    uint length = GetLongPathNameW(path, buffer, (uint)buffer.Capacity);
    if(length > 0 && length < buffer.Capacity) result.LongPath=buffer.ToString();
    else result.Error="GetLongPathNameW: " + new Win32Exception(Marshal.GetLastWin32Error()).Message;
    // FILE_READ_ATTRIBUTES, share read/write/delete, OPEN_EXISTING and backup
    // semantics allow the same non-writing probe for files and directories.
    using(var handle = CreateFileW(path, 0x80, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
      if(handle.IsInvalid) {
        result.Error=(result.Error ?? "") + "; CreateFileW(read attributes): " +
          new Win32Exception(Marshal.GetLastWin32Error()).Message;
        return result;
      }
      buffer.Clear();
      length=GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Capacity, 0);
      if(length > 0 && length < buffer.Capacity) result.FinalPath=buffer.ToString();
      else result.Error=(result.Error ?? "") + "; GetFinalPathNameByHandleW: " +
        new Win32Exception(Marshal.GetLastWin32Error()).Message;
    }
    return result;
  }
}
'@
}

if (-not ('GswInstallerShellLinkUnicode' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;
public static class GswInstallerShellLinkUnicode {
  [ComImport, Guid("000214F9-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IShellLinkW {
    void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int capacity, IntPtr findData, uint flags);
    void GetIDList(out IntPtr idList);
    void SetIDList(IntPtr idList);
    void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder text, int capacity);
    void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string text);
    void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder directory, int capacity);
    void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string directory);
    void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder arguments, int capacity);
    void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string arguments);
    void GetHotkey(out short hotkey);
    void SetHotkey(short hotkey);
    void GetShowCmd(out int showCommand);
    void SetShowCmd(int showCommand);
    void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int capacity, out int iconIndex);
    void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string path, int iconIndex);
    void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string path, uint reserved);
    void Resolve(IntPtr window, uint flags);
    void SetPath([MarshalAs(UnmanagedType.LPWStr)] string path);
  }
  public sealed class Details {
    public string TargetPath {get;set;} public string WorkingDirectory {get;set;}
    public string Arguments {get;set;}
  }
  static object NewLink() {
    return Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("00021401-0000-0000-C000-000000000046")));
  }
  public static Details Read(string shortcut) {
    object instance=NewLink();
    try {
      // STGM_READ. Do not resolve, save, modify or launch the existing link.
      ((IPersistFile)instance).Load(shortcut, 0);
      var link=(IShellLinkW)instance;
      var target=new StringBuilder(32768);
      var directory=new StringBuilder(32768);
      var arguments=new StringBuilder(32768);
      link.GetPath(target, target.Capacity, IntPtr.Zero, 4); // SLGP_RAWPATH
      link.GetWorkingDirectory(directory, directory.Capacity);
      link.GetArguments(arguments, arguments.Capacity);
      return new Details {TargetPath=target.ToString(), WorkingDirectory=directory.ToString(), Arguments=arguments.ToString()};
    } finally { Marshal.FinalReleaseComObject(instance); }
  }
  public static void CreateOwnedFixture(string shortcut, string target, string directory) {
    if(File.Exists(shortcut)) throw new IOException("Fixture link must be new.");
    if(!File.Exists(target) || !Directory.Exists(directory)) throw new IOException("Fixture target must exist without execution.");
    object instance=NewLink();
    try {
      var link=(IShellLinkW)instance;
      link.SetPath(target);
      link.SetWorkingDirectory(directory);
      link.SetDescription("GSW non-executable Unicode shortcut fixture; never launch");
      ((IPersistFile)instance).Save(shortcut, true);
    } finally { Marshal.FinalReleaseComObject(instance); }
  }
}
'@
}

function Assert-Condition([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function Get-Sha256([string]$Path) {
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-ObjectFingerprint([object]$Value) {
  $text = ConvertTo-Json -InputObject $Value -Compress -Depth 20
  $algorithm = [Security.Cryptography.SHA256]::Create()
  try {
    return [BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-', '').ToLowerInvariant()
  } finally { $algorithm.Dispose() }
}

function Test-SamePath([string]$Left, [string]$Right) {
  return [IO.Path]::GetFullPath($Left).TrimEnd('\', '/').Equals(
    [IO.Path]::GetFullPath($Right).TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoReparseAncestor([string]$Path) {
  $cursor = [IO.Path]::GetFullPath($Path)
  while ($cursor) {
    if (Test-Path -LiteralPath $cursor) {
      $item = Get-Item -LiteralPath $cursor -Force
      Assert-Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Reparse path is not allowed in acceptance: $cursor"
    }
    $parent = [IO.Path]::GetDirectoryName($cursor)
    if (-not $parent -or $parent -eq $cursor) { break }
    $cursor = $parent
  }
}

function Resolve-OwnedFile([string]$Root, [string]$Relative) {
  Assert-Condition (-not [string]::IsNullOrWhiteSpace($Relative)) 'Empty manifest path.'
  Assert-Condition (-not [IO.Path]::IsPathRooted($Relative) -and $Relative -notmatch '[\\:\x00\r\n]') "Unsafe manifest path: $Relative"
  $segments = @($Relative -split '/')
  Assert-Condition (-not ($segments | Where-Object { $_ -in @('', '.', '..') })) "Noncanonical manifest path: $Relative"
  $resolved = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
  $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  Assert-Condition ($resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) "Manifest escapes owned root: $Relative"
  Assert-NoReparseAncestor $resolved
  return $resolved
}

function Test-ManifestFiles([string]$Root, [object[]]$Files) {
  Assert-Condition ($Files.Count -gt 0) 'Manifest contains no files.'
  $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($entry in $Files) {
    $relative = [string]$entry.path
    Assert-Condition ($seen.Add($relative)) "Duplicate manifest path: $relative"
    $path = Resolve-OwnedFile $Root $relative
    Assert-Condition (Test-Path -LiteralPath $path -PathType Leaf) "Installed file is missing: $relative"
    Assert-Condition ((Get-Item -LiteralPath $path).Length -eq [long]$entry.size) "Installed file size mismatch: $relative"
    Assert-Condition ((Get-Sha256 $path) -eq [string]$entry.sha256) "Installed file hash mismatch: $relative"
  }
}

function Test-InstalledPayload([string]$InstallRoot, [object]$Build) {
  $payload = Get-Content -LiteralPath (Join-Path $InstallRoot 'installer-payload.json') -Raw | ConvertFrom-Json
  Assert-Condition ($payload.schema -eq 'gsw-installer-payload-v1') 'Wrong installed payload schema.'
  Assert-Condition ($payload.executableSha256 -eq $Build.executableSha256) 'Payload and Setup executable identities differ.'
  Test-ManifestFiles $InstallRoot @($payload.files)
  $ownedPath = Join-Path $InstallRoot 'gsw-owned-files.txt'
  $ownedBytes = [IO.File]::ReadAllBytes($ownedPath)
  Assert-Condition ($ownedBytes.Length -ge 3 -and $ownedBytes[0] -eq 0xef -and $ownedBytes[1] -eq 0xbb -and $ownedBytes[2] -eq 0xbf) 'Owned-file preflight list must have UTF-8 BOM identity for LoadStringsFromFile.'
  $ownedFiles = [IO.File]::ReadAllLines($ownedPath, [Text.UTF8Encoding]::new($true, $true))
  $expectedOwned = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($file in @($payload.files)) { [void]$expectedOwned.Add([string]$file.path) }
  [void]$expectedOwned.Add('installer-payload.json')
  $seenOwned = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($relative in $ownedFiles) {
    [void](Resolve-OwnedFile $InstallRoot $relative)
    Assert-Condition ($seenOwned.Add($relative) -and $expectedOwned.Contains($relative)) 'Unexpected or duplicate owned preflight file.'
  }
  Assert-Condition ($seenOwned.Count -eq $expectedOwned.Count) 'Preflight list omits installed owned files.'
  $executable = Join-Path $InstallRoot 'bin/Gaussian Scene Workbench.exe'
  Assert-Condition ((Get-Sha256 $executable) -eq $Build.executableSha256) 'Installed executable does not match accepted binary.'
  $sourceRoot = Join-Path $InstallRoot 'source'
  $source = Get-Content -LiteralPath (Join-Path $InstallRoot 'source-manifest.json') -Raw | ConvertFrom-Json
  Assert-Condition ($source.schema -eq 'gsw-native-corresponding-source-v1' -and $source.license -eq 'GPL-3.0-or-later') 'Missing GPL corresponding-source identity.'
  Assert-Condition ($source.executableSha256 -eq $Build.executableSha256) 'Installed corresponding-source executable identity mismatch.'
  Assert-Condition (@($source.files).Count -eq [int]$source.sourceFileCount) 'Source count mismatch.'
  Test-ManifestFiles $sourceRoot @($source.files)
  $actualSource = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File)
  Assert-Condition ($actualSource.Count -eq [int]$source.sourceFileCount) 'Unrecorded or missing corresponding source.'
  foreach ($required in @('packaging/windows/gsw.iss', 'scripts/prepare_windows_installer.py',
      'scripts/build_windows_installer.ps1', 'scripts/bootstrap_inno_setup.ps1',
      'scripts/test_windows_installer.py', 'scripts/test_windows_installer.ps1',
      'scripts/test_windows_installer_ui.ps1',
      'docs/WINDOWS_INSTALLER.md')) {
    Assert-Condition (Test-Path -LiteralPath (Join-Path $sourceRoot $required) -PathType Leaf) "Installer source material missing: $required"
  }
  return $payload
}

function Get-UninstallRecord([string]$AppId) {
  $records = @()
  foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
    try {
      $key = $base.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $AppId + '_is1', $false)
      if ($key) {
        try {
          $registrationValues = [ordered]@{}
          foreach ($name in @($key.GetValueNames() | Sort-Object)) {
            $registrationValues[$name] = [ordered]@{ kind=[string]$key.GetValueKind($name)
              value=$key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
          }
          $records += [pscustomobject]@{
            view = [string]$view
            displayName = [string]$key.GetValue('DisplayName')
            displayVersion = [string]$key.GetValue('DisplayVersion')
            installLocation = [string]$key.GetValue('InstallLocation')
            uninstallString = [string]$key.GetValue('UninstallString')
            registrationSha256 = Get-ObjectFingerprint $registrationValues
          }
        } finally { $key.Dispose() }
      }
    } finally { $base.Dispose() }
  }
  if ($records.Count -eq 2) {
    # HKCU uninstall registration can be shared between registry views. An
    # identical projection is not a second physical install; divergent data is.
    $left, $right = $records[0], $records[1]
    if ($left.registrationSha256 -eq $right.registrationSha256) {
      return $left
    }
  }
  return $records
}

function Get-NativeSettingsFingerprint {
  $values = [Collections.Generic.List[string]]::new()
  function Read-SettingsKey([Microsoft.Win32.RegistryKey]$Key, [string]$Prefix) {
    foreach ($name in @($Key.GetValueNames() | Sort-Object)) {
      $value = $Key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
      $encoded = if ($value -is [byte[]]) { [Convert]::ToBase64String($value) } else { ConvertTo-Json -InputObject $value -Compress -Depth 20 }
      $values.Add($Prefix + '|' + $name + '|' + [string]$Key.GetValueKind($name) + '|' + $encoded)
    }
    foreach ($name in @($Key.GetSubKeyNames() | Sort-Object)) {
      $child = $Key.OpenSubKey($name, $false)
      try { Read-SettingsKey $child ($Prefix + '/' + $name) } finally { $child.Dispose() }
    }
  }
  foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
    try {
      $key = $base.OpenSubKey('Software\GaussianSceneWorkbench', $false)
      if ($key) {
        try { Read-SettingsKey $key ([string]$view) } finally { $key.Dispose() }
      } else { $values.Add([string]$view + '|absent') }
    } finally { $base.Dispose() }
  }
  $bytes = [Text.Encoding]::UTF8.GetBytes(($values -join "`n"))
  $algorithm = [Security.Cryptography.SHA256]::Create()
  try { return [BitConverter]::ToString($algorithm.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant() } finally { $algorithm.Dispose() }
}

function Get-DesktopShortcutSnapshot {
  $result = [ordered]@{}
  foreach ($desktop in @([Environment]::GetFolderPath('DesktopDirectory'), [Environment]::GetFolderPath('CommonDesktopDirectory')) | Select-Object -Unique) {
    if (-not $desktop) { continue }
    $shortcut = Join-Path $desktop 'Gaussian Scene Workbench.lnk'
    $result[$shortcut] = if (Test-Path -LiteralPath $shortcut -PathType Leaf) { Get-Sha256 $shortcut } else { 'absent' }
  }
  return ConvertTo-Json -InputObject $result -Compress
}

function Invoke-HiddenInstaller([string]$Executable, [string[]]$Arguments, [switch]$ExpectFailure) {
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $Executable
  $start.WorkingDirectory = $script:runRoot
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
  foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $start
  try {
    Assert-Condition ($process.Start()) 'Unable to start isolated Setup process.'
    Assert-Condition ($process.WaitForExit(300000)) 'Installer timeout; process was not force-terminated. Inspect isolated test evidence.'
    if ($ExpectFailure) {
      Assert-Condition ($process.ExitCode -ne 0) 'Locked-file upgrade unexpectedly succeeded.'
      return $process.ExitCode
    }
    Assert-Condition ($process.ExitCode -eq 0) "Installer exited with $($process.ExitCode). Inspect test logs."
  } finally { $process.Dispose() }
}

function Test-InstalledShortcut([string]$Path, [string]$InstallRoot) {
  Assert-Condition (Test-Path -LiteralPath $Path -PathType Leaf) "Installed shortcut missing: $Path"
  $beforeRead = Get-Sha256 $Path
  $native = [GswInstallerShellLinkUnicode]::Read($Path)
  $actualTarget = [string]$native.TargetPath
  $actualWorkingDirectory = [string]$native.WorkingDirectory
  $expectedTarget = Join-Path $InstallRoot 'bin/Gaussian Scene Workbench.exe'
  $legacyTarget, $legacyDirectory, $legacyError = $null, $null, $null
  $shell = $null
  try {
    # WScript reads lose non-ACP characters on affected Windows systems. Keep
    # the observation for diagnostics only; IShellLinkW owns the strict gate.
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $legacyTarget = [string]$shortcut.TargetPath
    $legacyDirectory = [string]$shortcut.WorkingDirectory
  } catch { $legacyError = $_.Exception.Message }
  finally { if ($shell) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) } }
  $evidenceDirectory = Join-Path $script:runRoot 'shortcut-evidence'
  New-Item -ItemType Directory -Path $evidenceDirectory -Force | Out-Null
  $evidenceStem = Join-Path $evidenceDirectory ('shortcut-' + [guid]::NewGuid().ToString('N'))
  $evidencePath = $evidenceStem + '.json'
  [IO.File]::Copy($Path, ($evidenceStem + '.lnk'), $false)
  $afterRead = Get-Sha256 $Path
  $evidence = [ordered]@{ schema='gsw-shortcut-evidence-v1'; observedAtUtc=[DateTime]::UtcNow.ToString('o')
    pathReader='IShellLinkW'; shortcutPath=$Path; shortcutSha256=$afterRead; shortcutUnchanged=$beforeRead -eq $afterRead
    rawShortcutEvidence=$evidenceStem + '.lnk'; rawShortcutSha256=Get-Sha256 ($evidenceStem + '.lnk')
    actualTargetPath=$actualTarget; expectedTargetPath=$expectedTarget
    actualWorkingDirectory=$actualWorkingDirectory; expectedWorkingDirectory=$InstallRoot
    wscriptTargetPath=$legacyTarget; wscriptWorkingDirectory=$legacyDirectory; wscriptError=$legacyError
    actualTarget=Get-ShortcutPathEvidence $actualTarget; expectedTarget=Get-ShortcutPathEvidence $expectedTarget
    actualDirectory=Get-ShortcutPathEvidence $actualWorkingDirectory; expectedDirectory=Get-ShortcutPathEvidence $InstallRoot }
  [IO.File]::WriteAllText($evidencePath, ($evidence | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
  Write-Output "Shortcut evidence: $evidencePath"
  Assert-Condition ($beforeRead -eq $afterRead) 'Reading a shortcut unexpectedly changed its original bytes.'
  Assert-Condition (Test-SamePath $actualTarget $expectedTarget) "Shortcut target is not the installed bin executable. Actual='$actualTarget'; expected='$expectedTarget'; evidence='$evidencePath'."
  Assert-Condition (Test-SamePath $actualWorkingDirectory $InstallRoot) "Shortcut working directory mismatch. Actual='$actualWorkingDirectory'; expected='$InstallRoot'; evidence='$evidencePath'."
}

function Get-ShortcutPathEvidence([string]$Path) {
  $record = [ordered]@{ rawPath=$Path; characterCount=$Path.Length; absolutePath=$null; exists=$false
    providerPath=$null; longPath=$null; finalPhysicalPath=$null; sha256=$null; error=$null }
  try {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'Shell returned an empty path.' }
    $record.absolutePath = [IO.Path]::GetFullPath($Path)
    $record.exists = Test-Path -LiteralPath $Path
    if ($record.exists) {
      $record.providerPath = (Resolve-Path -LiteralPath $Path).ProviderPath
      $physical = [GswInstallerPathProbe]::Read($record.absolutePath)
      $record.longPath = $physical.LongPath
      $record.finalPhysicalPath = $physical.FinalPath
      $record.error = $physical.Error
      if (Test-Path -LiteralPath $Path -PathType Leaf) { $record.sha256 = Get-Sha256 $Path }
    }
  } catch { $record.error = $_.Exception.Message }
  return [pscustomobject]$record
}

function New-Sentinel([string]$Path, [string]$Text) {
  New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
  Assert-Condition (-not (Test-Path -LiteralPath $Path)) "Sentinel path already exists: $Path"
  [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
  return [pscustomobject]@{ path = $Path; sha256 = Get-Sha256 $Path }
}

function Test-Sentinels([object[]]$Sentinels) {
  foreach ($sentinel in $Sentinels) {
    Assert-Condition (Test-Path -LiteralPath $sentinel.path -PathType Leaf) "User data sentinel disappeared: $($sentinel.path)"
    Assert-Condition ((Get-Sha256 $sentinel.path) -eq $sentinel.sha256) "User data sentinel changed: $($sentinel.path)"
  }
}

$SetupPath = (Resolve-Path -LiteralPath $SetupPath).Path
$sidecarPath = $SetupPath + '.json'
Assert-Condition (Test-Path -LiteralPath $sidecarPath -PathType Leaf) 'Setup test-build metadata is required.'
$build = Get-Content -LiteralPath $sidecarPath -Raw | ConvertFrom-Json
Assert-Condition ($build.schema -eq 'gsw-windows-installer-v1' -and $build.testBuild -is [bool] -and $build.testBuild) 'Only an explicitly testBuild=true Setup can be accepted.'
Assert-Condition ($build.appId -match '^GSW\.Test\.[a-f0-9]{32}$') 'Production or unrecognized AppId is forbidden in acceptance.'
Assert-Condition ([IO.Path]::GetFullPath([string]$build.setupPath).Equals($SetupPath, [StringComparison]::OrdinalIgnoreCase)) 'Sidecar belongs to a different Setup path.'
Assert-Condition ((Get-Sha256 $SetupPath) -eq $build.setupSha256 -and (Get-Item -LiteralPath $SetupPath).Length -eq [long]$build.setupBytes) 'Setup binary does not match its metadata.'
Assert-Condition (($build.locales -join ',') -eq 'zh_CN,en_US,ja_JP') 'Three installer languages are required.'
Assert-Condition (-not $build.trainingRuntimesBundled -and -not $build.automaticAppLaunch) 'Unexpected runtime-installation or application-launch contract.'
Assert-Condition (@(Get-UninstallRecord $build.appId).Count -eq 0) 'This test AppId already has an installation; refusing to overwrite it.'

$TestRoot = [IO.Path]::GetFullPath($TestRoot)
Assert-Condition ($TestRoot.TrimEnd('\', '/') -ne [IO.Path]::GetPathRoot($TestRoot).TrimEnd('\', '/')) 'TestRoot cannot be a drive root.'
Assert-NoReparseAncestor $TestRoot
New-Item -ItemType Directory -Path $TestRoot -Force | Out-Null
$runId = [guid]::NewGuid().ToString('N')
$script:runRoot = Join-Path $TestRoot ('acceptance ' + $runId + ' 空白 日本語')
Assert-Condition (-not (Test-Path -LiteralPath $script:runRoot)) 'Acceptance run directory must be fresh.'
New-Item -ItemType Directory -Path $script:runRoot | Out-Null
$temp = Join-Path $script:runRoot 'temp'
$logs = Join-Path $script:runRoot 'logs'
New-Item -ItemType Directory -Path $temp, $logs | Out-Null
$originalTemp, $originalTmp = $env:TEMP, $env:TMP
$desktopBefore = Get-DesktopShortcutSnapshot
$desktopState = $desktopBefore | ConvertFrom-Json
$desktopInitiallyAbsent = @($desktopState.PSObject.Properties | Where-Object { $_.Value -ne 'absent' }).Count -eq 0
$testDesktopIcon = $env:GITHUB_ACTIONS -eq 'true' -and $desktopInitiallyAbsent
$settingsBefore = Get-NativeSettingsFingerprint
$results = [Collections.Generic.List[object]]::new()
$report = [ordered]@{ schema = 'gsw-installer-acceptance-v1'; setupPath = $SetupPath; setupSha256 = $build.setupSha256
  appId = $build.appId; startedAtUtc = [DateTime]::UtcNow.ToString('o'); passed = $false; locales = $results
  desktopShortcutPreserved = $false; nativeSettingsPreserved = $false; applicationLaunched = $false }
$failure = $null
try {
  $env:TEMP, $env:TMP = $temp, $temp
  # Calibrate the public filesystem observer with controlled mutations and
  # read-only access before accepting any zero-mutation upgrade result.
  $watcherCheckRoot = Join-Path $script:runRoot 'watcher self-check'
  New-Item -ItemType Directory -Path $watcherCheckRoot | Out-Null
  $watcherCheck = [GswInstallerMutationWatcher]::new($watcherCheckRoot)
  try {
    $checkOriginal = Join-Path $watcherCheckRoot 'controlled original.txt'
    $checkRenamed = Join-Path $watcherCheckRoot 'controlled renamed.txt'
    [IO.File]::WriteAllText($checkOriginal, 'created by watcher self-check')
    [IO.File]::AppendAllText($checkOriginal, ' changed')
    [IO.File]::Move($checkOriginal, $checkRenamed)
    [IO.File]::Delete($checkRenamed)
    $mutationsCheck = $watcherCheck.StopAndCollect(500, 5000)
  } finally { $watcherCheck.Dispose() }
  [IO.File]::WriteAllText((Join-Path $script:runRoot 'watcher-mutations-selfcheck.json'), ($mutationsCheck | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
  Assert-Condition ($mutationsCheck.ErrorCount -eq 0) 'Filesystem observer self-check reported an error or buffer overflow.'
  foreach ($kind in @('Changed', 'Created', 'Deleted', 'Renamed')) {
    Assert-Condition (@($mutationsCheck.Events | Where-Object Kind -eq $kind).Count -gt 0) "Filesystem observer did not detect controlled $kind."
  }
  $readOnlyProbe = Join-Path $watcherCheckRoot 'controlled readonly.txt'
  [IO.File]::WriteAllText($readOnlyProbe, 'reading this file must not report mutation')
  $quietCheck = [GswInstallerMutationWatcher]::new($watcherCheckRoot)
  try {
    [void](Get-Sha256 $readOnlyProbe)
    $quietEvidence = $quietCheck.StopAndCollect(500, 5000)
  } finally { $quietCheck.Dispose() }
  [IO.File]::WriteAllText((Join-Path $script:runRoot 'watcher-readonly-selfcheck.json'), ($quietEvidence | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
  Assert-Condition ($quietEvidence.ErrorCount -eq 0 -and $quietEvidence.MutationCount -eq 0) 'Read-only filesystem observer self-check was not quiet.'
  $report['filesystemObserverSelfCheck'] = $true
  foreach ($locale in @('zh_CN', 'en_US', 'ja_JP')) {
    $install = Join-Path $script:runRoot ($locale + ' 程序 安装')
    $group = 'GSW Installer Acceptance ' + $runId + ' ' + $locale
    $shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) ($group + '/Gaussian Scene Workbench.lnk')
    Assert-Condition (-not (Test-Path -LiteralPath (Split-Path -Parent $shortcutPath))) 'Acceptance Start menu group already exists.'
    Assert-Condition ((Get-DesktopShortcutSnapshot) -eq $desktopBefore) 'Production desktop shortcut state changed before locale test.'
    $desktopTask = if ($testDesktopIcon) { '/TASKS=desktopicon' } else { '/TASKS=!desktopicon' }
    $desktopShortcutPath = Join-Path ([Environment]::GetFolderPath('DesktopDirectory')) 'Gaussian Scene Workbench.lnk'
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOCLOSEAPPLICATIONS', '/NORESTARTAPPLICATIONS',
      $desktopTask, ('/LANG=' + $locale), ('/DIR=' + $install), ('/GROUP=' + $group))
    Invoke-HiddenInstaller $SetupPath ($arguments + ('/LOG=' + (Join-Path $logs ($locale + '-install.log'))))
    $payload = Test-InstalledPayload $install $build
    $records = @(Get-UninstallRecord $build.appId)
    Assert-Condition ($records.Count -eq 1 -and $records[0].view -eq 'Registry64') 'Expected one per-user x64 Windows uninstall entry.'
    $record = $records[0]
    Assert-Condition ($record.displayName -eq 'Gaussian Scene Workbench' -and $record.displayVersion -eq $build.version) 'Wrong Windows installed-program identity/version.'
    Assert-Condition (Test-SamePath $record.installLocation $install) 'Wrong uninstall install-location.'
    Test-InstalledShortcut $shortcutPath $install
    if ($testDesktopIcon) { Test-InstalledShortcut $desktopShortcutPath $install }
    $sentinels = @()
    foreach ($name in @('project.gsw', 'model.ply', 'photo.jpg', 'optimizer-checkpoint.pth', 'settings.ini')) {
      $sentinels += New-Sentinel (Join-Path $install ('user acceptance files/' + $name)) ($runId + ' user data ' + $locale + ' ' + $name)
    }
    $sentinels += New-Sentinel (Join-Path $script:runRoot ('separate runtime unchanged/' + $locale + '/runtime-sentinel.txt')) ($runId + ' independent runtime ' + $locale)
    # Do not pass /DIR on either upgrade: this verifies Windows registration
    # remembers this exact installation rather than falling back to a new one.
    $upgradeArguments = @($arguments | Where-Object { -not $_.StartsWith('/DIR=', [StringComparison]::OrdinalIgnoreCase) })
    $registrationBefore = Get-ObjectFingerprint @(Get-UninstallRecord $build.appId)
    $payloadManifestBefore = Get-Sha256 (Join-Path $install 'installer-payload.json')
    $shortcutBeforeLockedUpgrade = Get-DesktopShortcutSnapshot
    $lockedExe = [IO.File]::Open((Join-Path $install 'bin/Gaussian Scene Workbench.exe'),
      [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $mutationWatcher = $null
    try {
      $mutationWatcher = [GswInstallerMutationWatcher]::new($install)
      try {
        $lockedExitCode = Invoke-HiddenInstaller $SetupPath ($upgradeArguments + ('/LOG=' + (Join-Path $logs ($locale + '-locked-upgrade.log')))) -ExpectFailure
      } finally {
        $mutationEvidence = $mutationWatcher.StopAndCollect(500, 5000)
        [IO.File]::WriteAllText((Join-Path $logs ($locale + '-locked-upgrade-filesystem.json')), ($mutationEvidence | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
      }
      Assert-Condition ($mutationEvidence.ErrorCount -eq 0 -and -not $mutationEvidence.EvidenceTruncated) 'Locked upgrade filesystem observation failed or overflowed; zero changes cannot be established.'
      Assert-Condition ($mutationEvidence.MutationCount -eq 0) "Locked upgrade mutated installed paths ($($mutationEvidence.MutationCount) events), even if rollback restored their final bytes."
      Assert-Condition $lockedExe.CanRead 'Test-owned read lock was unexpectedly closed.'
      [void](Test-InstalledPayload $install $build)
      Test-Sentinels $sentinels
      Assert-Condition ((Get-Sha256 (Join-Path $install 'installer-payload.json')) -eq $payloadManifestBefore) 'Failed locked upgrade changed payload metadata.'
      Assert-Condition ((Get-ObjectFingerprint @(Get-UninstallRecord $build.appId)) -eq $registrationBefore) 'Failed locked upgrade changed Windows registration.'
      Assert-Condition ((Get-DesktopShortcutSnapshot) -eq $shortcutBeforeLockedUpgrade) 'Failed locked upgrade changed desktop shortcut state.'
    } finally {
      if ($mutationWatcher) { $mutationWatcher.Dispose() }
      $lockedExe.Dispose()
    }
    Invoke-HiddenInstaller $SetupPath ($upgradeArguments + ('/LOG=' + (Join-Path $logs ($locale + '-reinstall.log'))))
    [void](Test-InstalledPayload $install $build)
    $upgradedRecord = @(Get-UninstallRecord $build.appId)
    Assert-Condition ($upgradedRecord.Count -eq 1 -and (Test-SamePath $upgradedRecord[0].installLocation $install)) 'Remembered-folder upgrade changed installation directory.'
    Test-InstalledShortcut $shortcutPath $install
    if ($testDesktopIcon) { Test-InstalledShortcut $desktopShortcutPath $install }
    Test-Sentinels $sentinels
    $uninstallCommand = [string](@(Get-UninstallRecord $build.appId)[0].uninstallString)
    Assert-Condition ($uninstallCommand -match '^"([^"]+unins\d+\.exe)"$') 'Unexpected test uninstaller command.'
    $uninstaller = [IO.Path]::GetFullPath($Matches[1])
    Assert-Condition (Test-SamePath (Split-Path -Parent $uninstaller) $install) 'Uninstaller is outside the test install directory.'
    Invoke-HiddenInstaller $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCLOSEAPPLICATIONS',
      '/NORESTARTAPPLICATIONS', ('/LOG=' + (Join-Path $logs ($locale + '-uninstall.log'))))
    Test-Sentinels $sentinels
    foreach ($entry in @($payload.files)) {
      Assert-Condition (-not (Test-Path -LiteralPath (Resolve-OwnedFile $install $entry.path) -PathType Leaf)) "Owned file remains after uninstall: $($entry.path)"
    }
    Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $install 'installer-payload.json'))) 'Payload manifest remains after uninstall.'
    Assert-Condition (-not (Test-Path -LiteralPath $shortcutPath)) 'Test Start menu shortcut remains after uninstall.'
    if ($testDesktopIcon) { Assert-Condition (-not (Test-Path -LiteralPath $desktopShortcutPath)) 'Test desktop shortcut remains after uninstall.' }
    Assert-Condition (@(Get-UninstallRecord $build.appId).Count -eq 0) 'Test uninstall registry entry remains.'
    $results.Add([pscustomobject]@{ locale = $locale; installRoot = $install; group = $group
      payloadFilesVerified = @($payload.files).Count; freshInstall = $true; reinstall = $true; uninstall = $true
      lockedUpgradeRejected = $true; lockedUpgradeExitCode = $lockedExitCode; failedUpgradePreservedPayload = $true
      lockedUpgradeFilesystemMutations = $mutationEvidence.MutationCount; lockedUpgradeWatcherErrors = $mutationEvidence.ErrorCount
      rememberedFolderUpgrade = $true; desktopIconTested = $testDesktopIcon
      userDataPreserved = $true; sentinels = $sentinels })
    Write-Output "Installer acceptance passed: $locale (install, locked-upgrade refusal, remembered-folder upgrade, uninstall; user data preserved)."
  }
  $report.passed = $true
} catch {
  $failure = $_
  $report['error'] = $_.Exception.Message
} finally {
  $env:TEMP, $env:TMP = $originalTemp, $originalTmp
  $report.desktopShortcutPreserved = (Get-DesktopShortcutSnapshot) -eq $desktopBefore
  $report.nativeSettingsPreserved = (Get-NativeSettingsFingerprint) -eq $settingsBefore
  if (-not $report.desktopShortcutPreserved -or -not $report.nativeSettingsPreserved) {
    $report.passed = $false
    if (-not $failure) { $failure = 'Production desktop shortcut or native settings changed during acceptance.' }
  }
  $report['finishedAtUtc'] = [DateTime]::UtcNow.ToString('o')
  [IO.File]::WriteAllText((Join-Path $script:runRoot 'acceptance.json'), ($report | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}
Write-Output ('Installer acceptance evidence: ' + (Join-Path $script:runRoot 'acceptance.json'))
if ($failure) { throw $failure }
Assert-Condition $report.passed 'Installer acceptance failed.'
