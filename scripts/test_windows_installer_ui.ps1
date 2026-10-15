#requires -Version 7.2
# GPL-3.0-or-later. Visible wizard checks are permitted only on a CI desktop.
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$SetupPath,
  [Parameter(Mandatory=$true)][string]$TestRoot
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') {
  throw 'Visible installer UI acceptance is remote-GitHub-Actions-only. No window was opened.'
}
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Windows desktop required.' }
$SetupPath = (Resolve-Path -LiteralPath $SetupPath).Path
$metadata = Get-Content -LiteralPath ($SetupPath + '.json') -Raw | ConvertFrom-Json
if ($metadata.schema -ne 'gsw-windows-installer-v1' -or $metadata.testBuild -isnot [bool] -or
    -not $metadata.testBuild -or $metadata.appId -notmatch '^GSW\.Test\.[a-f0-9]{32}$') {
  throw 'Only a unique test-build installer may open on the CI desktop.'
}
if (-not [IO.Path]::GetFullPath([string]$metadata.setupPath).Equals($SetupPath, [StringComparison]::OrdinalIgnoreCase) -or
    (Get-FileHash -LiteralPath $SetupPath -Algorithm SHA256).Hash -ne $metadata.setupSha256 -or
    (Get-Item -LiteralPath $SetupPath).Length -ne [long]$metadata.setupBytes) {
  throw 'Setup and test-build metadata do not match.'
}
if (($metadata.locales -join ',') -ne 'zh_CN,en_US,ja_JP' -or $metadata.automaticAppLaunch -or $metadata.trainingRuntimesBundled) {
  throw 'Unexpected installer behavior contract.'
}
$TestRoot = [IO.Path]::GetFullPath($TestRoot)
if ($TestRoot.TrimEnd('\', '/') -eq [IO.Path]::GetPathRoot($TestRoot).TrimEnd('\', '/')) { throw 'A dedicated TestRoot is required.' }
$cursor = $TestRoot
while ($cursor) {
  if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Reparse paths are not allowed for remote UI evidence.'
  }
  $parent = [IO.Path]::GetDirectoryName($cursor)
  if (-not $parent -or $parent -eq $cursor) { break }
  $cursor = $parent
}
New-Item -ItemType Directory -Path $TestRoot -Force | Out-Null
$runId = [guid]::NewGuid().ToString('N')
$root = Join-Path $TestRoot ('wizard-ui ' + $runId + ' 空白 日本語')
New-Item -ItemType Directory -Path $root | Out-Null
$temp = Join-Path $root 'temp'
New-Item -ItemType Directory -Path $temp | Out-Null
$catalogPath = Join-Path $metadata.payloadRoot 'source/packaging/windows/installer-catalog.json'
$catalog = Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$drawingReferences = @(Get-ChildItem -LiteralPath (Join-Path $PSHOME 'ref') -Filter '*.dll' -File | ForEach-Object FullName) +
  @([Drawing.Bitmap].Assembly.Location, [Drawing.Size].Assembly.Location, [Windows.Forms.TextRenderer].Assembly.Location) +
  @(Get-ChildItem -LiteralPath $PSHOME -Filter 'System.Private.Windows*.dll' -File | ForEach-Object FullName)
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
public static class GswInstallerUiNative {
  public delegate bool EnumProc(IntPtr handle, IntPtr parameter);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public class Control {
    public long Handle {get;set;} public uint ProcessId {get;set;}
    public int ControlId {get;set;}
    public string ClassName {get;set;} public string Text {get;set;}
    public int Left {get;set;} public int Top {get;set;}
    public int Width {get;set;} public int Height {get;set;}
    public bool Enabled {get;set;}
  }
  [StructLayout(LayoutKind.Sequential, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
  public struct LOGFONT {
    public int Height, Width, Escapement, Orientation, Weight;
    public byte Italic, Underline, StrikeOut, CharSet, OutPrecision, ClipPrecision, Quality, PitchAndFamily;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string FaceName;
  }
  public class CaptionMeasurement {
    public int Width {get;set;} public int Height {get;set;}
    public int ClientWidth {get;set;} public int ClientHeight {get;set;}
    public string FontName {get;set;} public int FontPixelHeight {get;set;}
    public int FontWeight {get;set;} public int FontCharSet {get;set;}
    public long FontHandle {get;set;} public uint WindowStyle {get;set;}
    public uint DrawTextFlags {get;set;} public bool WordWrap {get;set;}
    public bool FitsHeight {get;set;} public bool FitsWidth {get;set;}
  }
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr parameter);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr handle);
  [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr handle);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr handle, out RECT rect);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr handle, out RECT rect);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr handle);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr handle, IntPtr dc);
  [DllImport("user32.dll", EntryPoint="GetWindowLongW")] static extern uint GetWindowStyle(IntPtr handle, int index);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int DrawText(IntPtr dc, string text, int count, ref RECT rect, uint flags);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
  [DllImport("gdi32.dll")] static extern IntPtr GetCurrentObject(IntPtr dc, uint type);
  [DllImport("gdi32.dll", CharSet=CharSet.Unicode, EntryPoint="GetObjectW")] static extern int GetFontObject(IntPtr obj, int size, out LOGFONT font);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr handle, StringBuilder text, int count);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextLength(IntPtr handle);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr handle, StringBuilder text, int count);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr handle, out uint processId);
  [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr handle);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr handle, IntPtr device, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr handle);
  [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr handle, uint message, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr handle, uint message, IntPtr wParam, IntPtr lParam);
  static Control Read(IntPtr handle) {
    RECT rect; GetWindowRect(handle, out rect);
    var text = new StringBuilder(Math.Max(1, GetWindowTextLength(handle) + 1));
    var className = new StringBuilder(256);
    GetWindowText(handle, text, text.Capacity); GetClassName(handle, className, className.Capacity);
    uint processId; GetWindowThreadProcessId(handle, out processId);
    return new Control {Handle=handle.ToInt64(), ProcessId=processId, ControlId=GetDlgCtrlID(handle), Text=text.ToString(), ClassName=className.ToString(),
      Left=rect.Left, Top=rect.Top, Width=rect.Right-rect.Left, Height=rect.Bottom-rect.Top, Enabled=IsWindowEnabled(handle)};
  }
  public static Control[] Windows() {
    var controls = new List<Control>();
    EnumWindows((handle, parameter) => {if(IsWindowVisible(handle)) controls.Add(Read(handle)); return true;}, IntPtr.Zero);
    return controls.ToArray();
  }
  public static Control[] Children(long parent) {
    var controls = new List<Control>();
    EnumChildWindows(new IntPtr(parent), (handle, parameter) => {if(IsWindowVisible(handle)) controls.Add(Read(handle)); return true;}, IntPtr.Zero);
    return controls.ToArray();
  }
  public static void Click(long handle) {
    // Asynchronous delivery is essential: Cancel opens a modal confirmation,
    // and synchronous SendMessage would deadlock the acceptance controller.
    if(!PostMessage(new IntPtr(handle), 0x00F5, IntPtr.Zero, IntPtr.Zero))
      throw new InvalidOperationException("Unable to post test-owned button click.");
  }
  public static bool Capture(long handle, string path) {
    RECT rect; if(!GetWindowRect(new IntPtr(handle), out rect)) return false;
    if(rect.Right<=rect.Left || rect.Bottom<=rect.Top) return false;
    using(var image = new Bitmap(rect.Right-rect.Left, rect.Bottom-rect.Top, PixelFormat.Format32bppArgb)) {
      using(var graphics = Graphics.FromImage(image)) {
        IntPtr device = graphics.GetHdc();
        try { if(!PrintWindow(new IntPtr(handle), device, 2) && !PrintWindow(new IntPtr(handle), device, 0)) return false; }
        finally { graphics.ReleaseHdc(device); }
      }
      var colors = new HashSet<int>();
      for(int y=0; y<image.Height; y+=Math.Max(1,image.Height/40))
        for(int x=0; x<image.Width; x+=Math.Max(1,image.Width/60)) colors.Add(image.GetPixel(x,y).ToArgb());
      if(colors.Count < 5) return false;
      image.Save(path, ImageFormat.Png); return true;
    }
  }
  public static CaptionMeasurement MeasureCaption(Control control) {
    IntPtr handle = new IntPtr(control.Handle);
    RECT client;
    if(!GetClientRect(handle, out client)) throw new InvalidOperationException("Cannot read caption client bounds.");
    var result = new CaptionMeasurement {ClientWidth=client.Right-client.Left, ClientHeight=client.Bottom-client.Top};
    result.WindowStyle = GetWindowStyle(handle, -16);
    bool isStatic = control.ClassName.EndsWith("StaticText", StringComparison.Ordinal) || control.ClassName == "Static";
    uint staticType = result.WindowStyle & 0x1F;
    result.WordWrap = isStatic ? staticType != 0x0C && staticType != 0x0B : (result.WindowStyle & 0x2000) != 0;
    // Match Inno TNewStaticText.CalcBounds: original HFONT, GDI DrawText and
    // the control's wrapping/prefix style. Converting HFONT to a GDI+ Font and
    // using TextRenderer adds different metrics/padding (notably for CJK).
    uint flags = 0x0400 | 0x0040; // DT_CALCRECT | DT_EXPANDTABS
    if(result.WordWrap) flags |= 0x0010; // DT_WORDBREAK
    else if(!isStatic) flags |= 0x0020; // DT_SINGLELINE for ordinary buttons
    if(isStatic && (result.WindowStyle & 0x0080) != 0) flags |= 0x0800; // SS_NOPREFIX -> DT_NOPREFIX
    if((GetWindowStyle(handle, -20) & 0x2000) != 0) flags |= 0x20000; // WS_EX_RTLREADING -> DT_RTLREADING
    result.DrawTextFlags = flags;
    IntPtr dc = GetDC(handle);
    if(dc == IntPtr.Zero) throw new InvalidOperationException("Cannot acquire caption device context.");
    IntPtr previousFont = IntPtr.Zero;
    try {
      IntPtr font = SendMessage(handle, 0x0031, IntPtr.Zero, IntPtr.Zero); // WM_GETFONT
      if(font != IntPtr.Zero) previousFont = SelectObject(dc, font);
      else font = GetCurrentObject(dc, 6); // OBJ_FONT: native default, not a substitute GDI+ font
      result.FontHandle = font.ToInt64();
      LOGFONT description;
      if(GetFontObject(font, Marshal.SizeOf<LOGFONT>(), out description) == 0)
        throw new InvalidOperationException("Cannot read the actual caption HFONT.");
      result.FontName=description.FaceName; result.FontPixelHeight=description.Height;
      result.FontWeight=description.Weight; result.FontCharSet=description.CharSet;
      RECT measured = new RECT {Right=result.ClientWidth};
      if(!String.IsNullOrWhiteSpace(control.Text) && DrawText(dc, control.Text, control.Text.Length, ref measured, flags) == 0)
        throw new InvalidOperationException("Native caption measurement failed.");
      result.Width=measured.Right-measured.Left; result.Height=measured.Bottom-measured.Top;
      result.FitsHeight=result.Height <= result.ClientHeight;
      result.FitsWidth=result.Width <= result.ClientWidth;
      return result;
    } finally {
      if(previousFont != IntPtr.Zero) SelectObject(dc, previousFont);
      ReleaseDC(handle, dc);
    }
  }
}
'@ -ReferencedAssemblies $drawingReferences

