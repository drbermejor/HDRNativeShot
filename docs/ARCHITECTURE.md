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
Normalización del blanco SDR de cada monitor
DISPLAYCONFIG_SDR_WHITE_LEVEL
          │
          ▼
Compresión suave de luces HDR + conversión sRGB
          │
          ▼
Composición SDR congelada del escritorio virtual
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

Antes de abrir el selector se adquiere un fotograma de cada monitor. Cada imagen se convierte a SDR con los parámetros de su propia pantalla y después se coloca en un búfer que reproduce las coordenadas del escritorio virtual. Ese búfer se dibuja en una única ventana Win32 opaca, `WS_EX_TOPMOST`, que abarca todos los monitores y toma el primer plano. La cruz y el marco aparecen sobre la copia congelada, no sobre las superficies originales.

La ventana recibe el ratón de forma normal, sin depender de un hook global que un sistema anti-cheat pueda bloquear. Se libera temporalmente `ClipCursor`, se dibuja una cruz propia y se restaura la restricción anterior al terminar. Después se devuelve el primer plano a la ventana que estaba activa.

Mientras la selección está abierta, un temporizador de vida corta reafirma la posición del selector en la banda `TOPMOST` y recupera el primer plano si otra ventana intenta ocuparlo. Esta vigilancia desaparece junto con el selector y no permanece activa en segundo plano.

El recorte se extrae directamente del búfer congelado y puede atravesar los límites entre monitores. El archivo y el portapapeles contienen exactamente la imagen mostrada durante la selección. Un clic menor de tres píxeles se ignora y mantiene abierto el selector.

### Adquisición

Para cada monitor se crea un dispositivo D3D11 en el adaptador que lo controla. `Windows.Graphics.Capture` entrega un fotograma flotante de 16 bits por componente. El recurso se copia a una textura staging legible por CPU y, tras convertirlo, se compone en sus coordenadas físicas dentro del escritorio virtual.

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

Las preferencias se almacenan bajo `HKCU\Software\NativeHDRShot`. El ejecutable se instala en `%ProgramFiles%\NativeHDRShot` y una tarea programada vinculada al inicio de sesión lo ejecuta con el nivel de integridad más alto. Esto permite que el hook y el selector funcionen sobre procesos elevados sin depender de `uiAccess`, certificados instalados, servicios ni controladores.

## Recuperación y diagnóstico

- Solo puede existir una instancia residente mediante un mutex con nombre.
- Cada espera de fotograma tiene un timeout de cinco segundos.
- Las capturas simultáneas se rechazan hasta que finaliza la anterior.
- El registro rota al superar 1 MiB.
- Registro actual: `%LOCALAPPDATA%\NativeHDRShot\NativeHDRShot.log`.
