# Arquitectura

## Objetivo

Windows compone el escritorio HDR en scRGB lineal. Una captura directa a 8 bits puede recortar valores superiores a `1.0`, alterar el blanco SDR o producir una imagen lavada. NativeHDRShot conserva la precisión durante la adquisición y convierte a SDR únicamente al codificar el archivo final.

## Flujo de captura

```text
Impr Pant / Ctrl+Mayús+F12
          │
          ▼
Selección Win32 de región
          │
          ▼
Windows Graphics Capture
R16G16B16A16_FLOAT (scRGB)
          │
          ▼
Lectura D3D11 en CPU
          │
          ▼
Normalización del blanco SDR del monitor
DISPLAYCONFIG_SDR_WHITE_LEVEL
          │
          ▼
Compresión suave de luces HDR + conversión sRGB
          │
          ├── PNG BGRA 8 bits, sin pérdida
          └── JPEG BGR 8 bits, calidad configurable
```

## Componentes

### Ventana residente

Una ventana Win32 de solo mensajes registra los atajos globales, gestiona el icono del área de notificación y recibe el resultado de los trabajos de captura.

### Selector de región

Es una ventana `WS_EX_LAYERED` situada sobre el monitor bajo el cursor. El color de fondo se vuelve transparente mediante color key. Solo dibuja el marco de selección; al cerrarse se espera brevemente antes de capturar para que no aparezca en el fotograma.

### Adquisición

Para cada captura se crea un dispositivo D3D11 en el adaptador que controla el monitor seleccionado. `Windows.Graphics.Capture` entrega un fotograma flotante de 16 bits por componente. El recurso se copia a una textura staging legible por CPU.

Los objetos D3D y la sesión se recrean en cada operación. Esto añade un coste pequeño, pero evita conservar dispositivos inválidos después de suspensión, cambio de monitor o reinicio del controlador gráfico.

### Conversión HDR a SDR

`DISPLAYCONFIG_SDR_WHITE_LEVEL` informa del multiplicador con el que Windows representa el blanco SDR dentro del espacio scRGB del monitor HDR. Los canales se normalizan por este valor antes de aplicar una curva de compresión suave a las luces que todavía superan el rango SDR.

La conversión de lineal a sRGB utiliza la función de transferencia estándar por canal. Se conserva la relación entre canales durante la compresión de luminancia para reducir cambios de tono.

### Codificación

Windows Imaging Component codifica el búfer SDR:

- PNG: `GUID_WICPixelFormat32bppBGRA`;
- JPEG: `GUID_WICPixelFormat24bppBGR` con `ImageQuality` configurable.

### Persistencia

Las preferencias se almacenan bajo `HKCU\Software\NativeHDRShot`. El instalador crea un acceso directo en la carpeta Inicio del usuario; no instala servicios ni controladores.

## Recuperación y diagnóstico

- Solo puede existir una instancia residente mediante un mutex con nombre.
- Cada espera de fotograma tiene un timeout de cinco segundos.
- Las capturas simultáneas se rechazan hasta que finaliza la anterior.
- El registro rota al superar 1 MiB.
- Registro actual: `%LOCALAPPDATA%\NativeHDRShot\NativeHDRShot.log`.
