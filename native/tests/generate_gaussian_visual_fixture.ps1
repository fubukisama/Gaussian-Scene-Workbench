param(
  [string]$OutputDirectory = (Join-Path $env:TEMP "gsw-gaussian-visual"),
  [ValidateRange(300, 300000)]
  [int]$Count = 7200
)

$ErrorActionPreference = "Stop"
$culture = [Globalization.CultureInfo]::InvariantCulture
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$layerCount = 3
$samplesPerLayer = [Math]::Max(100, [Math]::Floor($Count / $layerCount))
$vertexCount = $samplesPerLayer * $layerCount
$shDc = 0.28209479177387814

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$plyPath = Join-Path $OutputDirectory "anisotropic-gaussians.ply"
$projectPath = Join-Path $OutputDirectory "anisotropic-gaussians.gsw.json"

$writer = [IO.StreamWriter]::new($plyPath, $false, $utf8NoBom)
try {
  $writer.WriteLine("ply")
  $writer.WriteLine("format ascii 1.0")
  $writer.WriteLine("comment Gaussian Scene Workbench visual QA fixture")
  $writer.WriteLine("element vertex $vertexCount")
  foreach ($property in @(
      "property float x", "property float y", "property float z",
      "property float f_dc_0", "property float f_dc_1", "property float f_dc_2",
      "property float opacity",
      "property float scale_0", "property float scale_1", "property float scale_2",
      "property float rot_0", "property float rot_1", "property float rot_2", "property float rot_3")) {
    $writer.WriteLine($property)
  }
  $writer.WriteLine("end_header")

  $palettes = @(
    @(0.20, 0.78, 0.63),
    @(0.92, 0.62, 0.18),
    @(0.32, 0.53, 0.94)
  )
  for ($layer = 0; $layer -lt $layerCount; ++$layer) {
    $phase = $layer * 0.72
    for ($index = 0; $index -lt $samplesPerLayer; ++$index) {
      $t = 2.0 * [Math]::PI * $index / $samplesPerLayer
      $radius = 1.85 + 0.24 * [Math]::Sin(5.0 * $t + $phase)
      $radiusDerivative = 1.20 * [Math]::Cos(5.0 * $t + $phase)
      $x = $radius * [Math]::Cos($t)
      $y = $radius * [Math]::Sin($t)
      $z = ($layer - 1) * 0.52 + 0.13 * [Math]::Sin(3.0 * $t + $phase)

      $dx = $radiusDerivative * [Math]::Cos($t) - $radius * [Math]::Sin($t)
      $dy = $radiusDerivative * [Math]::Sin($t) + $radius * [Math]::Cos($t)
      $angle = [Math]::Atan2($dy, $dx)
      $rotationW = [Math]::Cos($angle * 0.5)
      $rotationZ = [Math]::Sin($angle * 0.5)

      $scaleX = [Math]::Log(0.038 + 0.006 * $layer)
      $scaleY = [Math]::Log(0.010 + 0.002 * (($layer + 1) % 3))
      $scaleZ = [Math]::Log(0.016)
      $desiredOpacity = 0.72 + 0.18 * (0.5 + 0.5 * [Math]::Sin(7.0 * $t + $phase))
      $opacity = [Math]::Log($desiredOpacity / (1.0 - $desiredOpacity))

      $palette = $palettes[$layer]
      $brightness = 0.82 + 0.18 * (0.5 + 0.5 * [Math]::Cos(4.0 * $t))
      $red = [Math]::Min(1.0, $palette[0] * $brightness)
      $green = [Math]::Min(1.0, $palette[1] * $brightness)
      $blue = [Math]::Min(1.0, $palette[2] * $brightness)
      $dcRed = ($red - 0.5) / $shDc
      $dcGreen = ($green - 0.5) / $shDc
      $dcBlue = ($blue - 0.5) / $shDc

      $values = @(
        $x, $y, $z, $dcRed, $dcGreen, $dcBlue, $opacity,
        $scaleX, $scaleY, $scaleZ,
        $rotationW, 0.0, 0.0, $rotationZ
      )
      $writer.WriteLine([string]::Join(" ", @(
        $values | ForEach-Object { $_.ToString("R", $culture) }
      )))
    }
  }
} finally {
  $writer.Dispose()
}

$project = [ordered]@{
  schemaVersion = 1
  application = "Gaussian Scene Workbench"
  projectName = "Anisotropic Gaussian Visual QA"
  rootPath = "."
  datasetPath = ""
  scenePath = "anisotropic-gaussians.ply"
  updatedUtc = [DateTime]::UtcNow.ToString("o", $culture)
}
[IO.File]::WriteAllText($projectPath, ($project | ConvertTo-Json -Depth 4), $utf8NoBom)

Write-Output $projectPath
