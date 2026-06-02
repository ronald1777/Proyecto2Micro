# Lumines Pthreads

Proyecto en C++ inspirado en Lumines para consola ASCII. La implementacion usa
Pthreads, mutex, semaforos y variables de condicion para ejecutar en paralelo la
caida de bloques, la linea de tiempo, el control del jugador, la deteccion de
combos, los bloques especiales, el puntaje y el renderizado.

## Requisitos

- Compilador C++ con soporte C++17.
- Pthreads disponible.
- En Linux/WSL/macOS: `g++` y `make`.
- En Windows con MinGW/TDM-GCC: `g++` y `mingw32-make`.

No usa librerias graficas externas. Todo se dibuja en consola con caracteres
ASCII y codigos ANSI.

## Compilar

### Linux, WSL o macOS

```bash
make
```

### Windows con MinGW/TDM-GCC

```powershell
mingw32-make
```

Tambien puedes compilar manualmente:

```bash
g++ -std=c++17 -Wall -Wextra -pedantic -O2 src/main.cpp -o lumines -pthread
```

En Windows el ejecutable generado se llama `lumines.exe`.

## Probar rapidamente

Ejecuta la prueba automatica de logica:

```bash
make self-test
```

En Windows:

```powershell
mingw32-make self-test
```

Resultado esperado:

```text
SELF_TEST_OK: deteccion, puntaje, bloque especial y linea de tiempo funcionan.
```

Esta prueba valida que el programa puede detectar un cuadrado 2x2, sumar puntos,
activar un bloque especial y limpiar celdas marcadas con la linea de tiempo.

## Ejecutar el juego

Linux, WSL o macOS:

```bash
make run
```

Windows:

```powershell
.\lumines.exe
```

Si usaste `mingw32-make`, tambien puedes ejecutar:

```powershell
mingw32-make run
```

## Controles

- `A` o flecha izquierda: mover bloque a la izquierda.
- `D` o flecha derecha: mover bloque a la derecha.
- `W` o flecha arriba: rotar el bloque 2x2.
- `S` o flecha abajo: bajar una fila.
- `ESPACIO`: soltar inmediatamente.
- `P`: pausar o continuar.
- `Q`: terminar la partida actual.

## Reglas implementadas

- El objetivo es alcanzar 50 puntos.
- El jugador tiene 3 vidas.
- Se pierde una vida cuando la pila de bloques alcanza la fila superior.
- Cada cuadrado 2x2 del mismo color suma puntos.
- Varias combinaciones en el mismo barrido de la linea de tiempo generan bono
  de cadena.
- La linea de tiempo recorre el tablero por columnas y elimina los bloques
  marcados.
- Existen dos modalidades:
  - Modo lento: caida y linea de tiempo moderadas.
  - Modo rapido: mayor velocidad y dificultad.
- Los bloques especiales `@@` y `$$` marcan regiones conectadas del mismo color.
- Al finalizar, se puede guardar el puntaje en `high_scores.txt`.

## Hilos principales

- `inputThread`: captura teclado sin bloquear el juego.
- `playerThread`: procesa comandos del jugador.
- `gravityThread`: controla la caida automatica del bloque activo.
- `timelineThread`: mueve la linea de tiempo y elimina celdas marcadas.
- `detectorThread`: detecta cuadrados 2x2 del mismo color.
- `specialThread`: activa bloques especiales y marca regiones conectadas.
- `scoreThread`: calcula puntos, bonos y condicion de victoria.
- `rendererThread`: actualiza la interfaz ASCII en consola.

## Sincronizacion

- `pthread_mutex_t stateMutex`: protege el tablero, pieza activa, puntaje,
  vidas, comandos y eventos.
- `sem_t commandSem`: despierta al hilo del jugador cuando hay una tecla nueva.
- `sem_t scoreSem`: despierta al hilo de puntaje cuando hay puntos pendientes.
- `pthread_cond_t`: se usa en la barrera de arranque y para notificar redibujos.
- Barrera de arranque: sincroniza todos los hilos para iniciar la partida al
  mismo tiempo.

## Archivos generados en ejecucion

- `high_scores.txt`: tabla local de mejores puntajes. Se crea automaticamente en
  la carpeta desde donde se ejecuta el juego.
