<#
.SYNOPSIS
Provides a real, sufficiently wide Windows desktop for native UI CI tests.
.DESCRIPTION
Only GitHub Actions may change the display mode. Local callers must use
-InspectOnly, which enumerates modes without calling ChangeDisplaySettingsExW.
No registry overrides or display drivers are installed. The selected mode must
be returned by EnumDisplaySettingsW and pass CDS_TEST before it is applied.

Win32 references:
https://learn.microsoft.com/windows/win32/api/wingdi/ns-wingdi-devmodew
https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-enumdisplaysettingsw
https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-changedisplaysettingsexw
#>
[CmdletBinding()]
param([switch]$InspectOnly)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($env:OS -ne 'Windows_NT') {
    throw 'Native CI display setup requires Windows.'
}
if (-not $InspectOnly -and $env:GITHUB_ACTIONS -cne 'true') {
    throw 'Display changes are restricted to GITHUB_ACTIONS=true. Use -InspectOnly for local diagnostics.'
}

if (-not ('Gsw.NativeCiDisplay' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Gsw {
    public static class NativeCiDisplay {
        // The display arm of DEVMODEW's 16-byte union is expanded below.
        // All fields are fixed-width Win32 types; no pointer-sized members occur.
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct Mode {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string dmDeviceName;
            public ushort dmSpecVersion;
            public ushort dmDriverVersion;
            public ushort dmSize;
            public ushort dmDriverExtra;
            public uint dmFields;
            public int dmPositionX;
            public int dmPositionY;
            public uint dmDisplayOrientation;
            public uint dmDisplayFixedOutput;
            public short dmColor;
            public short dmDuplex;
            public short dmYResolution;
            public short dmTTOption;
            public short dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string dmFormName;
            public ushort dmLogPixels;
            public uint dmBitsPerPel;
            public uint dmPelsWidth;
            public uint dmPelsHeight;
            public uint dmDisplayFlags;
            public uint dmDisplayFrequency;
            public uint dmICMMethod;
            public uint dmICMIntent;
            public uint dmMediaType;
            public uint dmDitherType;
            public uint dmReserved1;
            public uint dmReserved2;
            public uint dmPanningWidth;
            public uint dmPanningHeight;
        }

        [DllImport("user32.dll", EntryPoint = "EnumDisplaySettingsW",
            CharSet = CharSet.Unicode, ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumDisplaySettings(
            string deviceName, uint modeNumber, ref Mode mode);

        [DllImport("user32.dll", EntryPoint = "ChangeDisplaySettingsExW",
            CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern int ChangeDisplaySettingsEx(
            string deviceName, ref Mode mode, IntPtr window, uint flags, IntPtr parameter);

        public static int StructureSize { get { return Marshal.SizeOf(typeof(Mode)); } }

        private static Mode NewMode() {
            if (StructureSize != 220)
                throw new InvalidOperationException("DEVMODEW ABI mismatch: expected 220 bytes, got " + StructureSize);
            return new Mode { dmSize = 220, dmDriverExtra = 0,
                dmDeviceName = String.Empty, dmFormName = String.Empty };
        }

        public static Mode CurrentMode() {
            Mode mode = NewMode();
            if (!EnumDisplaySettings(null, UInt32.MaxValue, ref mode))
                throw new InvalidOperationException("EnumDisplaySettingsW could not read the current primary display mode.");
            return mode;
        }

        public static Mode[] SupportedModes() {
            var modes = new List<Mode>();
            for (uint index = 0; ; ++index) {
                Mode mode = NewMode();
                if (!EnumDisplaySettings(null, index, ref mode)) break;
                if (mode.dmBitsPerPel == 32) modes.Add(mode);
            }
            return modes.ToArray();
        }

        public static int TestMode(Mode mode) {
            if (mode.dmDriverExtra != 0)
                throw new InvalidOperationException("Unexpected private display-mode data; refusing a truncated DEVMODEW.");
            return ChangeDisplaySettingsEx(null, ref mode, IntPtr.Zero, 2, IntPtr.Zero); // CDS_TEST
        }

        public static int ApplyMode(Mode mode) {
            // Defense in depth: invoking the public method directly cannot
            // bypass the script's local-display guard.
            if (Environment.GetEnvironmentVariable("GITHUB_ACTIONS") != "true")
                throw new InvalidOperationException("Display changes are restricted to GitHub Actions.");
            if (mode.dmDriverExtra != 0)
                throw new InvalidOperationException("Unexpected private display-mode data; refusing a truncated DEVMODEW.");
            // flags=0 applies dynamically; it does NOT use CDS_UPDATEREGISTRY.
            return ChangeDisplaySettingsEx(null, ref mode, IntPtr.Zero, 0, IntPtr.Zero);
        }
    }
}
'@
}

function Format-DisplayMode($Mode) {
    '{0}x{1}, {2} bpp, {3} Hz' -f $Mode.dmPelsWidth, $Mode.dmPelsHeight,
        $Mode.dmBitsPerPel, $Mode.dmDisplayFrequency
}

function Test-SufficientDisplay($Mode) {
    $Mode.dmBitsPerPel -eq 32 -and $Mode.dmPelsWidth -ge 1600 -and $Mode.dmPelsHeight -ge 900
}

function Format-DisplayResult([int]$Result) {
    $names = @{ 0 = 'SUCCESSFUL'; 1 = 'RESTART'; -1 = 'FAILED'; -2 = 'BADMODE';
        -3 = 'NOTUPDATED'; -4 = 'BADFLAGS'; -5 = 'BADPARAM'; -6 = 'BADDUALVIEW' }
    if ($names.ContainsKey($Result)) { 'DISP_CHANGE_{0} ({1})' -f $names[$Result], $Result }
    else { 'Unknown display-change result ({0})' -f $Result }
}

$initial = [Gsw.NativeCiDisplay]::CurrentMode()
Write-Host ('Native CI display: DEVMODEW={0} bytes; initial={1}' -f
    [Gsw.NativeCiDisplay]::StructureSize, (Format-DisplayMode $initial))
$modes = @([Gsw.NativeCiDisplay]::SupportedModes())
if ($modes.Count -eq 0) {
    throw 'Native CI display: no supported 32-bpp modes were enumerated.'
}
$descriptions = @($modes | ForEach-Object { Format-DisplayMode $_ } | Sort-Object -Unique)
foreach ($description in $descriptions) { Write-Host "Supported display mode: $description" }

if ($InspectOnly) {
    Write-Host ('Native CI display: inspection only; unchanged={0}; meets1600x900={1}' -f
        (Format-DisplayMode ([Gsw.NativeCiDisplay]::CurrentMode())), (Test-SufficientDisplay $initial))
    return
}

if (-not (Test-SufficientDisplay $initial)) {
    # Prefer the ordinary FHD fixture. Use 1600x900 next, then the smallest
    # supported larger mode; do not fabricate or force unsupported settings.
    $candidates = @($modes | Where-Object { Test-SufficientDisplay $_ } | Sort-Object -Property @(
        @{ Expression = {
            if ($_.dmPelsWidth -eq 1920 -and $_.dmPelsHeight -eq 1080) { 0 }
            elseif ($_.dmPelsWidth -eq 1600 -and $_.dmPelsHeight -eq 900) { 1 }
            else { 2 }
        } },
        @{ Expression = { [long]$_.dmPelsWidth * $_.dmPelsHeight } },
        @{ Expression = { [Math]::Abs([long]$_.dmDisplayFrequency - 60) } }
    ))
    $applied = $false
    foreach ($candidate in $candidates) {
        $description = Format-DisplayMode $candidate
        $testResult = [Gsw.NativeCiDisplay]::TestMode($candidate)
        Write-Host ('Native CI display: CDS_TEST {0}: {1}' -f $description, (Format-DisplayResult $testResult))
        if ($testResult -ne 0) { continue }
        $applyResult = [Gsw.NativeCiDisplay]::ApplyMode($candidate)
        Write-Host ('Native CI display: apply {0}: {1}' -f $description, (Format-DisplayResult $applyResult))
        if ($applyResult -ne 0) { continue }
        # Wait for the real WM_DISPLAYCHANGE transition, not a guessed delay.
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        do {
            $actual = [Gsw.NativeCiDisplay]::CurrentMode()
            if (Test-SufficientDisplay $actual) { $applied = $true; break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($applied) { break }
    }
    if (-not $applied) {
        $actual = [Gsw.NativeCiDisplay]::CurrentMode()
        throw ('Native CI display bootstrap failed: actual={0}; a supported 32-bpp mode of at least 1600x900 is required for the 1400-pixel native UI matrix. No tests will be skipped.' -f
            (Format-DisplayMode $actual))
    }
}

$final = [Gsw.NativeCiDisplay]::CurrentMode()
if (-not (Test-SufficientDisplay $final)) {
    throw ('Native CI display validation failed: actual={0}; required=1600x900 at 32 bpp.' -f (Format-DisplayMode $final))
}
Write-Host ('Native CI display ready: final={0}; unchanged={1}. Resolution values are physical pixels; CI Qt tests must use 100% backing scale.' -f
    (Format-DisplayMode $final), ((Format-DisplayMode $initial) -eq (Format-DisplayMode $final)))
