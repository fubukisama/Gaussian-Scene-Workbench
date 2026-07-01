$ErrorActionPreference = "Continue"

$Miniforge = Join-Path $env:USERPROFILE "miniforge3"
$ColmapBin = Join-Path $env:USERPROFILE "Downloads\colmap-x64-windows-cuda\bin"
if (Test-Path $Miniforge) {
    $env:Path = "$Miniforge;$Miniforge\Scripts;$Miniforge\condabin;$env:Path"
}
if (Test-Path $ColmapBin) {
    $env:Path = "$ColmapBin;$env:Path"
}

function Test-Tool {
    param(
        [string]$Name,
        [string]$Command
    )

    Write-Host ""
    Write-Host "== $Name =="
    try {
        Invoke-Expression $Command
    } catch {
        Write-Host "Missing or not available in PATH"
    }
}

Test-Tool "NVIDIA GPU driver" "nvidia-smi"
Test-Tool "Python" "python --version"
Test-Tool "Conda" "conda --version"
Test-Tool "Git" "git --version"
Test-Tool "COLMAP" "colmap -h"
Test-Tool "CUDA compiler" "nvcc --version"

Write-Host ""
Write-Host "== Visual Studio C++ compiler =="
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl) {
    Write-Host $cl.Source
} else {
    $VsWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $VsPath = & $VsWhere -products * -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath
        if ($VsPath) {
            Write-Host "Installed at $VsPath"
            Write-Host "Use scripts\open_3dgs_shell.bat before building 3DGS."
        } else {
            Write-Host "Missing Visual Studio C++ workload"
        }
    } else {
        Write-Host "Missing or not available in PATH"
    }
}

Write-Host ""
Write-Host "If a tool is installed but still shown as missing, open a fresh PowerShell window or add it to PATH."
