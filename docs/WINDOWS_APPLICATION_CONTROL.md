# Windows Application Control and enterprise signing

Gaussian Scene Workbench does not disable or bypass Windows Defender Application Control (WDAC), App Control for Business, or Enterprise Application Control. On a managed workstation, the native desktop executable, locally built CUDA extensions, and test executables must satisfy the organization's active policy before Windows will create the process or load the module.

An Authenticode signature is necessary only when the active policy permits that certificate or publisher. Signing with an arbitrary public or self-signed certificate does not make a binary enterprise-approved.

## Required organization input

Obtain all of the following from the security or endpoint-management owner:

1. An organization-approved code-signing certificate installed in `Cert:\CurrentUser\My` or `Cert:\LocalMachine\My`.
2. Access to its private key for the account that performs the build.
3. The Code Signing EKU (`1.3.6.1.5.5.7.3.3`) and a currently valid certificate lifetime.
4. A WDAC publisher or signer rule that permits the certificate. A publisher rule is preferred over per-build hashes because every relink changes the executable hash.
5. The organization's RFC 3161 timestamp URL, when timestamping is required.

Do not install a development root certificate, trust a self-signed certificate, or disable the active policy as a workaround.

List usable certificates without changing either certificate store:

```powershell
$codeSigningOid = "1.3.6.1.5.5.7.3.3"
Get-ChildItem Cert:\CurrentUser\My,Cert:\LocalMachine\My |
  Where-Object {
    $_.HasPrivateKey -and
    $_.NotBefore -le (Get-Date) -and
    $_.NotAfter -gt (Get-Date) -and
    ($_.EnhancedKeyUsageList.ObjectId.Value -contains $codeSigningOid)
  } |
  Select-Object Subject, Thumbprint, NotAfter, PSParentPath
```

## Sign a native build

The local build signs the application and every native test/helper executable before CTest launches them. Packaging verifies or signs the installed application again before its launch smoke test.

```powershell
$env:GSW_WINDOWS_SIGNING_CERTIFICATE_THUMBPRINT = "40_HEX_CHARACTER_THUMBPRINT"
$env:GSW_WINDOWS_SIGNING_CERTIFICATE_STORE_LOCATION = "LocalMachine"
$env:GSW_WINDOWS_SIGNING_TIMESTAMP_URL = "https://organization.example/rfc3161"
$env:GSW_NATIVE_CMAKE_ROOT = "C:\Program Files\CMake"

powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 `
  -Configuration Release `
  -Package
```

The equivalent command-line parameters are `-SigningCertificateThumbprint`, `-SigningCertificateStoreLocation`, `-SigningTimestampUrl`, `-SignToolPath`, and `-CMakeRoot`. The selected CMake root must contain both `cmake.exe` and `ctest.exe`; use a vendor-signed, organization-approved CMake distribution if policy rejects the Conda copy. Ninja, MSVC, Qt deployment tools, Python, and CUDA tools must likewise be permitted by policy. When no thumbprint is configured, the build remains unsigned; this preserves normal development on unmanaged machines but an enforced WDAC workstation may reject its tests or executable.

The signer script accepts only explicit `.exe`, `.dll`, or `.pyd` files, rejects symbolic links and unexpected file types, refuses to replace an existing signature, validates the certificate and Microsoft SignTool before use, and verifies the resulting signer. It never creates a certificate or changes a trust store.

The public GitHub Actions preview does not have access to an organization certificate, so its artifact remains unsigned. If managed deployment is required, sign in a protected internal release job backed by the organization's secret store; never commit a PFX, password, private key, or exported certificate bundle to this repository.

## Sign approved local CUDA extensions

The 3DGS training preflight loads the locally compiled `diff_gaussian_rasterization` and `simple_knn` extensions. If policy blocks either `_C*.pyd`, ask the security owner to approve the locally built modules. Sign them with the organization certificate only when that owner explicitly authorizes signing third-party-derived build output:

```powershell
$gaussianEnvironment = $env:GAUSSIAN_SPLATTING_CONDA_PREFIX
if (-not $gaussianEnvironment) { $gaussianEnvironment = $env:GS_CONDA_PREFIX }
if (-not $gaussianEnvironment) { throw "Set the Gaussian Conda environment first." }

$extensions = @(
  Get-ChildItem `
    (Join-Path $gaussianEnvironment "Lib\site-packages\diff_gaussian_rasterization") `
    -Filter "_C*.pyd" -File
  Get-ChildItem `
    (Join-Path $gaussianEnvironment "Lib\site-packages\simple_knn") `
    -Filter "_C*.pyd" -File
) | Select-Object -ExpandProperty FullName -Unique
if ($extensions.Count -ne 2) {
  throw "Expected exactly two local 3DGS CUDA extensions; found $($extensions.Count)."
}

& .\scripts\sign_windows_artifacts.ps1 `
  -Path $extensions `
  -CertificateThumbprint $env:GSW_WINDOWS_SIGNING_CERTIFICATE_THUMBPRINT `
  -CertificateStoreLocation $env:GSW_WINDOWS_SIGNING_CERTIFICATE_STORE_LOCATION `
  -TimestampUrl $env:GSW_WINDOWS_SIGNING_TIMESTAMP_URL
```

COLMAP is a third-party executable and needs a separate governance decision. Prefer a rule for its verified upstream publisher or an IT-distributed approved package. Do not overwrite an existing vendor signature with the project certificate.

## Verify and collect a policy request

Verify signatures and hashes:

```powershell
$artifacts = @(
  "native\build\Gaussian Scene Workbench.exe"
  $extensions
)
$artifacts | ForEach-Object {
  Get-AuthenticodeSignature -LiteralPath $_ |
    Select-Object Path, Status, @{Name="Signer"; Expression={$_.SignerCertificate.Subject}}
  Get-FileHash -LiteralPath $_ -Algorithm SHA256
}
```

When a correctly signed binary is still blocked, attach these items to the endpoint-management request:

- the Code Integrity event from `Applications and Services Logs/Microsoft/Windows/CodeIntegrity/Operational` (commonly event 3077 or 3033);
- the active policy identifier named in the event;
- the full binary path and SHA-256 hash;
- the signing certificate subject, issuer, serial number, and thumbprint;
- a request for a publisher/signer rule that covers the native application and locally rebuilt CUDA extensions.

The final gate is execution under the managed policy: run the packaged application's smoke test and the desktop training preflight. A valid Authenticode signature alone is not proof that the WDAC policy permits it.
