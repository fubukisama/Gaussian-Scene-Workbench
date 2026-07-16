[CmdletBinding(SupportsShouldProcess = $true)]
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [ValidateNotNullOrEmpty()]
  [string[]]$Path,

  [Parameter(Mandatory = $true)]
  [ValidateNotNullOrEmpty()]
  [string]$CertificateThumbprint,

  [ValidateSet("CurrentUser", "LocalMachine")]
  [string]$CertificateStoreLocation = "CurrentUser",

  [string]$TimestampUrl = "",
  [string]$SignToolPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

function Resolve-MicrosoftSignTool {
  param([string]$RequestedPath)

  $Candidates = [System.Collections.Generic.List[string]]::new()
  if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
    $Candidates.Add($RequestedPath)
  } else {
    $PathCommand = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($PathCommand) {
      $Candidates.Add($PathCommand.Source)
    }

    if ($env:WindowsSdkVerBinPath) {
      $Candidates.Add((Join-Path $env:WindowsSdkVerBinPath "x64\signtool.exe"))
      $Candidates.Add((Join-Path $env:WindowsSdkVerBinPath "signtool.exe"))
    }

    $ProgramFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    $WindowsKitsBin = Join-Path $ProgramFilesX86 "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $WindowsKitsBin -PathType Container) {
      $VersionDirectories = Get-ChildItem -LiteralPath $WindowsKitsBin -Directory |
        Sort-Object {
          try { [version]$_.Name } catch { [version]"0.0" }
        } -Descending
      foreach ($VersionDirectory in $VersionDirectories) {
        $Candidates.Add((Join-Path $VersionDirectory.FullName "x64\signtool.exe"))
      }
    }
  }

  $Seen = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase
  )
  foreach ($Candidate in $Candidates) {
    if ([string]::IsNullOrWhiteSpace($Candidate)) { continue }
    $CandidatePath = [IO.Path]::GetFullPath($Candidate)
    if (-not $Seen.Add($CandidatePath)) { continue }
    if (-not (Test-Path -LiteralPath $CandidatePath -PathType Leaf)) { continue }

    $Signature = Get-AuthenticodeSignature -LiteralPath $CandidatePath
    $SignerSubject = if ($Signature.SignerCertificate) {
      $Signature.SignerCertificate.Subject
    } else {
      ""
    }
    if ($Signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $SignerSubject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
      throw "Refusing to execute an unverified SignTool binary: $CandidatePath"
    }
    return $CandidatePath
  }

  throw "Microsoft SignTool was not found. Install the Windows 10/11 SDK or pass -SignToolPath."
}

function Resolve-CodeSigningCertificate {
  param(
    [Parameter(Mandatory = $true)][string]$Thumbprint,
    [Parameter(Mandatory = $true)][string]$StoreLocation
  )

  $NormalizedThumbprint = ($Thumbprint -replace '\s', '').ToUpperInvariant()
  if ($NormalizedThumbprint -notmatch '^[0-9A-F]{40}$') {
    throw "Certificate thumbprint must contain exactly 40 hexadecimal characters."
  }

  $CertificatePath = "Cert:\$StoreLocation\My\$NormalizedThumbprint"
  $Certificate = Get-Item -LiteralPath $CertificatePath -ErrorAction SilentlyContinue
  if (-not $Certificate) {
    throw "Code-signing certificate was not found at $CertificatePath."
  }
  if (-not $Certificate.HasPrivateKey) {
    throw "The certificate does not expose a private key to this account: $CertificatePath"
  }

  $Now = Get-Date
  if ($Certificate.NotBefore -gt $Now -or $Certificate.NotAfter -le $Now) {
    throw "The code-signing certificate is not currently valid: $CertificatePath"
  }

  $CodeSigningOid = "1.3.6.1.5.5.7.3.3"
  $HasCodeSigningEku = @($Certificate.EnhancedKeyUsageList) | Where-Object {
    $_.ObjectId.Value -eq $CodeSigningOid
  }
  if (-not $HasCodeSigningEku) {
    throw "The certificate is missing the Code Signing EKU ($CodeSigningOid): $CertificatePath"
  }

  return $Certificate
}

