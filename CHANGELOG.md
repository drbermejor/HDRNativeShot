# Changelog

Todas las modificaciones relevantes se documentarán en este archivo.

## 0.4.1 - 2026-08-11

- Selector continuo sobre el escritorio virtual completo y regiones que pueden abarcar varios monitores.
- Captura y conversión HDR→SDR separada para cada pantalla antes de componer el fotograma congelado.
- Selección automática del monitor ocupado por la ventana a pantalla completa, aunque el cursor haya quedado en otra pantalla.
- Comprobación real de que el selector obtuvo el primer plano.
- Recuperación para superficies exclusivas que no ceden el primer plano, restaurando la ventana anterior al terminar.
- Registro de las coordenadas, resolución y criterio usado para elegir el monitor.
- Confirmación visual propia al guardar, independiente de las notificaciones que Windows puede ocultar en pantalla completa.
- Vigilancia temporal de la banda `TOPMOST` para recuperar el selector si otra ventana intenta superponerse durante la selección.
- Instalación protegida en `Program Files` y arranque elevado mediante tarea programada para funcionar sobre ventanas administrativas.
- Manifiesto `requireAdministrator` para impedir ejecuciones accidentales con un nivel de integridad insuficiente.
- Captura y conversión simultánea de los monitores para reducir el retardo antes de mostrar el selector.
- Actualización del binario elevado después de detener la tarea, evitando bloqueos del ejecutable instalado.

## 0.4.0 - 2026-08-11

- Captura previa del monitor y selector opaco sobre un fotograma SDR congelado.
- Ventana nativa de primer plano para mostrar la cruz incluso sobre juegos a pantalla completa exclusiva.
- Selección independiente del cursor y de los hooks de ratón que pueda bloquear un sistema anti-cheat.
- Restauración de la ventana que estaba activa al completar o cancelar.
- El cursor se libera durante la selección y recupera después su restricción previa.
- Una pulsación corta ya no cancela silenciosamente: el selector espera un arrastre real.
- Cruce de selección dibujado por NativeHDRShot incluso si el juego oculta el cursor del sistema.

## 0.3.0 - 2026-08-11

- Copia automática de cada captura SDR al portapapeles mediante `CF_DIBV5`.
- Compatibilidad de pegado mediante las conversiones de bitmap proporcionadas por Windows.
- Reintentos breves cuando otra aplicación mantiene abierto el portapapeles.
- El archivo se conserva y se muestra una advertencia si la copia al portapapeles falla.
- El instalador espera y reintenta el reemplazo si Windows tarda en liberar el ejecutable anterior.

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
