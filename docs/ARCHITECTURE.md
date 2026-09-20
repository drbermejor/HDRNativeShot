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
          └── Portapapeles PNG + CF_DIBV5 (sRGB) + CF_DIB 24 bits
```

## Componentes

### Ventana residente

Una ventana Win32 de solo mensajes gestiona el icono del área de notificación y recibe el resultado de los trabajos de captura. Un hook `WH_KEYBOARD_LL` intercepta `Impr Pant` sin bloquear el hilo dentro del callback: únicamente publica un mensaje privado a la ventana y devuelve inmediatamente.

`RegisterHotKey` mantiene en paralelo una reserva de `Impr Pant`. Si Windows retira silenciosamente el hook y la reserva recibe la siguiente pulsación, la aplicación continúa capturando, cambia el icono a advertencia y ofrece reinstalar el hook desde Configuración. Las combinaciones modificadas como `Alt + Impr Pant` y `Win + Impr Pant` se dejan pasar a Windows.

### Selector de región

Antes de abrir el selector se adquiere un fotograma de cada monitor. Cada imagen se convierte a SDR con los parámetros de su propia pantalla y después se coloca en un búfer que reproduce las coordenadas del escritorio virtual. Ese búfer se dibuja en una única ventana Win32 opaca, `WS_EX_TOPMOST`, que abarca todos los monitores y toma el primer plano. La cruz y el marco aparecen sobre la copia congelada, no sobre las superficies originales.

La ventana recibe el ratón de forma normal, sin depender de un hook global que un sistema anti-cheat pueda bloquear. Se libera temporalmente `ClipCursor`, se dibuja una cruz propia y se restaura la restricción anterior al terminar. Después se devuelve el primer plano a la ventana que estaba activa.

Mientras la selección está abierta, un temporizador de vida corta reafirma la posición del selector en la banda `TOPMOST` y recupera el primer plano si otra ventana intenta ocuparlo. Esta vigilancia desaparece junto con el selector y no permanece activa en segundo plano.

En modo región o pantalla, el recorte se extrae del búfer congelado y puede atravesar los límites entre monitores. Después se aplica, si está configurada, una reducción de resolución proporcional con el filtro Fant de WIC. Un clic menor de tres píxeles en modo región se ignora y mantiene abierto el selector.

El modo ventana enumera ventanas visibles, excluye ventanas minimizadas, ocultas por DWM y herramientas auxiliares, y usa sus límites extendidos para identificar la elección. Al cerrar el selector se adquiere la ventana mediante `IGraphicsCaptureItemInterop::CreateForWindow`, con el blanco SDR del monitor que la contiene. Esto evita incluir las ventanas que la tapen. El fotograma de ventana corresponde al momento de confirmar la elección, no al instante del escritorio congelado.

El retardo usa un temporizador Win32 cancelable antes de iniciar la adquisición. Los trabajadores de preparación y guardado pertenecen a la instancia residente y se unen antes de reutilizarse o destruirla. Las preferencias se congelan al iniciar cada operación.

### Adquisición

Para cada monitor se crea un dispositivo D3D11 en el adaptador que lo controla. `Windows.Graphics.Capture` entrega un fotograma flotante de 16 bits por componente. El recurso se copia a una textura staging legible por CPU y, tras convertirlo, se compone en sus coordenadas físicas dentro del escritorio virtual.

Los objetos D3D y la sesión se recrean en cada operación. Esto añade un coste pequeño, pero evita conservar dispositivos inválidos después de suspensión, cambio de monitor o reinicio del controlador gráfico.

### Conversión HDR a SDR

`DISPLAYCONFIG_SDR_WHITE_LEVEL` informa del multiplicador con el que Windows representa el blanco SDR dentro del espacio scRGB del monitor HDR. Los canales se normalizan por este valor antes de aplicar una curva de compresión suave a las luces que todavía superan el rango SDR.

La conversión de lineal a sRGB utiliza la función de transferencia estándar por canal. Se conserva la relación entre canales durante la compresión de luminancia para reducir cambios de tono.

### Portapapeles

Después de guardar el archivo, el búfer SDR BGRA final se publica en este orden: PNG registrado, `CF_DIBV5` con `LCS_sRGB` y alfa opaco, y `CF_DIB` clásico de 24 bits, de abajo hacia arriba y con filas alineadas a cuatro bytes. El PNG evita depender exclusivamente de la interpretación de DIBV5; el DIB clásico ofrece una representación sin alfa ni cabecera extendida. `CF_BITMAP` se sintetiza por Windows.

Los datos se preparan antes de abrir y vaciar el portapapeles. La memoria usa `GMEM_MOVEABLE` y Windows adquiere su propiedad únicamente tras un `SetClipboardData` correcto. Hay reintentos durante aproximadamente un segundo. Los fallos parciales permiten conservar los formatos publicados y se registran; un fallo de portapapeles no invalida el archivo guardado. No se publican rutas de archivos ni texto como sustitutos de la imagen.

### Codificación

Windows Imaging Component codifica el búfer SDR:

- PNG: `GUID_WICPixelFormat32bppBGRA`;
- JPEG: `GUID_WICPixelFormat24bppBGR` con `ImageQuality` y `JpegYCrCbSubsampling` explícitos (4:4:4 o 4:2:0).

Ambos formatos se etiquetan como sRGB. `src/image_output.h` comparte la codificación, el escalado y los formatos de portapapeles entre captura y editor. Las pruebas comprueban el muestreo real de los archivos JPEG, no solo el valor solicitado al códec.

### Editor

`src/capture_editor.h` implementa una ventana Win32 modeless y rasteriza las anotaciones mediante GDI+. La vista se adapta al espacio disponible y las coordenadas del ratón se convierten a píxeles de la imagen. El historial almacena imágenes para poder deshacer recortes y anotaciones (hasta 20 pasos y un presupuesto de 256 MiB, conservando al menos el último paso).

Copiar exporta los píxeles editados por la misma vía del portapapeles. Guardar como codifica primero un archivo temporal junto al destino y lo sustituye solo cuando la codificación termina. El original autoguardado se mantiene salvo que el usuario elija expresamente sobrescribirlo.

### Persistencia

Las preferencias se almacenan bajo `HKCU\Software\NativeHDRShot`. El ejecutable se instala en `%ProgramFiles%\NativeHDRShot` y una tarea programada vinculada al inicio de sesión lo ejecuta con el nivel de integridad más alto. Esto permite que el hook y el selector funcionen sobre procesos elevados sin depender de `uiAccess`, certificados instalados, servicios ni controladores.

## Recuperación y diagnóstico

- Solo puede existir una instancia residente mediante un mutex con nombre.
- Cada espera de fotograma tiene un timeout de cinco segundos.
- Los callbacks de captura mantienen su propio estado y referencias D3D, incluso si vence el timeout.
- Las capturas simultáneas se rechazan hasta que finaliza la anterior.
- El registro rota al superar 1 MiB.
- Registro actual: `%LOCALAPPDATA%\NativeHDRShot\NativeHDRShot.log`.
