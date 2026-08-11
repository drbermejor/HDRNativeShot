<p align="center">
  <img src="assets/NativeHDRShot.png" width="150" alt="NativeHDRShot icon">
</p>

# NativeHDRShot

[![Build](https://github.com/drbermejor/HDRNativeShot/actions/workflows/build.yml/badge.svg)](https://github.com/drbermejor/HDRNativeShot/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Windows-11-0078D4.svg)](#requisitos)

Herramienta Win32 nativa para capturar regiones de una pantalla con HDR activo y guardarlas como imágenes SDR con colores y brillo correctos.

NativeHDRShot utiliza Windows Graphics Capture y Direct3D 11. La captura se realiza internamente en `R16G16B16A16_FLOAT` (scRGB), se normaliza usando el nivel de blanco SDR configurado para el monitor y finalmente se convierte a sRGB. No guarda archivos HDR.

## Características

- Captura de región mediante `Impr Pant`.
- Atajo alternativo `Ctrl + Mayús + F12`.
- Selección limpia: cursor en cruz y marco, sin animaciones ni carteles.
- `Enter` captura el monitor completo y `Esc` cancela.
- PNG sin pérdida o JPEG con calidad configurable entre 50 y 100 %.
- Selecciona automáticamente el monitor situado bajo el cursor.
- Compatible con escalado DPI y configuraciones multimonitor.
- Reinicializa D3D en cada captura para recuperarse de suspensión, cambios de pantalla o reinicios del controlador.
- Icono residente, registro de diagnóstico e inicio automático con Windows.
- Sin Electron, servicios en la nube, telemetría ni dependencias externas durante la ejecución.

## Instalación rápida

1. Descarga el ZIP del repositorio desde `Code → Download ZIP` y extráelo.
2. Ejecuta [`install.cmd`](install.cmd).
3. NativeHDRShot aparecerá en el área de notificación y quedará configurado para el próximo inicio de sesión.

La instalación es únicamente para el usuario actual y no requiere permisos de administrador. Copia el programa a:

```text
%LOCALAPPDATA%\NativeHDRShot\NativeHDRShot.exe
```

También crea este acceso directo de inicio:

```text
%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\NativeHDRShot.lnk
```

El ejecutable incluido en [`dist`](dist) no está firmado digitalmente. Windows SmartScreen puede mostrar una advertencia la primera vez; revisa el código o compílalo localmente si prefieres no ejecutar el binario distribuido.

La suma SHA-256 del binario se publica en [`dist/SHA256SUMS.txt`](dist/SHA256SUMS.txt).

## Uso

1. Coloca el cursor en el monitor que quieres capturar.
2. Pulsa `Impr Pant` o `Ctrl + Mayús + F12`.
3. Arrastra el cursor para marcar la región.

Durante la selección:

| Acción | Resultado |
|---|---|
| Arrastrar con el botón izquierdo | Capturar región |
| `Enter` | Capturar el monitor completo |
| `Esc` o botón derecho | Cancelar |

Las imágenes se guardan por mes en:

```text
%USERPROFILE%\Pictures\NativeHDRShot\AAAA-MM
```

## Configuración de calidad

Haz clic derecho en el icono del área de notificación y abre `Configuración…`.

- **PNG — máxima calidad:** formato predeterminado y sin pérdida.
- **JPEG — archivo más pequeño:** permite ajustar la calidad entre 50 y 100 %.

La configuración se guarda en `HKCU\Software\NativeHDRShot` y permanece después de reiniciar.

## Desinstalación

Ejecuta [`uninstall.cmd`](uninstall.cmd). El desinstalador:

- detiene NativeHDRShot;
- elimina el ejecutable instalado y el acceso directo de Inicio;
- elimina las preferencias del usuario;
- conserva todas las capturas de `Imágenes\NativeHDRShot`.

## Requisitos

- Windows 11 x64.
- GPU y controlador compatibles con Direct3D 11.
- Windows Graphics Capture disponible.

Para compilar:

- Visual Studio 2022 con **Desarrollo para el escritorio con C++**;
- Windows 11 SDK `10.0.26100.0` o compatible;
- MSVC v143.

## Compilación

Desde PowerShell:

```powershell
.\build.ps1
```

El resultado se genera en `bin\NativeHDRShot.exe`. La configuración Release enlaza estáticamente el runtime de C++, por lo que el ejecutable no necesita instalar el Visual C++ Redistributable.

También puedes abrir [`NativeHDRShot.sln`](NativeHDRShot.sln) en Visual Studio y compilar `Release | x64`.

## Estructura

```text
assets/             Iconos PNG e ICO
dist/               Ejecutable Release redistribuible
docs/               Arquitectura y solución de problemas
src/                Código C++ y recursos
NativeHDRShot.sln    Solución de Visual Studio
install.*            Instalador por usuario
uninstall.*          Desinstalador por usuario
```

Consulta [Arquitectura](docs/ARCHITECTURE.md) para conocer el flujo HDR → SDR y [Solución de problemas](docs/TROUBLESHOOTING.md) si un atajo o una captura falla.

## Privacidad

Todo el procesamiento se realiza localmente. NativeHDRShot no usa red, no envía telemetría y no analiza el contenido de las capturas. El registro solo contiene fechas, rutas de salida, estado del atajo y valores técnicos de luminancia.

## Licencia

Este proyecto se distribuye bajo [GNU General Public License v3.0](LICENSE).
