@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if errorlevel 1 (
  echo.
  echo La instalacion no se completo.
  pause
  exit /b 1
)
echo.
echo Instalacion completada.
pause
