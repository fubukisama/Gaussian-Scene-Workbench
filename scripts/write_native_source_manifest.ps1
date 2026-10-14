[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageRoot)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PackageRoot)
$source = Join-Path $root 'source'
$executable = Join-Path $root 'bin/Gaussian Scene Workbench.exe'
foreach ($required in @($executable, (Join-Path $source 'native/CMakeLists.txt'),
    (Join-Path $source 'native/src/shaders/SurfelProjection.glsl'),
    (Join-Path $source 'native/third_party/blender-gizmo/BlenderGizmoGeometry.cpp'),
    (Join-Path $root 'COPYING'), (Join-Path $root 'native/LICENSE.md'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Corresponding-source package file is missing: $required"
    }
}
$files = @(Get-ChildItem -LiteralPath $source -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($source.Length + 1).Replace('\','/')
        size = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$manifest = [ordered]@{
    schema = 'gsw-native-corresponding-source-v1'
    executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    license = 'GPL-3.0-or-later'
    sourceFileCount = $files.Count
    files = $files
}
$json = $manifest | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText((Join-Path $root 'source-manifest.json'), $json, [Text.UTF8Encoding]::new($false))
Write-Output "Corresponding source: $($files.Count) files; executable $($manifest.executableSha256)"
