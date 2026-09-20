# Revisión de implementación — 0.5.0-beta.1

Revisión del estado `19061d0` y ampliación local del flujo de capturas. El alcance acordado es región, ventana, pantalla, retardo y edición/anotaciones, además de compatibilidad al pegar y opciones de peso. No incluye vídeo ni OCR.

## Hallazgos y correcciones

| Hallazgo | Efecto | Cambio |
|---|---|---|
| Solo se publicaba DIBV5 | Compatibilidad dependiente de la interpretación del receptor y de conversiones del sistema; posible relación con el fallo observado en Steam | PNG primero, DIBV5 y DIB clásico explícitos; espera de hasta aproximadamente un segundo si el portapapeles está ocupado |
| La calidad solo controlaba la compresión del archivo JPEG | Elegir JPEG no reducía la resolución ni necesariamente la imagen pegada | Perfiles con resolución, compresión y muestreo de color; la reducción también afecta al portapapeles |
| El códec JPEG usaba su muestreo de color predeterminado | Los bordes de color podían perder detalle incluso al elevar la calidad | Opción 4:4:4 para detalle y 4:2:0 para tamaño |
| Exponente incorrecto en números FP16 subnormales | Valores lineales muy pequeños se reducían a la mitad | Conversión corregida y comprobada para los 65.536 patrones FP16 |
| Callback de WGC con referencias a variables de pila | Riesgo de acceso a memoria invalidada si el callback continuaba después del timeout | Estado compartido y referencias D3D retenidas por el callback |
| Búfer UTF-8→UTF-16 un carácter menor de lo pedido a Windows | Escritura del terminador fuera del tamaño lógico del string durante el manejo de errores | Reserva incluyendo el terminador y eliminación posterior |
| Flujo limitado a seleccionar y guardar | No cubría el alcance de sustitución solicitado | Modos, retardo y editor propios |

Publicar solo DIBV5 es válido según Windows. La nueva combinación amplía compatibilidad, pero no demuestra por sí misma que todas las versiones de Steam acepten el pegado. Chromium también publica PNG antes de DIBV5 para cubrir receptores con diferencias de compatibilidad.

Referencias primarias: [formatos del portapapeles de Microsoft](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-formats), [codificador JPEG WIC](https://learn.microsoft.com/en-us/windows/win32/wic/jpeg-format-overview), [implementación del portapapeles de Chromium](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/base/clipboard/clipboard_win.cc), [funciones de Recortes](https://support.microsoft.com/es-es/windows/apps/use-snipping-tool-to-capture-screenshots).

## Verificación realizada

- Compilación Release x64, MSVC v143 y Windows SDK 10.0.26100.0.
- Conversión exhaustiva de FP16 y casos límite de la transferencia sRGB.
- DIB24 y DIBV5: orientación, alfa, anchuras impares, relleno y paso de fila del origen.
- PNG: codificación/decodificación sin pérdida, también desde memoria como en el portapapeles.
- JPEG: lectura del marcador SOF del archivo generado para verificar 4:4:4 y 4:2:0 reales.
- Reducción proporcional horizontal y vertical, y ausencia de ampliación en recortes pequeños.
- Publicación, lectura y persistencia del portapapeles en una estación de ventanas separada: las pruebas no sustituyen el portapapeles del usuario.
- Ocultación opaca, recorte, deshacer y rehacer sobre los píxeles editados.
- Selector: cambio de modo, detección de ventana, arrastre inverso, clic accidental, Enter y Escape.
- Prueba de WGC sobre el escritorio real de **7024 × 2207** y una ventana independiente de **304 × 232**. Los fotogramas se mantuvieron en memoria.

En una imagen **sintética** de 3840 × 2160 con textura, los archivos generados pesaron 866.143 bytes (Ligera), 10.705.589 (Equilibrada) y 20.829.117 (PNG original). Son mediciones de esa imagen de prueba, no un límite ni una promesa para otras capturas. Una prueba con gráficos planos produjo PNG más pequeño que JPEG.

Comandos reproducibles:

```powershell
./build.ps1
./test.ps1
./test.ps1 -CaptureSmoke
```

La última opción requiere un escritorio interactivo con GPU/WGC. Las pruebas de codificación generan imágenes sintéticas en el directorio temporal de Windows. La prueba del editor genera allí una representación de su superficie para inspeccionar distribución; no equivale a una validación interactiva del editor.

## Validación manual pendiente antes de adoptar la versión

El servicio de control visual de aplicaciones de esta sesión devolvió `native pipe is unavailable`; no se ha podido completar una prueba interactiva en las aplicaciones de destino.

1. En Steam, pegar una captura Ligera en el destino donde fallaba, comprobar la previsualización y cancelar sin enviarla. Repetir con Máxima y un recorte pequeño. Registrar versión de Steam y destino exacto si falla.
2. Comparar HDR activado/desactivado, pantallas SDR/HDR mixtas y escenas de juegos con altas luces. La curva de compresión HDR existente se conserva; no se afirma que sea una reproducción colorimétrica exacta de todo el gamut HDR.
3. Capturar una región que cruce monitores, una ventana parcialmente tapada, un monitor completo y todo el escritorio; comprobar foco y retorno al juego en pantalla completa exclusiva.
4. Usar retardo de 3/5/10 segundos, cancelarlo y comprobar menús desplegables.
5. Editar, copiar, guardar como PNG/JPEG, cancelar el diálogo, confirmar sobrescritura y cerrar con cambios pendientes. Mover editor/configuración entre monitores con DPI distinto.

## Límites relevantes

- La ventana se captura al confirmar su selección. Puede mostrar un instante diferente del escritorio congelado; la conversión usa el blanco SDR del monitor al que pertenece la ventana.
- El contenido protegido o aplicaciones que bloqueen la captura pueden impedir WGC. Los formatos del portapapeles tampoco pueden obligar a un campo de texto a admitir imágenes.
- El editor conserva el original autoguardado. Ocultar una zona protege los píxeles de la **copia editada**, no elimina la información del archivo original.
- El editor es una primera implementación: vista ajustada a ventana, anotaciones básicas e historial limitado. No hay OCR, vídeo ni selección de forma libre en este alcance.

Se entrega como **beta** por la amplitud de los cambios y las comprobaciones interactivas pendientes. El binario no está firmado.
