# Solución de problemas

## `Impr Pant` no abre el selector

1. Comprueba que el icono de NativeHDRShot está en el área de notificación.
2. Abre **Configuración** y revisa el estado de `Impr Pant`. Si aparece sin protección, pulsa **Recuperar Impr Pant**.
3. Prueba `Ctrl + Mayús + F11`.
4. Cierra otras herramientas de captura que usen `Impr Pant`.
5. En Windows, revisa **Configuración → Accesibilidad → Teclado → Usar el botón Impr Pant para abrir Recortes de pantalla**.
6. Reinicia NativeHDRShot desde `%LOCALAPPDATA%\NativeHDRShot\NativeHDRShot.exe`.

El icono normal indica que el hook nativo protege `Impr Pant`. Un icono de advertencia indica que el hook no está activo; NativeHDRShot intentará mantener la captura mediante la reserva del atajo. Al arrancar, el registro indica el estado del hook, la reserva y el atajo alternativo.

## El selector aparece en otro monitor

El monitor se decide según la posición del cursor cuando se pulsa el atajo. Mueve el cursor al monitor deseado antes de comenzar.

## La captura parece demasiado oscura o clara

NativeHDRShot lee el nivel de blanco SDR del monitor en cada captura. Si acabas de modificar el brillo de contenido SDR en la configuración HDR de Windows, realiza una captura nueva. Actualiza también el controlador de la GPU si Windows devuelve un nivel incorrecto.

## No se crea ningún archivo

Comprueba:

```text
%LOCALAPPDATA%\NativeHDRShot\NativeHDRShot.log
```

La aplicación necesita permiso de escritura en la carpeta `Imágenes`. También puede fallar al capturar escritorios seguros, pantallas UAC, vídeo protegido por DRM o contenido excluido explícitamente de la captura.

## La captura no se puede pegar

NativeHDRShot muestra «sin portapapeles» y conserva el archivo si otra aplicación mantiene bloqueado el portapapeles durante todos los reintentos. Prueba de nuevo y consulta `NativeHDRShot.log`; una captura correcta queda publicada como `CF_DIBV5` y Windows proporciona conversiones para aplicaciones compatibles con otros formatos de bitmap.

## El icono no aparece después de actualizar

Finaliza cualquier instancia antigua y vuelve a ejecutar `install.cmd`. El instalador espera a que el proceso termine, reemplaza el ejecutable y recrea el acceso directo con el icono incrustado.

## Windows SmartScreen muestra una advertencia

El ejecutable distribuido no está firmado con un certificado comercial. Puedes verificar el código y compilarlo con `build.ps1`. El workflow de GitHub Actions también recompila el proyecto en Windows.

## Recopilar información para un issue

Incluye:

- versión de Windows;
- GPU y versión del controlador;
- disposición y escalado de monitores;
- si HDR está activo;
- pasos exactos para reproducir el fallo;
- las últimas líneas del registro, después de revisar que no contengan rutas que prefieras ocultar.
