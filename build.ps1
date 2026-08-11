param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'No se encontró Visual Studio Installer (vswhere.exe).'
}

$visualStudio = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $visualStudio) {
    throw 'No se encontró una instalación de Visual Studio con MSBuild.'
}

$msbuild = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild (Join-Path $projectDirectory 'NativeHDRShot.sln') /m "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$outputDirectory = if ($Configuration -eq 'Release') { 'bin' } else { 'bin-debug' }
Write-Host "Compilado: $(Join-Path $projectDirectory "$outputDirectory\NativeHDRShot.exe")"
