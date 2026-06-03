# ESP32 Calculadora Científica TFT ILI9341

Calculadora científica avanzada para ESP32 con pantalla ILI9341 320x240.

## Características

- **Números complejos** completos (`a+bi`)
- **Graficación** interactiva con Zoom, Pan y modo Trace
- **Polinomios**: resolución numérica hasta grado 9
- **Integrales** numéricas (método Simpson)
- **Tablas** de valores
- **Estadística** completa (media, mediana, cuartiles, skewness, histograma)
- **Matrices** (hasta 4×4) con operaciones básicas
- Historial de expresiones y variable `Ans`
- Soporte de variables `{a}`, `{b}`, `{c}`, `{d}`, `{f}`

## Estado Actual del Proyecto

**Importante:**

- La **introducción y edición de expresiones matemáticas** se realiza actualmente a través del **Monitor Serial** (115200 baudios).
- Los botones físicos (UP, DOWN, SELECT, MODE) ya permiten navegar por los menús y cambiar de modo.
- La **entrada completa de expresiones usando solo los botones** (editor en pantalla) está en desarrollo y será implementada en futuras actualizaciones.

## Hardware Requerido

- ESP32 DevKit
- Pantalla TFT ILI9341 320×240
- 4 botones pulsadores
- (Opcional) Potenciómetro en pin 32

## Pines por defecto

| Función         | Pin  |
|-----------------|------|
| TFT CS          | 15   |
| TFT DC          | 2    |
| TFT RST         | 4    |
| Botón UP        | 12   |
| Botón DOWN      | 14   |
| Botón SELECT    | 13   |
| Botón MODE      | 27   |
| Potenciómetro   | 32   |

## Cómo usar

1. Abre el Monitor Serial a **115200 baudios**
2. Selecciona un modo desde el menú
3. Escribe expresiones y presiona **Enter**
4. Usa los botones para navegar entre modos

**Ejemplos:**
- `sin(pi/2)`
- `2+3i`
- `x^2 + 3*x - 4`
- `{a}*cos(x)`

---

**Autor:** Jonathan Cabrera  
**Estado:** Funcional (entrada por Serial)
