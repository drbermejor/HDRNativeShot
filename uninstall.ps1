param([switch]$Elevated)

$ErrorActionPreference = 'Stop'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdministrator)) {
    $quotedScript = '"' + $PSCommandPath.Replace('"', '""') + '"'
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File $quotedScript -Elevated"
    $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

$taskName = 'NativeHDRShot'
$installDirectory = Join-Path $env:ProgramFiles 'NativeHDRShot'
$expectedDirectory = [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'NativeHDRShot'))
if ([IO.Path]::GetFullPath($installDirectory) -ne $expectedDirectory) {
    throw 'La ruta de desinstalacion no coincide con el destino protegido esperado.'
}

Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue

$runningInstances = Get-Process -Name NativeHDRShot -ErrorAction SilentlyContinue
if ($runningInstances) {
    $runningInstances | Stop-Process -Force
    $runningInstances | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}

$startupShortcut = Join-Path ([Environment]::GetFolderPath('Startup')) 'NativeHDRShot.lnk'
Remove-Item -LiteralPath $startupShortcut -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $installDirectory -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $env:LOCALAPPDATA 'NativeHDRShot') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'HKCU:\Software\NativeHDRShot' -Recurse -Force -ErrorAction SilentlyContinue

Write-Host 'NativeHDRShot se ha desinstalado.'
Write-Host 'Las capturas de la carpeta Imagenes\NativeHDRShot no se han eliminado.'
