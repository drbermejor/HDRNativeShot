# Solución de problemas

## No puedo pegar en Steam u otra aplicación

La versión 0.5 publica PNG, DIBV5 y DIB de 24 bits. Comprueba que realmente está ejecutándose el binario actualizado: el instalador usa el ejecutable de `dist`, no uno antiguo que haya quedado abierto.

Prueba un recorte pequeño con el perfil **Ligera** y un destino que admita imágenes. Si falla, comprueba si la imagen puede pegarse en Paint; esto distingue un fallo general del portapapeles de una restricción del receptor. Conserva el archivo guardado para adjuntarlo manualmente si el destino no permite pegar imágenes. El registro indica si se publicó solo parte de los formatos o si otra aplicación mantuvo ocupado el portapapeles.

## Los archivos o los pegados pesan demasiado

Elige **Ligera** o reduce el lado máximo a 1280/1920 px. JPEG ajusta el archivo guardado; las aplicaciones que reciben el portapapeles pueden volver a codificarlo. El límite de resolución sí se aplica a ambas salidas. El peso exacto depende de la escena y PNG puede resultar más pequeño en interfaces con colores planos.

## Quiero capturar sin que aparezca el editor

Desmarca **Abrir editor después de capturar** en el menú de bandeja. Podrás abrir **Editar última captura** cuando lo necesites. Para desactivar capturas programadas elige **Retardo → Sin retardo**.

## `Impr Pant` no abre el selector

1. Comprueba que el icono de NativeHDRShot está en el área de notificación.
2. Abre **Configuración** y revisa el estado de `Impr Pant`. Si aparece sin protección, pulsa **Recuperar Impr Pant**.
3. Prueba `Ctrl + Mayús + F11`.
4. Cierra otras herramientas de captura que usen `Impr Pant`.
5. En Windows, revisa **Configuración → Accesibilidad → Teclado → Usar el botón Impr Pant para abrir Recortes de pantalla**.
6. Comprueba en el Programador de tareas que la tarea `NativeHDRShot` esté habilitada y ejecútala de nuevo.

El icono normal indica que el hook nativo protege `Impr Pant`. Un icono de advertencia indica que el hook no está activo; NativeHDRShot intentará mantener la captura mediante la reserva del atajo. Al arrancar, el registro indica el estado del hook, la reserva y el atajo alternativo.

## No funciona sobre una ventana ejecutada como administrador

Instala la versión 0.4.1 o posterior mediante `install.cmd` y acepta UAC. El registro de inicio debe terminar con `integridad=elevada`. Una ejecución manual con integridad estándar no puede interceptar entradas destinadas a procesos elevados debido al aislamiento de privilegios de Windows.

## El selector no cubre todos los monitores

Desde la versión 0.4.1 el selector compone y cubre el escritorio virtual completo. El registro debe indicar `Fotograma virtual compuesto con N monitores`. Si falta una pantalla, comprueba que Windows la tenga configurada como **Extender estas pantallas** y vuelve a iniciar NativeHDRShot tras conectarla.

## El juego desaparece o conserva el ratón

NativeHDRShot captura primero todos los monitores y abre una copia SDR congelada en primer plano. El registro debe mostrar `Fotograma virtual compuesto con N monitores`, `Fotograma congelado listo; abriendo selector` y `Selector congelado activo en primer plano`. Seleccionas sobre esa copia, de modo que no importa que una aplicación pierda el foco, se minimice o retenga su propio cursor. Al terminar se intenta devolver el foco a la ventana anterior.

La aplicación elevada puede trabajar sobre ventanas administrativas normales, pero no existe una API que eluda el escritorio seguro donde aparece la propia confirmación de UAC, DRM o una exclusión explícita de captura. Ese contenido puede seguir apareciendo negro por decisión de Windows o de la aplicación.

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
