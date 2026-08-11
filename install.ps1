$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$distributionBinary = Join-Path $projectDirectory 'dist\NativeHDRShot.exe'
$buildBinary = Join-Path $projectDirectory 'bin\NativeHDRShot.exe'
if (Test-Path -LiteralPath $distributionBinary) {
    $source = $distributionBinary
} elseif (Test-Path -LiteralPath $buildBinary) {
    $source = $buildBinary
} else {
    & (Join-Path $projectDirectory 'build.ps1') -Configuration Release
    $source = $buildBinary
}

$installDirectory = Join-Path $env:LOCALAPPDATA 'NativeHDRShot'
$destination = Join-Path $installDirectory 'NativeHDRShot.exe'
New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
$runningInstances = Get-Process -Name NativeHDRShot -ErrorAction SilentlyContinue
if ($runningInstances) {
    $runningInstances | Stop-Process
    $runningInstances | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
}
Copy-Item -LiteralPath $source -Destination $destination -Force

$startupDirectory = [Environment]::GetFolderPath('Startup')
$shortcutPath = Join-Path $startupDirectory 'NativeHDRShot.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $destination
$shortcut.WorkingDirectory = $installDirectory
$shortcut.Description = 'Captura regiones HDR y las convierte a SDR'
$shortcut.IconLocation = "$destination,0"
$shortcut.Save()

Start-Process -FilePath $destination
Write-Host "Instalado y en ejecucion: $destination"
