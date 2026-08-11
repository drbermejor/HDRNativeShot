# Changelog

Todas las modificaciones relevantes se documentarán en este archivo.

## 0.2.0 - 2026-08-11

- Intercepción nativa de `Impr Pant` mediante `WH_KEYBOARD_LL`.
- Reserva paralela con `RegisterHotKey` para detectar y cubrir la pérdida del hook.
- Estado de protección en el icono de bandeja, el menú y Configuración.
- Botón para recuperar o renovar el control de `Impr Pant` sin reiniciar.
- Atajo alternativo cambiado a `Ctrl + Mayús + F11`; `F12` está reservado por Windows para depuradores.

## 0.1.0 - 2026-08-11

Primera versión pública.

- Captura nativa de regiones y monitores con Windows Graphics Capture.
- Adquisición scRGB flotante y conversión HDR a SDR consciente del blanco del monitor.
- Salida PNG sin pérdida y JPEG con calidad configurable.
- Atajos globales, selector de región e icono residente.
- Panel de configuración y persistencia por usuario.
- Instalador, desinstalador, binario x64 y compilación automatizada.