function Get-OwnedWindows([int]$RootProcessId) {
  $processes = @(Get-CimInstance Win32_Process)
  $ids = [Collections.Generic.HashSet[uint32]]::new()
  [void]$ids.Add([uint32]$RootProcessId)
  do {
    $changed = $false
    foreach ($process in $processes) {
      if ($ids.Contains([uint32]$process.ParentProcessId) -and $ids.Add([uint32]$process.ProcessId)) { $changed = $true }
    }
  } while ($changed)
  return @([GswInstallerUiNative]::Windows() | Where-Object { $ids.Contains($_.ProcessId) })
}

function Wait-Wizard([int]$ProcessId) {
  $deadline = [DateTime]::UtcNow.AddSeconds(30)
  do {
    $wizard = @(Get-OwnedWindows $ProcessId | Where-Object { $_.ClassName -eq 'TWizardForm' })
    if ($wizard.Count -eq 1) { return $wizard[0] }
    Start-Sleep -Milliseconds 200
  } while ([DateTime]::UtcNow -lt $deadline)
  throw 'No unique test-owned Setup wizard appeared.'
}

function Get-Button([object]$Wizard, [string]$Pattern, [bool]$RequireEnabled=$true) {
  $deadline = [DateTime]::UtcNow.AddSeconds(5)
  do {
    $buttons = @([GswInstallerUiNative]::Children($Wizard.Handle) | Where-Object {
      $_.ClassName -match 'Button$' -and $_.Text.Replace('&', '') -match $Pattern -and (-not $RequireEnabled -or $_.Enabled)
    })
    if ($buttons.Count -eq 1) { return $buttons[0] }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  if ($buttons.Count -ne 1) { throw "Expected one wizard button: $Pattern; found $($buttons.Count)." }
}

function Capture-Page([object]$Wizard, [string]$Locale, [string]$Page) {
  Start-Sleep -Milliseconds 250
  [void][GswInstallerUiNative]::SetForegroundWindow([IntPtr]$Wizard.Handle)
  $controls = @([GswInstallerUiNative]::Children($Wizard.Handle))
  $picture = Join-Path $root ($Locale + '-' + $Page + '.png')
  if (-not [GswInstallerUiNative]::Capture($Wizard.Handle, $picture)) { throw 'Window capture is blank or failed.' }
  $automation = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$Wizard.Handle)
  $uia = @()
  foreach ($element in $automation.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
    try { $uia += [pscustomobject]@{ name=$element.Current.Name; type=$element.Current.ControlType.ProgrammaticName; enabled=$element.Current.IsEnabled; offscreen=$element.Current.IsOffscreen } } catch { }
  }
  $measured = @()
  $captions = @($controls | Where-Object { $_.ClassName -match 'StaticText$|Button$' -and $_.Text.Trim() })
  if (-not $captions.Count) { throw "No visible nonempty wizard captions were found ($Locale/$Page)." }
  foreach ($control in $captions) {
    $measurement = $null
    $measurementError = $null
    try { $measurement = [GswInstallerUiNative]::MeasureCaption($control) } catch { $measurementError = $_.Exception.Message }
    $measured += [pscustomobject]@{ text=$control.Text; className=$control.ClassName; width=$control.Width; height=$control.Height
      authority='diagnostic-only'; captionReview='manual-required'; nativeMeasurement=$measurement; measurementError=$measurementError }
  }
  # VCL's TNewStaticTextStyleHook can draw through the style service rather
  # than WM_GETFONT. These external metrics are not an authoritative clipping
  # verdict. Real page screenshots require a separately recorded human review.
  $record = [ordered]@{ locale=$Locale; page=$Page; screenshot=$picture; window=$Wizard; controls=$controls; automation=$uia
    captionReview='manual-required'; captionMeasurementAuthority='diagnostic-only'; measuredCaptions=$measured }
  $evidencePath = Join-Path $root ($Locale + '-' + $Page + '.json')
  [IO.File]::WriteAllText($evidencePath, ($record | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
  $report.captionEvidence += [pscustomobject]@{locale=$Locale;page=$Page;screenshot=$picture;controlsJson=$evidencePath;captionReview='manual-required'}
  return $controls
}

$patterns = @{
  zh_CN = @{ next='下一步'; back='上一步'; cancel='取消'; accept='我同意|我接受' }
  en_US = @{ next='Next'; back='Back'; cancel='Cancel'; accept='I accept' }
  ja_JP = @{ next='次へ'; back='戻る'; cancel='キャンセル'; accept='同意する' }
}
$oldTemp, $oldTmp = $env:TEMP, $env:TMP
$report = [ordered]@{ schema='gsw-installer-ui-acceptance-v2'; setupSha256=$metadata.setupSha256; appId=$metadata.appId
  runRoot=$root; passed=$false; fullUiAccepted=$false; navigationAndLanguagePassed=$false
  captionReview='manual-required'; manualCaptionReviewRequired=$true; automaticCaptionClippingVerified=$false; captionEvidence=@()
  applicationLaunched=$false; installationPerformed=$false; locales=@(); cancelConfirmations=@() }
$failure = $null
$process = $null
try {
  $env:TEMP, $env:TMP = $temp, $temp
  foreach ($locale in @('zh_CN', 'en_US', 'ja_JP')) {
    $directory = Join-Path $root ($locale + ' uninstalled 程序')
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $SetupPath
    $start.UseShellExecute = $false
    $start.WorkingDirectory = $root
    foreach ($argument in @('/SP-', '/NORESTART', '/NOCLOSEAPPLICATIONS', '/NORESTARTAPPLICATIONS',
        '/TASKS=!desktopicon', ('/LANG=' + $locale), ('/DIR=' + $directory),
        ('/GROUP=GSW Installer UI Acceptance ' + $runId + ' ' + $locale), ('/LOG=' + (Join-Path $root ($locale + '-wizard.log'))))) {
      $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    $wizard = Wait-Wizard $process.Id
    $pattern = $patterns[$locale]
    [void](Capture-Page $wizard $locale 'welcome')
    [void](Get-Button $wizard $pattern.cancel)
    [GswInstallerUiNative]::Click((Get-Button $wizard $pattern.next).Handle)
    [void](Capture-Page $wizard $locale 'license')
    [void](Get-Button $wizard $pattern.back)
    $accept = @([GswInstallerUiNative]::Children($wizard.Handle) | Where-Object { $_.ClassName -match 'RadioButton$' -and $_.Text.Replace('&','') -match $pattern.accept })
    if ($accept.Count -ne 1) { throw 'Translated license acceptance option is missing.' }
    [GswInstallerUiNative]::Click($accept[0].Handle)
    [GswInstallerUiNative]::Click((Get-Button $wizard $pattern.next).Handle)
    $runtime = @(Capture-Page $wizard $locale 'runtime')
    $runtimeText = ($runtime | ForEach-Object Text) -join "`n"
    $title = [string]$catalog.PSObject.Properties['安装内容与训练环境'].Value.PSObject.Properties[$locale].Value
    if (-not $runtimeText.Contains($title) -or $runtimeText -notmatch 'CUDA' -or $runtimeText -notmatch 'PyTorch' -or
        $runtimeText -notmatch 'Python/Conda' -or $runtimeText -notmatch 'COLMAP') { throw 'Localized training-runtime explanation is missing.' }
    [GswInstallerUiNative]::Click((Get-Button $wizard $pattern.next).Handle)
    $destination = @(Capture-Page $wizard $locale 'directory')
    $edit = @($destination | Where-Object { $_.ClassName -match 'Edit$' -and $_.Enabled -and $_.Text -eq $directory })
    if ($edit.Count -ne 1) { throw 'Installation directory is not visible and editable.' }
    [void](Get-Button $wizard $pattern.next)
    [void](Get-Button $wizard $pattern.back)
    [GswInstallerUiNative]::Click((Get-Button $wizard $pattern.back).Handle)
    $back = @(Capture-Page $wizard $locale 'runtime-back')
    if ((($back | ForEach-Object Text) -join "`n") -notmatch [regex]::Escape($title)) { throw 'Back navigation did not restore runtime explanation.' }
    [GswInstallerUiNative]::Click((Get-Button $wizard $pattern.next).Handle)
    [GswInstallerUiNative]::Click((Get-Button $wizard $pattern.cancel).Handle)
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $confirmed = $false
    do {
      $dialogs = @(Get-OwnedWindows $process.Id | Where-Object { $_.Handle -ne $wizard.Handle })
      $confirmations = @(foreach ($dialog in $dialogs) {
        $controls = @([GswInstallerUiNative]::Children($dialog.Handle))
        # Inno's system-style ExitSetupMsgBox uses native MessageBox buttons.
        # Their captions follow Windows, not /LANG. IDYES is always 6.
        $yes = @($controls | Where-Object { $_.ClassName -match 'Button$' -and $_.ControlId -eq 6 -and $_.Enabled })
        $selection = 'Win32-IDYES-6'
        if ($yes.Count -eq 1) { [pscustomobject]@{ dialog=$dialog; button=$yes[0]; controls=$controls; selection=$selection } }
      })
      if ($confirmations.Count -eq 1) {
        $confirmation = $confirmations[0]
        $evidence = [ordered]@{locale=$locale;selection=$confirmation.selection;window=$confirmation.dialog
          selectedButton=$confirmation.button;controls=$confirmation.controls}
        [IO.File]::WriteAllText((Join-Path $root ($locale + '-cancel-selection.json')), ($evidence | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
        [void](Capture-Page $confirmation.dialog $locale 'cancel-confirmation')
        [GswInstallerUiNative]::Click($confirmation.button.Handle)
        $report.cancelConfirmations += [pscustomobject]@{locale=$locale;selection=$confirmation.selection
          controlId=$confirmation.button.ControlId;caption=$confirmation.button.Text}
        $confirmed = $true
        break
      }
      if ($confirmations.Count -gt 1) { throw 'More than one test-owned cancellation confirmation was found; no button was pressed.' }
      if ($process.HasExited) { break }
      Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $confirmed -and -not $process.HasExited) { throw 'No unique test-owned IDYES cancellation confirmation appeared; no force termination was attempted.' }
    if (-not $process.WaitForExit(10000)) { throw 'Wizard did not cancel cleanly; no force termination was attempted.' }
    if (Test-Path -LiteralPath (Join-Path $directory 'bin/Gaussian Scene Workbench.exe')) { throw 'UI-only acceptance unexpectedly installed the program.' }
    $report.locales += [pscustomobject]@{ locale=$locale; welcome=$true; license=$true; runtime=$true; directory=$true; back=$true; cancel=$true; exitCode=$process.ExitCode }
    $process.Dispose()
    $process = $null
    Write-Output "Remote installer navigation/language checks passed: $locale; page captions require screenshot review."
  }
  $report.navigationAndLanguagePassed = $true
} catch {
  $failure = $_
  $report['error'] = $_.Exception.Message
  $report['errorDetail'] = $_.ToString()
  $report['scriptStackTrace'] = $_.ScriptStackTrace
  # Failure diagnostics are best-effort and must never replace the first error.
  if ($process) {
    try {
      $windows = @(Get-OwnedWindows $process.Id)
      $state = @(foreach ($window in $windows) {
        [pscustomobject]@{window=$window;controls=@([GswInstallerUiNative]::Children($window.Handle))}
      })
      [IO.File]::WriteAllText((Join-Path $root 'failure-windows.json'), ($state | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
    } catch { $report['diagnosticsError'] = $_.Exception.Message }
  }
} finally {
  $env:TEMP, $env:TMP = $oldTemp, $oldTmp
  $report['finishedAtUtc'] = [DateTime]::UtcNow.ToString('o')
  try {
    [IO.File]::WriteAllText((Join-Path $root 'wizard-acceptance.json'), ($report | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
  } catch {
    if (-not $failure) { $failure = $_ } else { [Console]::Error.WriteLine('Wizard evidence write also failed: ' + $_.Exception.Message) }
  }
}
Write-Output ('Remote wizard evidence: ' + (Join-Path $root 'wizard-acceptance.json'))
Write-Output 'Full UI acceptance remains pending: review every listed screenshot and record manual-review.json separately.'
if ($failure) { throw $failure }
