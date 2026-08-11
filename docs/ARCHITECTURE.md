# Arquitectura

## Objetivo

Windows compone el escritorio HDR en scRGB lineal. Una captura directa a 8 bits puede recortar valores superiores a `1.0`, alterar el blanco SDR o producir una imagen lavada. NativeHDRShot conserva la precisión durante la adquisición y convierte a SDR únicamente al codificar el archivo final.

## Flujo de captura

```text
Impr Pant (hook nativo) / Ctrl+Mayús+F11
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
          ▼
Fotograma SDR congelado en una ventana Win32 de primer plano
          │
          ▼
Selección y recorte de región
          │
          ├── PNG BGRA 8 bits, sin pérdida
          ├── JPEG BGR 8 bits, calidad configurable
          └── Portapapeles CF_DIBV5 (sRGB)
```

## Componentes

### Ventana residente

Una ventana Win32 de solo mensajes gestiona el icono del área de notificación y recibe el resultado de los trabajos de captura. Un hook `WH_KEYBOARD_LL` intercepta `Impr Pant` sin bloquear el hilo dentro del callback: únicamente publica un mensaje privado a la ventana y devuelve inmediatamente.

`RegisterHotKey` mantiene en paralelo una reserva de `Impr Pant`. Si Windows retira silenciosamente el hook y la reserva recibe la siguiente pulsación, la aplicación continúa capturando, cambia el icono a advertencia y ofrece reinstalar el hook desde Configuración. Las combinaciones modificadas como `Alt + Impr Pant` y `Win + Impr Pant` se dejan pasar a Windows.

### Selector de región

Antes de abrir el selector se adquiere y convierte a SDR un fotograma completo del monitor. Ese búfer se dibuja en una ventana Win32 opaca, `WS_EX_TOPMOST`, que toma el primer plano. La cruz y el marco aparecen sobre la copia congelada, no sobre la superficie del juego. Esto también cubre juegos a pantalla completa exclusiva: aunque al cambiar el foco el juego se minimice o deje de presentar fotogramas, la imagen que se va a guardar ya está capturada.

La ventana recibe el ratón de forma normal, sin depender de un hook global que un sistema anti-cheat pueda bloquear. Se libera temporalmente `ClipCursor`, se dibuja una cruz propia y se restaura la restricción anterior al terminar. Después se devuelve el primer plano a la ventana que estaba activa.

El recorte se extrae directamente del búfer congelado, por lo que el archivo y el portapapeles contienen exactamente la imagen que se mostró durante la selección. Un clic menor de tres píxeles se ignora y mantiene abierto el selector.

### Adquisición

Para cada captura se crea un dispositivo D3D11 en el adaptador que controla el monitor seleccionado. `Windows.Graphics.Capture` entrega un fotograma flotante de 16 bits por componente. El recurso se copia a una textura staging legible por CPU.

Los objetos D3D y la sesión se recrean en cada operación. Esto añade un coste pequeño, pero evita conservar dispositivos inválidos después de suspensión, cambio de monitor o reinicio del controlador gráfico.

### Conversión HDR a SDR

`DISPLAYCONFIG_SDR_WHITE_LEVEL` informa del multiplicador con el que Windows representa el blanco SDR dentro del espacio scRGB del monitor HDR. Los canales se normalizan por este valor antes de aplicar una curva de compresión suave a las luces que todavía superan el rango SDR.

La conversión de lineal a sRGB utiliza la función de transferencia estándar por canal. Se conserva la relación entre canales durante la compresión de luminancia para reducir cambios de tono.

### Portapapeles

Después de guardar el archivo, el mismo búfer SDR BGRA se publica como `CF_DIBV5`, con perfil `LCS_sRGB` y alfa opaco. La memoria se reserva con `GMEM_MOVEABLE` y Windows toma su propiedad cuando `SetClipboardData` termina correctamente. Si el portapapeles está ocupado se realizan varios reintentos cortos; un fallo no elimina ni invalida el archivo guardado.

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
