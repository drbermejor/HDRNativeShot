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

$taskName = 'NativeHDRShot'
$installDirectory = Join-Path $env:ProgramFiles 'NativeHDRShot'
$destination = Join-Path $installDirectory 'NativeHDRShot.exe'
$expectedDirectory = [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'NativeHDRShot'))
if ([IO.Path]::GetFullPath($installDirectory) -ne $expectedDirectory) {
    throw 'La ruta de instalacion no coincide con el destino protegido esperado.'
}

Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
$runningInstances = Get-Process -Name NativeHDRShot -ErrorAction SilentlyContinue
if ($runningInstances) {
    $runningInstances | Stop-Process -Force
    $runningInstances | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $destination -Force

$account = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute $destination -WorkingDirectory $installDirectory
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $account
$principal = New-ScheduledTaskPrincipal -UserId $account -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings -Description 'Captura HDR nativa a SDR' -Force | Out-Null

$startupShortcut = Join-Path ([Environment]::GetFolderPath('Startup')) 'NativeHDRShot.lnk'
Remove-Item -LiteralPath $startupShortcut -Force -ErrorAction SilentlyContinue
$legacyExecutable = Join-Path $env:LOCALAPPDATA 'NativeHDRShot\NativeHDRShot.exe'
Remove-Item -LiteralPath $legacyExecutable -Force -ErrorAction SilentlyContinue

Start-ScheduledTask -TaskName $taskName
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    $installedProcess = Get-Process -Name NativeHDRShot -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $destination } | Select-Object -First 1
    if ($installedProcess) { break }
    Start-Sleep -Milliseconds 100
}
if (-not $installedProcess) {
    throw 'La tarea se registro, pero NativeHDRShot no pudo iniciarse.'
}

Write-Host "Instalado con privilegios elevados y en ejecucion: $destination"
Write-Host "Inicio automatico: tarea programada $taskName"
