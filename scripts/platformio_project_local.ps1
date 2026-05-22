param(
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$PlatformioArgs
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pythonExe = Join-Path $repoRoot '.venv\Scripts\python.exe'

if (-not (Test-Path $pythonExe)) {
  throw "Python executable not found: $pythonExe"
}

$usedDrives = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Name.ToUpperInvariant() })
$driveLetter = $null
foreach ($candidate in [char[]]([char]'Z'..[char]'P')) {
  $letter = [string]$candidate
  if ($usedDrives -notcontains $letter) {
    $driveLetter = $letter
    break
  }
}

if (-not $driveLetter) {
  throw "No free drive letter was available for a short PlatformIO workspace path."
}

$mappedRoot = "$driveLetter`:"
$needsUnmap = $false

try {
  & cmd.exe /c "subst $mappedRoot `"$repoRoot`"" | Out-Null
  $needsUnmap = $true

  $mappedPythonExe = Join-Path $mappedRoot '.venv\Scripts\python.exe'
  $env:PLATFORMIO_HOME_DIR = Join-Path $mappedRoot '.platformio-home'

  Push-Location $mappedRoot
  try {
    & $mappedPythonExe -X utf8 -m platformio @PlatformioArgs
    exit $LASTEXITCODE
  } finally {
    Pop-Location
  }
} finally {
  if ($needsUnmap) {
    & cmd.exe /c "subst $mappedRoot /d" | Out-Null
  }
}
