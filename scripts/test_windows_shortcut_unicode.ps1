#requires -Version 7.2
# SPDX-License-Identifier: GPL-3.0-or-later
# Non-GUI link-reading regression. Never installs or launches any executable.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$TestRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Windows Shell COM is required.' }
$TestRoot=[IO.Path]::GetFullPath($TestRoot)
$cursor=$TestRoot
while($cursor) {
  if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Shortcut fixture cannot be placed through a reparse point.'
  }
  $parent=[IO.Path]::GetDirectoryName($cursor)
  if(-not $parent -or $parent -eq $cursor){break}
  $cursor=$parent
}
New-Item -ItemType Directory -Path $TestRoot -Force|Out-Null
$root=Join-Path $TestRoot ('shortcut-unicode-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root|Out-Null
$tokens=$null
$parseErrors=$null
$source=Join-Path $PSScriptRoot 'test_windows_installer.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Installer acceptance script cannot be parsed.'}
$definitions=@($ast.FindAll({param($node)
  $node -is [Management.Automation.Language.StringConstantExpressionAst] -and
  ($node.Value.Contains('public static class GswInstallerShellLinkUnicode') -or
   $node.Value.Contains('public static class GswInstallerPathProbe'))
},$true))
if($definitions.Count -ne 2){throw 'Shared Unicode Shell-link and physical-path probe definitions are required.'}
foreach($definition in $definitions) {
  $name=if($definition.Value.Contains('public static class GswInstallerShellLinkUnicode')){'GswInstallerShellLinkUnicode'}else{'GswInstallerPathProbe'}
  if(-not ($name -as [type])){Add-Type -TypeDefinition $definition.Value}
}
$functionNames=@('Assert-Condition','Get-Sha256','Test-SamePath','Get-ShortcutPathEvidence','Test-InstalledShortcut')
foreach($name in $functionNames) {
  $definition=@($ast.FindAll({param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
  },$true))
  if($definition.Count -ne 1){throw "One shared acceptance function is required: $name"}
  . ([scriptblock]::Create($definition[0].Extent.Text))
}
$script:runRoot=$root
$directory=Join-Path $root 'Café 空白 日本語 程序 安装 🧪 한글'
New-Item -ItemType Directory -Path (Join-Path $directory 'bin')|Out-Null
$target=Join-Path $directory 'bin/Gaussian Scene Workbench.exe'
[IO.File]::WriteAllText($target,'GSW controlled non-executable shortcut target; never launch',[Text.UTF8Encoding]::new($false))
$shortcutPath=Join-Path $root 'owned-unicode-fixture.lnk'
[GswInstallerShellLinkUnicode]::CreateOwnedFixture($shortcutPath,$target,$directory)
$before=(Get-FileHash -LiteralPath $shortcutPath -Algorithm SHA256).Hash
$native=[GswInstallerShellLinkUnicode]::Read($shortcutPath)
$shell=New-Object -ComObject WScript.Shell
try {
  $legacy=$shell.CreateShortcut($shortcutPath)
  $legacyTarget=[string]$legacy.TargetPath
  $legacyDirectory=[string]$legacy.WorkingDirectory
} finally {[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)}
$after=(Get-FileHash -LiteralPath $shortcutPath -Algorithm SHA256).Hash
Test-InstalledShortcut $shortcutPath $directory
$wrongTarget=Join-Path $directory 'bin/Wrong non-executable target.exe'
[IO.File]::WriteAllText($wrongTarget,'Different non-executable fixture target; never launch',[Text.UTF8Encoding]::new($false))
$wrongShortcut=Join-Path $root 'owned-wrong-target.lnk'
[GswInstallerShellLinkUnicode]::CreateOwnedFixture($wrongShortcut,$wrongTarget,$directory)
$wrongTargetRejected=$false
try{Test-InstalledShortcut $wrongShortcut $directory}
catch{if($_.Exception.Message -notlike 'Shortcut target is not the installed bin executable.*'){throw};$wrongTargetRejected=$true}
$wrongDirectoryShortcut=Join-Path $root 'owned-wrong-working-directory.lnk'
[GswInstallerShellLinkUnicode]::CreateOwnedFixture($wrongDirectoryShortcut,$target,$root)
$wrongDirectoryRejected=$false
try{Test-InstalledShortcut $wrongDirectoryShortcut $directory}
catch{if($_.Exception.Message -notlike 'Shortcut working directory mismatch.*'){throw};$wrongDirectoryRejected=$true}
$record=[ordered]@{schema='gsw-shortcut-unicode-regression-v1';fixtureRoot=$root;shortcutPath=$shortcutPath
  expectedTargetPath=$target;expectedWorkingDirectory=$directory;nativeTargetPath=$native.TargetPath
  nativeWorkingDirectory=$native.WorkingDirectory;wscriptTargetPath=$legacyTarget;wscriptWorkingDirectory=$legacyDirectory
  wscriptTargetMatches=$legacyTarget.Equals($target,[StringComparison]::OrdinalIgnoreCase)
  wscriptDirectoryMatches=$legacyDirectory.Equals($directory,[StringComparison]::OrdinalIgnoreCase)
  nativeTargetMatches=$native.TargetPath.Equals($target,[StringComparison]::OrdinalIgnoreCase)
  nativeDirectoryMatches=$native.WorkingDirectory.Equals($directory,[StringComparison]::OrdinalIgnoreCase)
  wrongTargetRejected=$wrongTargetRejected
  wrongWorkingDirectoryRejected=$wrongDirectoryRejected
  linkUnchanged=$before -eq $after;shortcutSha256=$after;applicationLaunched=$false
  passed=$false}
$record.passed=$record.nativeTargetMatches -and $record.nativeDirectoryMatches -and $record.linkUnchanged -and $wrongTargetRejected -and $wrongDirectoryRejected
$evidence=Join-Path $root 'unicode-shortcut-evidence.json'
[IO.File]::WriteAllText($evidence,($record|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
Write-Output ('Unicode shortcut evidence: '+$evidence)
$record|ConvertTo-Json -Compress
if(-not $record.passed){throw 'Unicode Shell-link reader did not preserve exact target/directory and original link bytes.'}