function Resolve-SignableFiles {
  param([Parameter(Mandatory = $true)][string[]]$InputPaths)

  $AllowedExtensions = @(".exe", ".dll", ".pyd")
  $ResolvedFiles = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
  $Seen = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase
  )
  foreach ($InputPath in $InputPaths) {
    if ([string]::IsNullOrWhiteSpace($InputPath)) {
      throw "Artifact paths cannot be empty."
    }
    if (-not (Test-Path -LiteralPath $InputPath -PathType Leaf)) {
      throw "Signing artifact was not found or is not a file: $InputPath"
    }

    $Item = Get-Item -LiteralPath $InputPath -Force
    if (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
      throw "Refusing to sign a reparse point or symbolic link: $($Item.FullName)"
    }
    if ($Item.Extension.ToLowerInvariant() -notin $AllowedExtensions) {
      throw "Unsupported signing artifact type '$($Item.Extension)': $($Item.FullName)"
    }
    if ($Seen.Add($Item.FullName)) {
      $ResolvedFiles.Add($Item)
    }
  }

  if ($ResolvedFiles.Count -eq 0) {
    throw "No signing artifacts were provided."
  }
  return $ResolvedFiles.ToArray()
}

function Assert-TimestampUrl {
  param([string]$Url)

  if ([string]::IsNullOrWhiteSpace($Url)) { return }
  $Parsed = $null
  if (-not [Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$Parsed) -or
      $Parsed.Scheme -notin @("http", "https")) {
    throw "TimestampUrl must be an absolute HTTP or HTTPS RFC 3161 endpoint."
  }
}

$Certificate = Resolve-CodeSigningCertificate `
  -Thumbprint $CertificateThumbprint `
  -StoreLocation $CertificateStoreLocation
$NormalizedThumbprint = $Certificate.Thumbprint.ToUpperInvariant()
$Artifacts = Resolve-SignableFiles -InputPaths $Path
Assert-TimestampUrl -Url $TimestampUrl
$ResolvedSignTool = Resolve-MicrosoftSignTool -RequestedPath $SignToolPath

$BaseSignArguments = @(
  "sign",
  "/fd", "SHA256",
  "/sha1", $NormalizedThumbprint,
  "/s", "My"
)
if ($CertificateStoreLocation -eq "LocalMachine") {
  $BaseSignArguments += "/sm"
}
if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
  $BaseSignArguments += @("/tr", $TimestampUrl, "/td", "SHA256")
}

$ArtifactsToSign = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
foreach ($Artifact in $Artifacts) {
  $ExistingSignature = Get-AuthenticodeSignature -LiteralPath $Artifact.FullName
  $ExistingThumbprint = if ($ExistingSignature.SignerCertificate) {
    $ExistingSignature.SignerCertificate.Thumbprint.ToUpperInvariant()
  } else {
    ""
  }
  if ($ExistingSignature.Status -eq [System.Management.Automation.SignatureStatus]::Valid) {
    if ($ExistingThumbprint -eq $NormalizedThumbprint) {
      Write-Host "Already signed and verified: $($Artifact.FullName)"
      continue
    }
    throw "Refusing to replace an existing valid signature on: $($Artifact.FullName)"
  }
  if ($ExistingSignature.SignerCertificate) {
    throw "Refusing to replace an existing invalid or untrusted signature on: $($Artifact.FullName)"
  }
  $ArtifactsToSign.Add($Artifact)
}

foreach ($Artifact in $ArtifactsToSign) {
  if (-not $PSCmdlet.ShouldProcess($Artifact.FullName, "Authenticode sign")) {
    continue
  }

  Write-Host "Signing: $($Artifact.FullName)"
  & $ResolvedSignTool @BaseSignArguments $Artifact.FullName
  if ($LASTEXITCODE -ne 0) {
    throw "SignTool failed with exit code $LASTEXITCODE for: $($Artifact.FullName)"
  }

  $VerifiedSignature = Get-AuthenticodeSignature -LiteralPath $Artifact.FullName
  $VerifiedThumbprint = if ($VerifiedSignature.SignerCertificate) {
    $VerifiedSignature.SignerCertificate.Thumbprint.ToUpperInvariant()
  } else {
    ""
  }
  if ($VerifiedSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
      $VerifiedThumbprint -ne $NormalizedThumbprint) {
    throw "Authenticode verification failed after signing: $($Artifact.FullName)"
  }

  & $ResolvedSignTool verify /pa /v $Artifact.FullName
  if ($LASTEXITCODE -ne 0) {
    throw "SignTool verification failed with exit code $LASTEXITCODE for: $($Artifact.FullName)"
  }
  Write-Host "Verified signer $NormalizedThumbprint on: $($Artifact.FullName)"
}
