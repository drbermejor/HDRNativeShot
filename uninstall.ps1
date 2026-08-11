$ErrorActionPreference = 'Stop'

$installDirectory = Join-Path $env:LOCALAPPDATA 'NativeHDRShot'
$expectedDirectory = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'NativeHDRShot'))
$resolvedDirectory = [IO.Path]::GetFullPath($installDirectory)
if ($resolvedDirectory -ne $expectedDirectory) {
    throw 'La ruta de desinstalacion no coincide con el destino esperado.'
}

$runningInstances = Get-Process -Name NativeHDRShot -ErrorAction SilentlyContinue
if ($runningInstances) {
    $runningInstances | Stop-Process
    $runningInstances | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
}

$shortcutPath = Join-Path ([Environment]::GetFolderPath('Startup')) 'NativeHDRShot.lnk'
Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $installDirectory -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'HKCU:\Software\NativeHDRShot' -Recurse -Force -ErrorAction SilentlyContinue

Write-Host 'NativeHDRShot se ha desinstalado.'
Write-Host 'Las capturas de la carpeta Imagenes\NativeHDRShot no se han eliminado.'
