param([switch]$SkipClipboard, [switch]$CaptureSmoke)
$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $visualStudio) { throw 'No se encontró Visual Studio con MSBuild.' }
$msbuild = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild (Join-Path $projectDirectory 'tests\tests.vcxproj') /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$testBinary = Join-Path $projectDirectory 'bin\tests\NativeHDRShot.Tests.exe'
& $testBinary
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $SkipClipboard) {
    # A private window station isolates these tests from the user's clipboard.
    & $testBinary --clipboard
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& $msbuild (Join-Path $projectDirectory 'tests\capture_tests.vcxproj') /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$captureTests = Join-Path $projectDirectory 'bin\tests\NativeHDRShot.Capture.Tests.exe'
& $captureTests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($CaptureSmoke) {
    & $captureTests --capture
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
