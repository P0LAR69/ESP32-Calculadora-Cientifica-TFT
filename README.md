# ESP32 Calculadora Científica TFT (ILI9341 320x240)

**Calculadora científica completa** para ESP32 con pantalla ILI9341. Incluye soporte avanzado para números complejos, graficación interactiva, polinomios, integrales, estadística y matrices.

![Splash](images/splash.jpg)

## Características

### **Modos disponibles**
- **MATEMÁTICAS** — Expresiones complejas + `Ans` + historial
- **GRÁFICOS** — Graficación en tiempo real con **Zoom**, **Pan** y **Trace**
- **POLINOMIOS** — Raíces hasta grado 9 (Newton + deflación)
- **VARIABLES** — `a=`, `b=`, `c=`, `d=`, `f=`
- **INTEGRALES** — Simpson 1/3 (no bloqueante), falta revisarlo más a detalle 
- **TABLAS** — Tabla de valores
- **ESTADÍSTICA** — Media, mediana, desviación, cuartiles, skewness, histograma, este modo aun esta en trabajo
- **MATRICES** — Hasta 4×4, suma, multiplicación, determinante, transpuesta este modo aun esta en trabajo

### **Soporte de números complejos**
- `a+bi`, operaciones completas (`+`, `-`, `*`, `/`, `^`)
- `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`
- `sqrt`, `exp`, `ln`, `log10`, `log2`, `abs`, `arg`, `re`, `im`, `conj`
- `erf`, `erfc`, `gamma`, `fac`, `ncr`, `npr`

## Hardware

- ESP32 DevKit
- Pantalla ILI9341 320×240 (SPI)
- 4 botones (UP, DOWN, SELECT, MODE)
- Potenciómetro opcional (pin 32)

## Pines por defecto

| Función       | Pin  |
|---------------|------|
| TFT CS        | 15   |
| TFT DC        | 2    |
| TFT RST       | 4    |
| Botón UP      | 12   |
| Botón DOWN    | 14   |
| Botón SELECT  | 13   |
| Botón MODE    | 27   |
| Potenciómetro | 32   |

## Instalación

1. Clona el repositorio
2. Abre `Calculadora.ino` en Arduino IDE
3. Instala las librerías:
   - Adafruit GFX
   - Adafruit ILI9341
4. Selecciona placa **ESP32 Dev Module**
5. Sube el código
6. Abre el Monitor Serial (115200 baudios)

**Autor:** Jonathan Cabrea
