# Plan de pruebas rapidas

Este archivo sirve para demostrar que el proyecto cumple la rubrica durante la
presentacion.

## 1. Compilacion

Comando:

```bash
make
```

En Windows:

```powershell
mingw32-make
```

Resultado esperado: se genera `lumines` o `lumines.exe` sin errores.

## 2. Prueba automatica

Comando:

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

## 3. Menu de inicio

Ejecutar el juego y verificar:

- Opcion 1 inicia modo lento.
- Opcion 2 inicia modo rapido.
- Opcion 3 abre instrucciones.
- Opcion 4 abre puntajes destacados.
- Opcion 5 sale del programa.

## 4. Juego en modo lento

Durante la partida:

- Mover con `A` y `D`.
- Rotar con `W`.
- Bajar con `S`.
- Soltar con `ESPACIO`.
- Pausar con `P`.
- Salir con `Q`.

Resultado esperado: el tablero se actualiza en tiempo real, la pieza cae sola y
la linea de tiempo barre el tablero por columnas.

## 5. Combos y puntaje

Formar un cuadrado 2x2 del mismo color.

Resultado esperado:

- El cuadrado se marca con `xx`.
- La linea de tiempo elimina las celdas marcadas.
- El puntaje aumenta.
- Si hay varios cuadrados en el mismo barrido, se registra bono de cadena.

## 6. Bloques especiales

Colocar un bloque especial `@@` o `$$` dentro de un grupo del mismo color.

Resultado esperado:

- El hilo de bloques especiales marca celdas conectadas del mismo color.
- La linea de tiempo elimina la region marcada.
- El puntaje recibe puntos extra.

## 7. Victoria y derrota

- Victoria: alcanzar 50 puntos.
- Derrota: perder 3 vidas o salir con `Q`.

Resultado esperado: al finalizar, el programa permite guardar el nombre y
actualiza `high_scores.txt`.
