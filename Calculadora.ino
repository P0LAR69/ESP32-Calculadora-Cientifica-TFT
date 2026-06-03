/*
 * CALCULADORA CIENTÍFICA ESP32 – v2.0 MEJORADA
 * ─────────────────────────────────────────────
 * Hardware:  ESP32 + ILI9341 320×240 TFT
 * Botones:   UP=12  DOWN=14  SELECT=13  MODE=27
 * Potenc.:   pin 32
 *
 * CORRECCIONES vs. v1:
 *  • Eliminadas funciones duplicadas findScaleExponent / normalizeValue.
 *  • Bug crítico en prepareTableData: doble incremento de numPoints.
 *  • Serial.print() removido de ISRs (crash seguro).
 *  • Botones con INPUT_PULLUP; no se usa Serial1 (conflicto con pin 32).
 *  • modoIntegral convertido a state-machine no bloqueante + Simpson 1/3.
 *  • Modo GRAPH usa te_compile/te_eval (8-10× más rápido).
 *  • Trace mode: cursor sobre la curva con valores X/Y.
 *  • Historial de 10 expresiones + variable Ans.
 *  • Estadística ampliada: Q1, Q3, skewness, histograma.
 *  • Modo MATRICES nuevo (matrices hasta 4×4, vectores hasta 1×6).
 *  • Splash screen al inicio.
 *  • Debounce mejorado; long-press en MODE para ayuda contextual.
 */

#include <math.h>
#include "tinyexpr_complex_mejorada.h"
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

/* ════════════════════════════════════════════════════════
 *  HARDWARE
 * ════════════════════════════════════════════════════════ */
#define TFT_DC  2
#define TFT_CS  15
#define TFT_RST 4
#define POT_PIN 32

#define BUTTON_UP     12
#define BUTTON_DOWN   14
#define BUTTON_SELECT 13
#define BUTTON_MODE   27

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

/* ════════════════════════════════════════════════════════
 *  PANTALLA
 * ════════════════════════════════════════════════════════ */
#define SCREEN_W  320
#define SCREEN_H  240
#define MARGIN_L   24
#define MARGIN_R   10
#define MARGIN_T   20
#define MARGIN_B   14
#define GRAPH_W   (SCREEN_W - MARGIN_L - MARGIN_R)
#define GRAPH_H   (SCREEN_H - MARGIN_T - MARGIN_B)
#define GRAPH_X0  MARGIN_L
#define GRAPH_Y0  MARGIN_T

/* ════════════════════════════════════════════════════════
 *  BOTONES – ISR
 * ════════════════════════════════════════════════════════ */
volatile bool upPressed     = false;
volatile bool downPressed   = false;
volatile bool selectPressed = false;
volatile bool modePressed   = false;

volatile unsigned long lastIrqUp  = 0, lastIrqDn  = 0;
volatile unsigned long lastIrqSel = 0, lastIrqMod = 0;
const unsigned long    DEBOUNCE   = 180; /* ms */

void IRAM_ATTR isrUp()     { unsigned long n=millis(); if(n-lastIrqUp >DEBOUNCE){upPressed    =true;lastIrqUp =n;} }
void IRAM_ATTR isrDown()   { unsigned long n=millis(); if(n-lastIrqDn >DEBOUNCE){downPressed  =true;lastIrqDn =n;} }
void IRAM_ATTR isrSelect() { unsigned long n=millis(); if(n-lastIrqSel>DEBOUNCE){selectPressed=true;lastIrqSel=n;} }
void IRAM_ATTR isrMode()   { unsigned long n=millis(); if(n-lastIrqMod>DEBOUNCE){modePressed  =true;lastIrqMod=n;} }

/* Consume y retorna el estado de un botón de forma atómica */
inline bool consumeUp()     { if(!upPressed)    return false; upPressed    =false; return true; }
inline bool consumeDown()   { if(!downPressed)  return false; downPressed  =false; return true; }
inline bool consumeSelect() { if(!selectPressed)return false; selectPressed=false; return true; }
inline bool consumeMode()   { if(!modePressed)  return false; modePressed  =false; return true; }

/* ════════════════════════════════════════════════════════
 *  MODOS DE OPERACIÓN
 * ════════════════════════════════════════════════════════ */
enum Mode {
    MODE_MENU, MODE_MATH, MODE_GRAPH, MODE_POLY,
    MODE_VARS, MODE_INT,  MODE_TABLE, MODE_STATS, MODE_MATRIX
};
Mode currentMode = MODE_MENU;

/* ════════════════════════════════════════════════════════
 *  MENÚ PRINCIPAL
 * ════════════════════════════════════════════════════════ */
const int TOTAL_ITEMS = 8;
const char* MENU_ITEMS[TOTAL_ITEMS] = {
    "MATEMATICAS", "GRAFICOS", "POLINOMIOS", "VARIABLES",
    "INTEGRALES",  "TABLAS",   "ESTADISTICA","MATRICES"
};
int menuSelection = 0;

/* ════════════════════════════════════════════════════════
 *  VARIABLES DE USUARIO  (a..f) + Ans
 * ════════════════════════════════════════════════════════ */
double userA = 0, userB = 0, userC = 0, userD = 0, userF = 0;
te_complex ansValue = {0.0, 0.0};          /* último resultado */

/* Historial de expresiones (ring buffer) */
#define HIST_SIZE 10
#define HIST_LEN  80
char  history[HIST_SIZE][HIST_LEN];
int   histCount = 0;
int   histIdx   = 0;   /* índice para navegación */

void histPush(const char *expr) {
    strncpy(history[histCount % HIST_SIZE], expr, HIST_LEN - 1);
    history[histCount % HIST_SIZE][HIST_LEN - 1] = '\0';
    histCount++;
    histIdx = histCount;
}

/* ════════════════════════════════════════════════════════
 *  BUFFER COMPARTIDO DE FUNCIÓN / ENTRADA SERIAL
 * ════════════════════════════════════════════════════════ */
char   functionBuffer[80];
String inputLine = "";
int    cursorPos = 0;

/* ════════════════════════════════════════════════════════
 *  PARÁMETROS DE GRÁFICA / TABLA
 * ════════════════════════════════════════════════════════ */
float startX = -10.0f, endX = 10.0f, stepX = 0.0f;

#define MAX_POINTS GRAPH_W
float x_values[MAX_POINTS];
float y_values[MAX_POINTS];
int   numPoints = 0;
float minY = 0.0f, maxY = 0.0f;

bool functionRead = false;
bool paramsRead   = false;

/* ════════════════════════════════════════════════════════
 *  MODO GRÁFICO – Trace e interacción
 * ════════════════════════════════════════════════════════ */
enum GraphMode { GMODE_ZOOM, GMODE_PAN, GMODE_TRACE };
GraphMode graphMode  = GMODE_ZOOM;
int       traceIdx   = 0;     /* índice del punto actual en el array */
bool      traceOn    = false;

te_expr *compiledExpr = NULL;  /* expresión compilada reutilizable */
te_complex xVar       = {0.0, 0.0};

/* ════════════════════════════════════════════════════════
 *  MODO TABLA
 * ════════════════════════════════════════════════════════ */
int  tablePage       = 0;
const int ROWS_PAGE  = 18;
bool tableNeedsRedraw = true;

/* ════════════════════════════════════════════════════════
 *  MODO INTEGRAL (state-machine no bloqueante)
 * ════════════════════════════════════════════════════════ */
enum IntState { INT_GET_A, INT_GET_B, INT_GET_FUNC, INT_COMPUTING, INT_DONE };
IntState intState     = INT_GET_A;
double   intA = 0, intB = 0;
bool     intLowerSet  = false, intUpperSet = false, intFuncSet = false;
/* Simpson acumulado por pasos para no bloquear */
int      simpsonStep  = 0;
const int SIMP_N      = 1000;   /* debe ser par */
double   simpsonSum   = 0;
double   simpsonH     = 0;
te_expr *simpsonExpr  = NULL;
te_complex simpX      = {0.0, 0.0};

/* ════════════════════════════════════════════════════════
 *  ESTADÍSTICA
 * ════════════════════════════════════════════════════════ */
#define MAX_STATS     100
#define STATS_VISIBLE   8
float statsData[MAX_STATS];
int   statsCount = 0, statsCursor = 0, statsViewOff = 0;
String statsInput = "";

enum StatsState { ST_MENU, ST_LIST, ST_RESULTS, ST_HISTOGRAM };
StatsState statsState = ST_MENU;
int        statsMenuCursor = 0;
#define STATS_MENU_N 5
const char* STATS_MENU_OPTS[STATS_MENU_N] = {
    "Ver datos", "1-Var Stats", "Histograma", "Limpiar", "Salir"
};

/* ════════════════════════════════════════════════════════
 *  MATRICES  (hasta 4 matrices 4×4)
 * ════════════════════════════════════════════════════════ */
#define MAT_MAX  4
#define MAT_DIM  4
float matrices[MAT_MAX][MAT_DIM][MAT_DIM];
int   matRows[MAT_MAX], matCols[MAT_MAX];

enum MatState { MAT_MENU, MAT_VIEW, MAT_EDIT, MAT_OPS };
MatState matState     = MAT_MENU;
int      matSel       = 0;    /* matriz seleccionada (0-3) */
int      matCurR = 0, matCurC = 0;
String   matInput     = "";
int      matMenuCursor = 0;
#define MAT_MENU_N 6
const char* MAT_MENU_OPTS[MAT_MENU_N] = {
    "Ver/Editar M1", "Ver/Editar M2", "Ver/Editar M3", "Ver/Editar M4",
    "Operaciones",   "Salir"
};

/* ════════════════════════════════════════════════════════
 *  UTILIDADES
 * ════════════════════════════════════════════════════════ */

float mapFloat(float v, float lo, float hi, float out_lo, float out_hi) {
    if (fabs(hi - lo) < 1e-30f) return out_lo;
    return (v - lo) * (out_hi - out_lo) / (hi - lo) + out_lo;
}

/* Devuelve el exponente de escala óptimo para mostrar 'value'. */
int bestExponent(double value) {
    if (value == 0.0 || !isfinite(value)) return 0;
    double av = fabs(value);
    if (av >= 0.01 && av < 1000.0) return 0;
    int e = (int)floor(log10(av));
    if (e >  100) return  100;
    if (e < -100) return -100;
    return e;
}

double scaleBy(double value, int exponent) {
    if (exponent == 0) return value;
    return value / pow(10.0, exponent);
}

/* Imprime un float con formato automático en TFT */
void printFmt(float v, int decimals = 4) {
    if (!isfinite(v)) { tft.print(isnan(v) ? "NaN" : "Inf"); return; }
    if (fabs(v) >= 1e6 || (fabs(v) < 1e-3 && v != 0)) {
        int e = bestExponent((double)v);
        tft.print(scaleBy(v, e), decimals > 2 ? 2 : decimals);
        tft.print("e"); tft.print(e);
    } else {
        tft.print(v, decimals);
    }
}

/* ════════════════════════════════════════════════════════
 *  SUSTITUCIÓN DE VARIABLES EN EXPRESIÓN
 *  Convierte "{a}" → valor numérico antes de parsear.
 * ════════════════════════════════════════════════════════ */
void substituteVars(String &s) {
    s.replace("{a}", String(userA, 8));
    s.replace("{b}", String(userB, 8));
    s.replace("{c}", String(userC, 8));
    s.replace("{d}", String(userD, 8));
    s.replace("{f}", String(userF, 8));
}

/* ════════════════════════════════════════════════════════
 *  BITMAP DEL SÍMBOLO INTEGRAL
 * ════════════════════════════════════════════════════════ */
const unsigned char bmpIntegral[] PROGMEM = {
    0x00,0x07, 0x00,0x13, 0x00,0x10, 0x00,0x20, 0x00,0x20, 0x00,0x20,
    0x00,0x20, 0x00,0x60, 0x00,0x60, 0x00,0x60, 0x00,0x40, 0x00,0x40,
    0x00,0x40, 0x00,0x40, 0x00,0x40, 0x00,0x40, 0x00,0xc0, 0x00,0xc0,
    0x00,0xc0, 0x00,0x80, 0x00,0x80, 0x00,0x80, 0x00,0x80, 0x01,0x80,
    0x00,0x19, 0x00,0x1c, 0x00,0x08, 0x00,0x00
};

void drawIntegralSymbol(int x, int y) {
    tft.drawBitmap(x, y, bmpIntegral, 18, 28, ILI9341_WHITE);
}

/* ════════════════════════════════════════════════════════
 *  SPLASH SCREEN
 * ════════════════════════════════════════════════════════ */
void splashScreen() {
    tft.fillScreen(ILI9341_BLACK);
    /* Borde decorativo */
    tft.drawRect(2,  2, SCREEN_W-4, SCREEN_H-4, ILI9341_CYAN);
    tft.drawRect(5,  5, SCREEN_W-10, SCREEN_H-10, ILI9341_BLUE);

    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(2);
    tft.setCursor(40, 30);
    tft.println("CALCULADORA");
    tft.setCursor(50, 55);
    tft.setTextColor(ILI9341_WHITE);
    tft.println("CIENTIFICA");
    tft.setCursor(90, 80);
    tft.setTextColor(ILI9341_YELLOW);
    tft.println("ESP32");

    tft.setTextSize(1);
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(30, 115);
    tft.println("v2.0  -  Numeros Complejos");
    tft.setCursor(30, 130);
    tft.println("Integrales  -  Matrices");
    tft.setCursor(30, 145);
    tft.println("Estadistica  -  Graficos");

    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(55, 175);
    tft.println("Funciones disponibles:");
    tft.setTextColor(0x07FF);
    tft.setCursor(15, 190);
    tft.println("sin cos tan asin acos atan");
    tft.setCursor(15, 202);
    tft.println("sinh cosh tanh asinh acosh");
    tft.setCursor(15, 214);
    tft.println("sqrt ln log exp gamma erf");

    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(65, 228);
    tft.println("Presione SELECT...");
    delay(200);
}

/* ════════════════════════════════════════════════════════
 *  MENÚ PRINCIPAL
 * ════════════════════════════════════════════════════════ */
void drawMenu() {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_CYAN);
    tft.setTextSize(2);
    tft.setCursor(30, 5);
    tft.println("MENU PRINCIPAL");

    tft.setTextSize(1);
    const int itemH = 26, startY = 40, bx = 12, bw = SCREEN_W - 24, bh = 20;

    for (int i = 0; i < TOTAL_ITEMS; i++) {
        int y = startY + i * itemH;
        if (i == menuSelection) {
            tft.fillRect(bx, y - 2, bw, bh, ILI9341_BLUE);
            tft.setTextColor(ILI9341_WHITE);
        } else {
            tft.setTextColor(0xC618); /* gris claro */
        }
        int16_t x1, y1; uint16_t w, h;
        tft.getTextBounds(MENU_ITEMS[i], 0, 0, &x1, &y1, &w, &h);
        tft.setCursor((SCREEN_W - w) / 2, y);
        tft.println(MENU_ITEMS[i]);
    }
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(4, SCREEN_H - 10);
    tft.print("Ans="); printFmt(ansValue.real, 3);
}

void processMenu() {
    if (consumeUp())   { menuSelection = (menuSelection - 1 + TOTAL_ITEMS) % TOTAL_ITEMS; drawMenu(); }
    if (consumeDown()) { menuSelection = (menuSelection + 1) % TOTAL_ITEMS;               drawMenu(); }

    if (consumeSelect()) {
        tft.fillScreen(ILI9341_BLACK);
        tft.setTextSize(1);
        tft.setTextColor(ILI9341_WHITE);
        tft.setCursor(0, 0);
        inputLine = ""; cursorPos = 0;

        switch (menuSelection) {
            case 0: currentMode = MODE_MATH;
                tft.println("MODO MATEMATICO");
                tft.println("Ingrese expresion (Ans disponible):");
                break;
            case 1: currentMode = MODE_GRAPH;
                functionRead = paramsRead = false;
                if (compiledExpr) { te_free(compiledExpr); compiledExpr = NULL; }
                traceOn = false; graphMode = GMODE_ZOOM;
                tft.println("MODO GRAFICOS");
                tft.println("Ingrese funcion (use 'x' minuscula):");
                break;
            case 2: currentMode = MODE_POLY;
                tft.println("MODO POLINOMIOS");
                tft.println("Ingrese grado (1-9):");
                break;
            case 3: currentMode = MODE_VARS;
                tft.println("MODO VARIABLES");
                tft.println("Formato: a=expresion");
                tft.print("a="); tft.print(userA,4);
                tft.print(" b="); tft.print(userB,4);
                tft.print(" c="); tft.println(userC,4);
                tft.print("d="); tft.print(userD,4);
                tft.print(" f="); tft.println(userF,4);
                break;
            case 4: currentMode = MODE_INT;
                intState = INT_GET_A; intLowerSet = intUpperSet = intFuncSet = false;
                simpsonStep = 0; simpsonSum = 0;
                if (simpsonExpr) { te_free(simpsonExpr); simpsonExpr = NULL; }
                tft.fillScreen(ILI9341_BLACK);
                tft.setTextColor(ILI9341_WHITE);
                tft.setCursor(60, 0); tft.println("MODO INTEGRAL");
                drawIntegralSymbol(20, 30);
                tft.setCursor(50, 40); tft.println("f(x) dx");
                tft.setCursor(10, 80); tft.println("Ingrese limite inferior a:");
                break;
            case 5: currentMode = MODE_TABLE;
                functionRead = paramsRead = false;
                tablePage = 0; tableNeedsRedraw = true;
                tft.println("MODO TABLA");
                tft.println("Ingrese funcion (use 'x'):");
                break;
            case 6: currentMode = MODE_STATS;
                statsState = ST_MENU; statsMenuCursor = 0;
                break;
            case 7: currentMode = MODE_MATRIX;
                matState = MAT_MENU; matMenuCursor = 0;
                break;
        }
    }
}

/* ════════════════════════════════════════════════════════
 *  MODO MATEMÁTICO
 * ════════════════════════════════════════════════════════ */
void processMathMode() {
    static unsigned long lastBlink = 0;
    static bool cursorVis = true;
    static int  histNav   = -1;  /* -1 = no navegando historial */

    /* Navegación del historial con botones */
    if (consumeUp()) {
        if (histCount > 0) {
            histNav = (histNav < 0) ? (histCount - 1) :
                       max(0, histNav - 1);
            int slot = histNav % HIST_SIZE;
            inputLine = String(history[slot]);
            cursorPos = inputLine.length();
            /* Redibujar prompt */
            tft.fillRect(0, 20, SCREEN_W, 20, ILI9341_BLACK);
            tft.setCursor(0, 20); tft.setTextSize(1);
            tft.setTextColor(ILI9341_CYAN);
            tft.print("> "); tft.print(inputLine);
        }
        return;
    }
    if (consumeDown()) {
        if (histNav >= 0) {
            histNav++;
            if (histNav >= histCount) { histNav = -1; inputLine = ""; cursorPos = 0; }
            else {
                int slot = histNav % HIST_SIZE;
                inputLine = String(history[slot]);
                cursorPos = inputLine.length();
            }
            tft.fillRect(0, 20, SCREEN_W, 20, ILI9341_BLACK);
            tft.setCursor(0, 20); tft.setTextSize(1);
            tft.setTextColor(ILI9341_CYAN);
            tft.print("> "); tft.print(inputLine);
        }
        return;
    }

    /* Lectura serial */
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\r') continue;
        histNav = -1;

        if (ch == '\n') {
            inputLine.trim();
            if (inputLine.length() == 0) return;

            /* Sustituir variables y ans */
            substituteVars(inputLine);
            inputLine.replace("ans", String(ansValue.real, 10));

            char buf[120];
            inputLine.toCharArray(buf, sizeof(buf));

            /* Registrar en historial */
            histPush(buf);

            int err = 0;
            /* Registrar x como variable (para compatibilidad con graph) */
            te_complex xDummy = {0.0, 0.0};
            te_variable lookups[] = {
                {"ans", &ansValue, TE_VARIABLE, 0},
                {"x",   &xDummy,  TE_VARIABLE, 0}
            };
            te_complex result = te_interp_vars(buf, lookups, 2, &err);

            tft.fillScreen(ILI9341_BLACK);
            tft.setCursor(0, 0); tft.setTextSize(1);
            tft.setTextColor(ILI9341_CYAN);
            tft.print(">> "); tft.println(buf);
            tft.drawLine(0, 16, SCREEN_W, 16, ILI9341_BLUE);

            if (err != 0) {
                tft.setTextColor(ILI9341_RED);
                tft.setCursor(0, 22); tft.print("ERROR posicion: "); tft.println(err);
                Serial.print("ERROR pos: "); Serial.println(err);
            } else if (!isfinite(result.real) || isnan(result.real)) {
                tft.setTextColor(ILI9341_YELLOW);
                tft.setCursor(0, 22); tft.println("Resultado: indefinido / infinito");
            } else {
                ansValue = result;

                tft.setTextColor(ILI9341_WHITE);
                tft.setTextSize(2);
                tft.setCursor(0, 25);

                bool hasImag = fabs(result.imag) > 1e-9;
                if (hasImag) {
                    tft.print("(");
                    tft.print(result.real, 5);
                    tft.print(result.imag >= 0 ? "+" : "");
                    tft.print(result.imag, 5);
                    tft.print("i)");
                } else {
                    tft.print(result.real, 7);
                }

                tft.setTextSize(1);
                tft.setTextColor(ILI9341_YELLOW);
                tft.setCursor(0, 65);
                tft.print("Ans = "); tft.print(result.real, 8);
                if (hasImag) { tft.print(" + "); tft.print(result.imag, 8); tft.print("i"); }

                /* Forma polar si es complejo */
                if (hasImag) {
                    tft.setCursor(0, 80);
                    tft.setTextColor(0x07FF);
                    tft.print("|z|="); tft.print(te_complex_abs(result), 6);
                    tft.print("  arg="); tft.print(te_complex_arg(result)*180.0/M_PI, 4); tft.print("°");
                }

                Serial.print("= "); Serial.print(result.real, 8);
                if (hasImag) { Serial.print(" + "); Serial.print(result.imag, 8); Serial.print("i"); }
                Serial.println();
            }

            tft.setTextColor(ILI9341_GREEN);
            tft.setCursor(0, 100); tft.println("Nueva expresion:");
            inputLine = ""; cursorPos = 0;
        }
        else if (ch == 8 || ch == 127) { /* Backspace */
            if (cursorPos > 0) { inputLine.remove(cursorPos - 1, 1); cursorPos--; }
        }
        else if (ch >= 32 && ch <= 126) {
            inputLine = inputLine.substring(0, cursorPos) + ch + inputLine.substring(cursorPos);
            cursorPos++;
        }
    }

    /* Parpadeo del cursor */
    unsigned long now = millis();
    if (now - lastBlink > 500) { lastBlink = now; cursorVis = !cursorVis; }

    /* Dibujar línea de entrada */
    tft.fillRect(0, 110, SCREEN_W, 14, ILI9341_BLACK);
    tft.setCursor(0, 110); tft.setTextSize(1); tft.setTextColor(ILI9341_GREEN);
    String before = inputLine.substring(0, cursorPos);
    String after  = inputLine.substring(cursorPos);
    tft.print("> "); tft.print(before);
    tft.setTextColor(cursorVis ? ILI9341_WHITE : ILI9341_BLACK);
    tft.print("|");
    tft.setTextColor(ILI9341_GREEN); tft.print(after);

    if (consumeSelect()) { currentMode = MODE_MENU; drawMenu(); }
}

/* ════════════════════════════════════════════════════════
 *  MODO GRÁFICO
 * ════════════════════════════════════════════════════════ */

/* Construye la tabla de variables para el parser de gráficos */
te_variable graphVarTable[] = {
    {"x", &xVar, TE_VARIABLE, 0},
    {"X", &xVar, TE_VARIABLE, 0}
};

/* Evalúa la función compilada en x; retorna NaN si hay error. */
float evalGraphFunc(float xf) {
    if (!compiledExpr) return NAN;
    xVar.real = (double)xf;
    xVar.imag = 0.0;
    te_complex z = te_eval(compiledExpr);
    if (!isfinite(z.real)) return NAN;
    return (float)z.real;
}

/* Dibuja la gráfica completa (ejes, función, trace) */
void drawGraph() {
    if (numPoints < 2) return;

    tft.fillScreen(ILI9341_BLACK);

    /* ── Calcular escalas Y ── */
    int yExp = bestExponent((double)fmax(fabs(minY), fabs(maxY)));
    float sMinY = (float)scaleBy(minY, yExp);
    float sMaxY = (float)scaleBy(maxY, yExp);
    float sStartX = startX, sEndX = endX;

    /* ── Marco ── */
    tft.drawRect(GRAPH_X0, GRAPH_Y0, GRAPH_W, GRAPH_H, ILI9341_WHITE);

    /* ── Grid punteado ── */
    uint16_t gridCol = tft.color565(60, 60, 60);
    for (int gi = 1; gi < 5; gi++) {
        int gx = GRAPH_X0 + gi * GRAPH_W / 5;
        int gy = GRAPH_Y0 + gi * GRAPH_H / 5;
        for (int p = GRAPH_Y0; p < GRAPH_Y0 + GRAPH_H; p += 4) tft.drawPixel(gx, p, gridCol);
        for (int p = GRAPH_X0; p < GRAPH_X0 + GRAPH_W; p += 4) tft.drawPixel(p, gy, gridCol);
    }

    /* ── Eje X=0 ── */
    if (startX <= 0.0f && endX >= 0.0f) {
        int x0px = GRAPH_X0 + (int)mapFloat(0, startX, endX, 0, GRAPH_W - 1);
        for (int p = GRAPH_Y0; p < GRAPH_Y0 + GRAPH_H; p += 2) tft.drawPixel(x0px, p, ILI9341_WHITE);
    }
    /* ── Eje Y=0 ── */
    if (sMinY <= 0.0f && sMaxY >= 0.0f) {
        int y0px = GRAPH_Y0 + (int)mapFloat(0, sMinY, sMaxY, GRAPH_H - 1, 0);
        for (int p = GRAPH_X0; p < GRAPH_X0 + GRAPH_W; p += 2) tft.drawPixel(p, y0px, ILI9341_WHITE);
    }

    /* ── Dibujar función ── */
    for (int i = 1; i < numPoints; i++) {
        if (isnan(y_values[i - 1]) || isnan(y_values[i])) continue;
        float sy0 = (float)scaleBy(y_values[i - 1], yExp);
        float sy1 = (float)scaleBy(y_values[i],     yExp);
        /* Detección de discontinuidad */
        if (fabs(sy1 - sy0) > (sMaxY - sMinY) * 0.5f) continue;
        int px0 = GRAPH_X0 + (int)mapFloat(x_values[i-1], sStartX, sEndX, 0, GRAPH_W-1);
        int py0 = GRAPH_Y0 + (int)mapFloat(sy0, sMinY, sMaxY, GRAPH_H-1, 0);
        int px1 = GRAPH_X0 + (int)mapFloat(x_values[i],   sStartX, sEndX, 0, GRAPH_W-1);
        int py1 = GRAPH_Y0 + (int)mapFloat(sy1, sMinY, sMaxY, GRAPH_H-1, 0);
        /* Clip vertical */
        if (py0 < GRAPH_Y0) py0 = GRAPH_Y0;
        if (py0 > GRAPH_Y0 + GRAPH_H - 1) py0 = GRAPH_Y0 + GRAPH_H - 1;
        if (py1 < GRAPH_Y0) py1 = GRAPH_Y0;
        if (py1 > GRAPH_Y0 + GRAPH_H - 1) py1 = GRAPH_Y0 + GRAPH_H - 1;
        tft.drawLine(px0, py0, px1, py1, ILI9341_GREEN);
    }

    /* ── Etiquetas eje X ── */
    tft.setTextSize(1);
    for (int i = 0; i <= 4; i++) {
        float xv = startX + i * (endX - startX) / 4.0f;
        int   xp = GRAPH_X0 + (int)mapFloat(xv, startX, endX, 0, GRAPH_W - 1);
        tft.drawLine(xp, GRAPH_Y0 + GRAPH_H, xp, GRAPH_Y0 + GRAPH_H + 3, ILI9341_WHITE);
        char lb[12]; snprintf(lb, sizeof(lb), "%.1f", xv);
        int tw = strlen(lb) * 6;
        tft.setCursor(max(GRAPH_X0, min(GRAPH_X0 + GRAPH_W - tw, xp - tw/2)),
                      GRAPH_Y0 + GRAPH_H + 4);
        tft.setTextColor(ILI9341_WHITE);
        tft.print(lb);
    }
    /* ── Etiquetas eje Y ── */
    for (int i = 0; i <= 4; i++) {
        float yv = sMinY + i * (sMaxY - sMinY) / 4.0f;
        int   yp = GRAPH_Y0 + (int)mapFloat(yv, sMinY, sMaxY, GRAPH_H - 1, 0);
        tft.drawLine(GRAPH_X0 - 3, yp, GRAPH_X0, yp, ILI9341_WHITE);
        char lb[12]; snprintf(lb, sizeof(lb), "%.2f", yv);
        int tw = strlen(lb) * 6;
        tft.setCursor(max(0, GRAPH_X0 - tw - 2), yp - 3);
        tft.setTextColor(ILI9341_WHITE);
        tft.print(lb);
    }
    if (yExp != 0) {
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(SCREEN_W - 60, 2);
        tft.print("Y*1e"); tft.print(yExp);
    }

    /* ── Trace ── */
    if (traceOn && numPoints > 0) {
        int ti = max(0, min(numPoints - 1, traceIdx));
        while (ti < numPoints - 1 && isnan(y_values[ti])) ti++;
        if (!isnan(y_values[ti])) {
            float tx = x_values[ti];
            float ty = (float)scaleBy(y_values[ti], yExp);
            int px = GRAPH_X0 + (int)mapFloat(tx, sStartX, sEndX, 0, GRAPH_W-1);
            int py = GRAPH_Y0 + (int)mapFloat(ty, sMinY,   sMaxY, GRAPH_H-1, 0);
            /* Crosshair amarillo */
            tft.drawLine(px - 6, py,     px + 6, py,     ILI9341_YELLOW);
            tft.drawLine(px,     py - 6, px,     py + 6, ILI9341_YELLOW);
            tft.drawCircle(px, py, 3, ILI9341_YELLOW);
            /* Valores X, Y */
            tft.fillRect(0, SCREEN_H - 14, SCREEN_W, 14, ILI9341_BLACK);
            tft.setCursor(0, SCREEN_H - 12);
            tft.setTextColor(ILI9341_YELLOW);
            tft.print("X="); tft.print(tx, 5);
            tft.print("  Y="); tft.print(y_values[ti], 5);
        }
    }

    /* ── Indicador de modo ── */
    tft.fillRect(0, 0, 80, 12, ILI9341_BLACK);
    tft.setCursor(0, 2); tft.setTextSize(1);
    tft.setTextColor(ILI9341_CYAN);
    const char* modeStr[] = {"ZOOM", "PAN", "TRACE"};
    tft.print(modeStr[graphMode]);
}

/* Recalcula todos los puntos de la función */
void recalcGraph() {
    numPoints = 0; minY = 1e18f; maxY = -1e18f;
    if (!compiledExpr) return;

    stepX = (endX - startX) / (MAX_POINTS - 1);
    for (int i = 0; i < MAX_POINTS; i++) {
        float xf = startX + i * stepX;
        float yf = evalGraphFunc(xf);
        x_values[i] = xf;
        y_values[i] = yf;
        if (!isnan(yf) && isfinite(yf)) {
            if (yf < minY) minY = yf;
            if (yf > maxY) maxY = yf;
        }
        numPoints++;
        if (i % 50 == 0) yield();
    }
    /* Margen del 8% */
    float rng = maxY - minY;
    if (rng < 1e-8f) { float c = (maxY+minY)*0.5f; minY = c-1; maxY = c+1; }
    else { minY -= rng*0.08f; maxY += rng*0.08f; }
}

void processGraphMode() {
    /* ── Cambio de modo ── */
    if (consumeMode()) {
        graphMode = (GraphMode)((graphMode + 1) % 3);
        traceOn   = (graphMode == GMODE_TRACE);
        if (traceOn && traceIdx < 0) traceIdx = 0;
        drawGraph();
        return;
    }

    /* ── Acción según modo ── */
    if (paramsRead) {
        if (graphMode == GMODE_TRACE) {
            bool moved = false;
            if (consumeUp())   { traceIdx = min(numPoints-1, traceIdx+2); moved = true; }
            if (consumeDown()) { traceIdx = max(0,            traceIdx-2); moved = true; }
            if (moved) drawGraph();
        } else {
            /* Zoom / Pan */
            if (upPressed || downPressed) {
                float center = (startX + endX) * 0.5f;
                float range  = endX - startX;
                if (graphMode == GMODE_ZOOM) {
                    range *= (upPressed ? 0.55f : 1.85f);
                } else {
                    float shift = range * 0.15f * (upPressed ? -1 : 1);
                    startX += shift; endX += shift;
                    upPressed = downPressed = false;
                    if (compiledExpr) { recalcGraph(); drawGraph(); }
                    return;
                }
                startX = center - range * 0.5f;
                endX   = center + range * 0.5f;
                upPressed = downPressed = false;
                if (compiledExpr) { recalcGraph(); drawGraph(); }
            }
        }
    }

    /* ── Lectura serial: función ── */
    if (!functionRead) {
        while (Serial.available()) {
            char ch = Serial.read();
            if (ch == '\n') {
                inputLine.trim();
                substituteVars(inputLine);
                if (inputLine.length() > 0) {
                    inputLine.toCharArray(functionBuffer, sizeof(functionBuffer));
                    /* Compilar expresión una sola vez */
                    if (compiledExpr) te_free(compiledExpr);
                    int err = 0;
                    compiledExpr = te_compile(functionBuffer, graphVarTable, 2, &err);
                    if (!compiledExpr) {
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setCursor(0,0); tft.setTextColor(ILI9341_RED);
                        tft.print("Error en funcion pos:"); tft.println(err);
                        Serial.print("Error pos:"); Serial.println(err);
                        inputLine = "";
                        return;
                    }
                    functionRead = true; paramsRead = false;
                    tft.fillScreen(ILI9341_BLACK);
                    tft.setCursor(0,0); tft.setTextColor(ILI9341_WHITE);
                    tft.println("Funcion:"); tft.println(functionBuffer);
                    tft.println("Rango x (startX/endX) o ENTER para -10/10:");
                }
                inputLine = "";
            } else if (ch == 8 && inputLine.length() > 0) inputLine.remove(inputLine.length()-1);
            else if (ch >= 32) inputLine += ch;
        }
        return;
    }

    /* ── Lectura serial: rango X ── */
    if (!paramsRead) {
        while (Serial.available()) {
            char ch = Serial.read();
            if (ch == '\n') {
                inputLine.trim();
                if (inputLine.length() == 0) {
                    startX = -10; endX = 10;
                } else {
                    int p = inputLine.indexOf('/');
                    if (p > 0) {
                        startX = inputLine.substring(0, p).toFloat();
                        endX   = inputLine.substring(p + 1).toFloat();
                    } else { startX = -10; endX = 10; }
                }
                if (startX >= endX) { startX = -10; endX = 10; }
                paramsRead = true; traceIdx = MAX_POINTS / 2;
                tft.fillScreen(ILI9341_BLACK);
                tft.setCursor(SCREEN_W/2 - 30, SCREEN_H/2 - 8);
                tft.setTextColor(ILI9341_YELLOW);
                tft.println("Calculando...");
                recalcGraph();
                drawGraph();
                inputLine = "";
            } else if (ch == 8 && inputLine.length() > 0) inputLine.remove(inputLine.length()-1);
            else if (ch >= 32) inputLine += ch;
        }
        return;
    }

    /* ── Salir ── */
    if (consumeSelect()) {
        if (compiledExpr) { te_free(compiledExpr); compiledExpr = NULL; }
        functionRead = paramsRead = false; traceOn = false;
        currentMode = MODE_MENU; drawMenu();
    }
}

/* ════════════════════════════════════════════════════════
 *  MODO POLINOMIOS
 * ════════════════════════════════════════════════════════ */
void processPolyMode() {
    static int    stage = 0, degree = 0, cIdx = 0;
    static double coeffs[10];

    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\n') {
            inputLine.trim();
            if (stage == 0) {
                degree = inputLine.toInt();
                if (degree < 1 || degree > 9) {
                    tft.fillScreen(ILI9341_BLACK); tft.setCursor(0,0);
                    tft.setTextColor(ILI9341_RED); tft.println("Grado invalido (1-9)");
                    inputLine = ""; return;
                }
                cIdx = 0; stage = 1;
                tft.fillScreen(ILI9341_BLACK); tft.setCursor(0,0);
                tft.setTextColor(ILI9341_WHITE);
                tft.println("Coeficientes: a0 (constante) primero");
                tft.print("a0 = ");
            } else if (stage == 1) {
                coeffs[cIdx++] = inputLine.toDouble();
                if (cIdx <= degree) {
                    tft.fillScreen(ILI9341_BLACK); tft.setCursor(0,0);
                    tft.setTextColor(ILI9341_WHITE);
                    tft.print("a"); tft.print(cIdx);
                    tft.print(" (x^"); tft.print(cIdx); tft.print(") = ");
                } else {
                    /* Resolver */
                    tft.fillScreen(ILI9341_BLACK); tft.setCursor(0,0);
                    tft.setTextColor(ILI9341_YELLOW);
                    tft.println("Calculando...");

                    te_complex roots[10];
                    int found = te_solve_poly(coeffs, degree, roots, degree);

                    tft.fillScreen(ILI9341_BLACK); tft.setCursor(0,0);
                    tft.setTextSize(1);
                    if (found > 0) {
                        tft.setTextColor(ILI9341_CYAN);
                        tft.print("Raices ("); tft.print(found); tft.println("):");
                        /* Ordenar por parte real */
                        for (int i = 0; i < found - 1; i++)
                            for (int j = i+1; j < found; j++)
                                if (roots[j].real < roots[i].real) {
                                    te_complex tmp = roots[i]; roots[i]=roots[j]; roots[j]=tmp;
                                }
                        int y = 18;
                        for (int i = 0; i < found; i++) {
                            tft.setCursor(0, y);
                            tft.setTextColor(ILI9341_WHITE);
                            tft.print("x"); tft.print(i+1); tft.print("=");
                            tft.print(roots[i].real, 5);
                            if (fabs(roots[i].imag) > 1e-6) {
                                tft.print(roots[i].imag >= 0 ? "+" : "");
                                tft.print(roots[i].imag, 5); tft.print("i");
                            }
                            y += 14;
                            Serial.print("x"); Serial.print(i+1); Serial.print("=");
                            Serial.print(roots[i].real, 8);
                            if (fabs(roots[i].imag) > 1e-6) {
                                Serial.print("+"); Serial.print(roots[i].imag, 8); Serial.print("i");
                            }
                            Serial.println();
                        }
                    } else {
                        tft.setTextColor(ILI9341_RED); tft.println("No se encontraron raices");
                    }
                    tft.setTextColor(ILI9341_YELLOW);
                    tft.setCursor(0, SCREEN_H - 12);
                    tft.println("Nuevo grado?  SELECT=menu");
                    stage = 0;
                }
            }
            inputLine = "";
        } else if (ch == 8 && inputLine.length() > 0) inputLine.remove(inputLine.length()-1);
        else if (ch >= 32) inputLine += ch;
    }

    if (consumeSelect()) { stage = 0; currentMode = MODE_MENU; drawMenu(); }
}

/* ════════════════════════════════════════════════════════
 *  MODO VARIABLES
 * ════════════════════════════════════════════════════════ */
void processVarsMode() {
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\n' || ch == '\r') {
            inputLine.trim();
            if (inputLine.length() == 0) { inputLine = ""; return; }
            int eq = inputLine.indexOf('=');
            if (eq <= 0) {
                Serial.println("Formato: a=expresion");
                inputLine = ""; return;
            }
            String vname = inputLine.substring(0, eq); vname.trim();
            String vexpr = inputLine.substring(eq + 1); vexpr.trim();

            char exprBuf[100]; vexpr.toCharArray(exprBuf, sizeof(exprBuf));
            int err = 0;
            te_complex res = te_interp(exprBuf, &err);
            if (err != 0) {
                Serial.print("ERROR en posicion: "); Serial.println(err);
            } else {
                double val = res.real;
                bool   ok  = true;
                if      (vname == "a") userA = val;
                else if (vname == "b") userB = val;
                else if (vname == "c") userC = val;
                else if (vname == "d") userD = val;
                else if (vname == "f") userF = val;
                else { Serial.print("Variable desconocida: "); Serial.println(vname); ok = false; }
                if (ok) {
                    Serial.print(vname); Serial.print(" = "); Serial.println(val, 8);
                    tft.fillScreen(ILI9341_BLACK); tft.setCursor(0,0);
                    tft.setTextColor(ILI9341_CYAN); tft.println("VARIABLES:");
                    tft.setTextColor(ILI9341_WHITE);
                    tft.print("a="); tft.print(userA,5); tft.print("  b="); tft.print(userB,5);
                    tft.print("  c="); tft.println(userC,5);
                    tft.print("d="); tft.print(userD,5); tft.print("  f="); tft.println(userF,5);
                }
            }
            inputLine = "";
        } else if (ch == 8 && inputLine.length() > 0) inputLine.remove(inputLine.length()-1);
        else if (ch >= 32) inputLine += ch;
    }
    if (consumeSelect()) { currentMode = MODE_MENU; drawMenu(); }
}

/* ════════════════════════════════════════════════════════
 *  MODO INTEGRAL  (state-machine no bloqueante, Simpson 1/3)
 * ════════════════════════════════════════════════════════ */
void processIntegral() {
    /* Paso de cálculo Simpson (llamado repetidamente desde loop) */
    if (intState == INT_COMPUTING && simpsonExpr) {
        const int BATCH = 20; /* pasos por llamada al loop */
        for (int b = 0; b < BATCH && simpsonStep <= SIMP_N; b++) {
            int k = simpsonStep;
            simpX.real = intA + k * simpsonH;
            simpX.imag = 0.0;
            te_complex fk = te_eval(simpsonExpr);

            if (k == 0 || k == SIMP_N) {
                simpsonSum += fk.real;
            } else {
                simpsonSum += (k % 2 == 0 ? 2.0 : 4.0) * fk.real;
            }
            simpsonStep++;
        }

        /* Actualizar barra de progreso */
        int prog = (int)((float)simpsonStep / SIMP_N * (SCREEN_W - 20));
        tft.fillRect(10, SCREEN_H/2 + 20, prog, 8, ILI9341_GREEN);

        if (simpsonStep > SIMP_N) {
            double result = simpsonSum * simpsonH / 3.0;
            te_free(simpsonExpr); simpsonExpr = NULL;
            intState = INT_DONE;

            tft.fillScreen(ILI9341_BLACK);
            tft.setCursor(10, 10); tft.setTextColor(ILI9341_CYAN); tft.setTextSize(1);
            tft.println("INTEGRAL (Simpson 1/3):");
            tft.setCursor(10, 30); tft.setTextColor(ILI9341_WHITE); tft.setTextSize(2);
            tft.println(result, 7);
            tft.setTextSize(1);
            tft.setCursor(10, 65); tft.setTextColor(ILI9341_YELLOW);
            tft.print("a="); tft.print(intA,5);
            tft.print("  b="); tft.print(intB,5);
            tft.setCursor(10, 80); tft.print("f(x)="); tft.println(functionBuffer);
            tft.setCursor(10, 100); tft.print("n="); tft.print(SIMP_N);
            tft.print(" pasos, h="); tft.print(simpsonH, 6);
            tft.setCursor(0, SCREEN_H - 12);
            tft.setTextColor(ILI9341_GREEN);
            tft.println("OK=nuevo  SELECT=menu");

            ansValue.real = result; ansValue.imag = 0;
            Serial.print("Integral = "); Serial.println(result, 10);
        }
        return;
    }

    /* Lectura serial para cada etapa */
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\n') {
            inputLine.trim();
            if (intState == INT_GET_A) {
                intA = inputLine.toDouble();
                intLowerSet = true; intState = INT_GET_B;
                tft.fillRect(10, 80, 200, 14, ILI9341_BLACK);
                tft.setCursor(10, 80); tft.setTextColor(ILI9341_GREEN);
                tft.print("a = "); tft.println(intA, 5);
                tft.setCursor(10, 96); tft.setTextColor(ILI9341_WHITE);
                tft.println("Ingrese limite superior b:");
            } else if (intState == INT_GET_B) {
                intB = inputLine.toDouble();
                intUpperSet = true; intState = INT_GET_FUNC;
                tft.fillRect(10, 10, 130, 14, ILI9341_BLACK);
                tft.setCursor(10, 10); tft.setTextColor(ILI9341_GREEN);
                tft.print("b = "); tft.println(intB, 5);
                tft.setCursor(10, 110); tft.setTextColor(ILI9341_WHITE);
                tft.println("Ingrese f(x):");
            } else if (intState == INT_GET_FUNC) {
                inputLine.toCharArray(functionBuffer, sizeof(functionBuffer));
                /* Compilar con variable x */
                if (simpsonExpr) te_free(simpsonExpr);
                int err = 0;
                te_variable simpVars[] = {{"x", &simpX, TE_VARIABLE, 0}};
                simpsonExpr = te_compile(functionBuffer, simpVars, 1, &err);
                if (!simpsonExpr) {
                    tft.setCursor(10, 130); tft.setTextColor(ILI9341_RED);
                    tft.print("Error en funcion pos:"); tft.println(err);
                    intState = INT_GET_FUNC;
                } else {
                    intFuncSet = true; intState = INT_COMPUTING;
                    simpsonStep = 0; simpsonSum = 0;
                    simpsonH = (intB - intA) / SIMP_N;
                    tft.fillScreen(ILI9341_BLACK);
                    tft.setCursor(10, SCREEN_H/2 - 10);
                    tft.setTextColor(ILI9341_YELLOW);
                    tft.println("Calculando integral...");
                    tft.setCursor(10, SCREEN_H/2 + 10); tft.setTextColor(ILI9341_WHITE);
                    tft.println("[                              ]");
                    Serial.println("Calculando...");
                }
            } else if (intState == INT_DONE) {
                intState = INT_GET_A;
                intLowerSet = intUpperSet = intFuncSet = false;
                tft.fillScreen(ILI9341_BLACK);
                tft.setCursor(60, 0); tft.println("MODO INTEGRAL");
                drawIntegralSymbol(20, 30);
                tft.setCursor(10, 80); tft.println("Ingrese limite inferior a:");
            }
            inputLine = "";
        } else if (ch == 8 && inputLine.length() > 0) inputLine.remove(inputLine.length()-1);
        else if (ch >= 32) inputLine += ch;
    }

    if (consumeSelect()) {
        if (simpsonExpr) { te_free(simpsonExpr); simpsonExpr = NULL; }
        intState = INT_GET_A;
        currentMode = MODE_MENU; drawMenu();
    }
}

/* ════════════════════════════════════════════════════════
 *  MODO TABLA
 * ════════════════════════════════════════════════════════ */
te_complex tableX = {0.0, 0.0};
te_expr   *tableExpr = NULL;

void prepareTableData() {
    numPoints = 0;
    if (!tableExpr) return;
    stepX = (endX > startX) ? ((endX - startX) / (MAX_POINTS - 1)) : 0.1f;
    for (float xf = startX; xf <= endX + stepX * 0.01f && numPoints < MAX_POINTS; xf += stepX) {
        tableX.real = (double)xf;
        tableX.imag = 0.0;
        te_complex z = te_eval(tableExpr);
        x_values[numPoints] = xf;
        if (isfinite(z.real)) y_values[numPoints] = (float)z.real;
        else                   y_values[numPoints] = NAN;
        numPoints++;
        if (numPoints % 100 == 0) yield();
    }
}

void drawTablePage() {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(1); tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(0, 0);
    tft.print("Idx"); tft.setCursor(32, 0); tft.print("X"); tft.setCursor(130, 0); tft.print("Y");
    tft.drawLine(0, 10, SCREEN_W, 10, ILI9341_BLUE);

    int start = tablePage * ROWS_PAGE;
    for (int i = 0; i < ROWS_PAGE; i++) {
        int idx = start + i;
        if (idx >= numPoints) break;
        int y = 14 + i * 12;
        tft.setCursor(0, y); tft.setTextColor(ILI9341_WHITE);
        char buf[40];
        snprintf(buf, sizeof(buf), "%3d  %10.4f  ", idx, x_values[idx]);
        tft.print(buf);
        if (isnan(y_values[idx])) tft.print("---");
        else { snprintf(buf, sizeof(buf), "%10.4f", y_values[idx]); tft.print(buf); }
    }
    int maxPage = (numPoints - 1) / ROWS_PAGE;
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(0, SCREEN_H - 10);
    tft.print("Pag "); tft.print(tablePage+1); tft.print("/"); tft.print(maxPage+1);
    tft.print("  SELECT=menu");
}

void processTable() {
    if (!functionRead) {
        while (Serial.available()) {
            char ch = Serial.read();
            if (ch == '\n') {
                inputLine.trim();
                substituteVars(inputLine);
                if (inputLine.length() > 0) {
                    inputLine.toCharArray(functionBuffer, sizeof(functionBuffer));
                    if (tableExpr) te_free(tableExpr);
                    int err = 0;
                    te_variable tv = {"x", &tableX, TE_VARIABLE, 0};
                    tableExpr = te_compile(functionBuffer, &tv, 1, &err);
                    if (!tableExpr) {
                        tft.setCursor(0,0); tft.setTextColor(ILI9341_RED);
                        tft.print("Error pos:"); tft.println(err);
                    } else {
                        functionRead = true;
                        tft.fillScreen(ILI9341_BLACK);
                        tft.setCursor(0,0); tft.setTextColor(ILI9341_WHITE);
                        tft.println(functionBuffer);
                        tft.println("Rango: startX/stepX/endX");
                    }
                }
                inputLine = "";
            } else if (ch == 8 && inputLine.length() > 0) inputLine.remove(inputLine.length()-1);
            else if (ch >= 32) inputLine += ch;
        }
        return;
    }

    if (!paramsRead) {
        while (Serial.available()) {
            char ch = Serial.read();
            if (ch == '\n') {
                inputLine.trim();
                int p1 = inputLine.indexOf('/');
                int p2 = (p1 >= 0) ? inputLine.indexOf('/', p1+1) : -1;
                if (p1 > 0 && p2 > p1) {
                    startX = inputLine.substring(0, p1).toFloat();
                    stepX  = inputLine.substring(p1+1, p2).toFloat();
                    endX   = inputLine.substring(p2+1).toFloat();
                } else { startX=-10; stepX=1; endX=10; }
                if (stepX <= 0) stepX = (endX - startX) / 100;
                paramsRead = true; tablePage = 0;
                prepareTableData();
                tableNeedsRedraw = true;
                inputLine = "";
            } else if (ch == 8 && inputLine.length() > 0) inputLine.remove(inputLine.length()-1);
            else if (ch >= 32) inputLine += ch;
        }
        return;
    }

    if (consumeUp())   { if (tablePage > 0) { tablePage--; tableNeedsRedraw = true; } }
    if (consumeDown()) { int mx=(numPoints-1)/ROWS_PAGE; if(tablePage<mx){tablePage++;tableNeedsRedraw=true;} }
    if (consumeSelect()) {
        if (tableExpr) { te_free(tableExpr); tableExpr = NULL; }
        functionRead = paramsRead = false; tablePage = 0;
        currentMode = MODE_MENU; drawMenu(); return;
    }
    if (tableNeedsRedraw) { tableNeedsRedraw = false; drawTablePage(); }
}

/* ════════════════════════════════════════════════════════
 *  CÁLCULOS ESTADÍSTICOS
 * ════════════════════════════════════════════════════════ */

/* Ordena una copia del array y la retorna en 'sorted' */
static void statSort(float *sorted, int n) {
    for (int i = 0; i < n - 1; i++)
        for (int j = 0; j < n - i - 1; j++)
            if (sorted[j] > sorted[j+1]) { float t=sorted[j]; sorted[j]=sorted[j+1]; sorted[j+1]=t; }
}

float statMean() {
    if (!statsCount) return 0;
    double s = 0; for (int i=0;i<statsCount;i++) s+=statsData[i];
    return (float)(s / statsCount);
}
float statVariance(bool population = false) {
    if (statsCount < 2) return 0;
    float m = statMean(); double s = 0;
    for (int i=0;i<statsCount;i++) { float d=statsData[i]-m; s+=d*d; }
    return (float)(s / (population ? statsCount : statsCount - 1));
}
float statStd(bool pop=false) { return sqrtf(statVariance(pop)); }

float statMedian() {
    if (!statsCount) return 0;
    float tmp[MAX_STATS];
    memcpy(tmp, statsData, statsCount * sizeof(float));
    statSort(tmp, statsCount);
    int n = statsCount;
    return (n%2==0) ? (tmp[n/2-1]+tmp[n/2])*0.5f : tmp[n/2];
}

/* Cuartil por método de interpolación lineal */
float statQuartile(float pct) {
    if (statsCount < 2) return statMean();
    float tmp[MAX_STATS];
    memcpy(tmp, statsData, statsCount * sizeof(float));
    statSort(tmp, statsCount);
    float pos = pct * (statsCount - 1);
    int   lo  = (int)pos;
    float frac= pos - lo;
    if (lo >= statsCount - 1) return tmp[statsCount - 1];
    return tmp[lo] * (1.0f - frac) + tmp[lo + 1] * frac;
}

float statMin() { if(!statsCount)return 0; float m=statsData[0]; for(int i=1;i<statsCount;i++) if(statsData[i]<m)m=statsData[i]; return m; }
float statMax() { if(!statsCount)return 0; float m=statsData[0]; for(int i=1;i<statsCount;i++) if(statsData[i]>m)m=statsData[i]; return m; }
float statSum() { double s=0; for(int i=0;i<statsCount;i++) s+=statsData[i]; return (float)s; }
float statSum2(){ double s=0; for(int i=0;i<statsCount;i++) s+=statsData[i]*statsData[i]; return (float)s; }

/* Skewness de Pearson (3ª momento) */
float statSkewness() {
    if (statsCount < 3) return 0;
    float m  = statMean(), s = statStd();
    if (s < 1e-10f) return 0;
    double sk = 0;
    for (int i=0;i<statsCount;i++) { float d=(statsData[i]-m)/s; sk+=d*d*d; }
    return (float)(sk / statsCount);
}

/* ── Dibujo estadística ── */
void drawStatsList() {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(1); tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(0,0); tft.print("ESTADISTICA  n=");
    tft.print(statsCount); tft.print("/"); tft.print(MAX_STATS);

    if (statsCount > STATS_VISIBLE) {
        int pg  = statsViewOff / STATS_VISIBLE + 1;
        int tpg = (statsCount + STATS_VISIBLE - 1) / STATS_VISIBLE;
        tft.setCursor(180, 0); tft.print("P"); tft.print(pg); tft.print("/"); tft.print(tpg);
    }
    tft.drawLine(0, 12, SCREEN_W, 12, ILI9341_BLUE);

    for (int i = 0; i < STATS_VISIBLE; i++) {
        int idx = statsViewOff + i;
        if (idx >= statsCount && idx != statsCursor) break;
        int y = 16 + i * 14;
        if (idx == statsCursor) {
            tft.fillRect(0, y-1, SCREEN_W, 13, ILI9341_BLUE);
            tft.setTextColor(ILI9341_BLACK);
        } else tft.setTextColor(ILI9341_WHITE);
        tft.setCursor(2, y);
        if (idx < statsCount) {
            tft.print("["); if(idx<10)tft.print("0"); tft.print(idx); tft.print("] ");
            printFmt(statsData[idx]);
        } else {
            tft.print("[+] agregar aqui");
        }
    }

    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(2, SCREEN_H - 36); tft.println("Serial: numero+ENTER | R=borrar");
    tft.setCursor(2, SCREEN_H - 24); tft.println("UP/DN=mover  SELECT=menu");
    if (statsInput.length() > 0) {
        tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, 0x2945);
        tft.setTextColor(ILI9341_WHITE);
        tft.setCursor(2, SCREEN_H - 10);
        tft.print("> "); tft.print(statsInput);
    }
}

void drawStatsResults() {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(1); tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(0,0); tft.println("1-VAR ESTADISTICA");
    tft.drawLine(0,10,SCREEN_W,10,ILI9341_BLUE);

    if (!statsCount) {
        tft.setTextColor(ILI9341_RED); tft.setCursor(0,14); tft.println("Sin datos");
        return;
    }

    int y = 14;
    #define SROW(label, val, dec) \
        tft.setCursor(0, y); tft.setTextColor(ILI9341_WHITE); tft.print(label); \
        tft.setTextColor(ILI9341_YELLOW); \
        if (dec == 0) tft.println((long)(val)); else tft.println((float)(val), dec); \
        y += 13;

    SROW("n        = ", (int)statsCount, 0)
    SROW("Sx       = ", statSum(),  4)
    SROW("Sx^2     = ", statSum2(), 4)
    SROW("Media    = ", statMean(), 5)
    SROW("Mediana  = ", statMedian(), 5)
    SROW("StdDev S = ", statStd(false), 5)
    SROW("StdDev P = ", statStd(true),  5)
    SROW("Var S    = ", statVariance(false), 5)
    SROW("Min      = ", statMin(), 5)
    SROW("Q1       = ", statQuartile(0.25f), 5)
    SROW("Q3       = ", statQuartile(0.75f), 5)
    SROW("Max      = ", statMax(), 5)
    SROW("Rango    = ", statMax()-statMin(), 5)
    SROW("Skewness = ", statSkewness(), 5)
    #undef SROW

    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(0, SCREEN_H - 10); tft.println("SELECT = volver");
}

void drawHistogram() {
    if (statsCount < 2) {
        tft.fillScreen(ILI9341_BLACK);
        tft.setCursor(0,0); tft.setTextColor(ILI9341_RED);
        tft.println("Minimo 2 datos para histograma");
        return;
    }
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(1); tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(0,0); tft.println("HISTOGRAMA");

    const int BINS = 10;
    float lo = statMin(), hi = statMax();
    if (fabs(hi - lo) < 1e-10f) hi = lo + 1;
    float bw = (hi - lo) / BINS;
    int   cnt[BINS] = {0};
    int   maxCnt    = 0;

    for (int i = 0; i < statsCount; i++) {
        int b = (int)((statsData[i] - lo) / bw);
        if (b >= BINS) b = BINS - 1;
        if (b < 0)     b = 0;
        cnt[b]++;
        if (cnt[b] > maxCnt) maxCnt = cnt[b];
    }

    const int GX = 24, GY = 14, GW = SCREEN_W - GX - 4, GH = SCREEN_H - GY - 20;
    int bpx = GW / BINS;

    for (int i = 0; i < BINS; i++) {
        int bh = (maxCnt > 0) ? (cnt[i] * GH / maxCnt) : 0;
        int x  = GX + i * bpx;
        int y  = GY + GH - bh;
        tft.fillRect(x + 1, y, bpx - 2, bh, ILI9341_BLUE);
        tft.drawRect(x + 1, y, bpx - 2, bh, ILI9341_CYAN);
        /* Valor de la barra */
        if (cnt[i] > 0) {
            tft.setCursor(x + 2, y - 10); tft.setTextColor(ILI9341_WHITE);
            tft.print(cnt[i]);
        }
    }
    tft.drawRect(GX, GY, GW, GH, ILI9341_WHITE);

    /* Etiquetas eje X */
    tft.setTextColor(ILI9341_YELLOW);
    for (int i = 0; i <= BINS; i++) {
        float val = lo + i * bw;
        int   xp  = GX + i * bpx;
        tft.setCursor(max(0, xp - 10), GY + GH + 2);
        if (fabs(val) < 100) tft.print(val, 1); else tft.print((int)val);
    }
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(0, SCREEN_H - 10); tft.println("SELECT = volver");
}

void drawStatsMenu() {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(2); tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(20, 5); tft.println("ESTADISTICA");
    tft.setTextSize(1);
    for (int i = 0; i < STATS_MENU_N; i++) {
        int y = 40 + i * 26;
        if (i == statsMenuCursor) {
            tft.fillRect(10, y-2, SCREEN_W-20, 20, ILI9341_BLUE);
            tft.setTextColor(ILI9341_BLACK);
        } else tft.setTextColor(ILI9341_WHITE);
        tft.setCursor(15, y); tft.println(STATS_MENU_OPTS[i]);
    }
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(2, SCREEN_H - 10); tft.print("n="); tft.print(statsCount);
    if (statsCount > 0) {
        tft.print("  x̄="); tft.print(statMean(), 3);
        tft.print("  σ="); tft.print(statStd(), 3);
    }
}

void processStatsSerial() {
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (!statsInput.length()) return;
            statsInput.trim();
            String up = statsInput; up.toUpperCase();
            if (up == "R" || up == "DEL") {
                if (statsCount > 0 && statsCursor < statsCount) {
                    for (int i = statsCursor; i < statsCount-1; i++) statsData[i]=statsData[i+1];
                    statsCount--;
                    if (statsCursor >= statsCount && statsCursor > 0) statsCursor--;
                    Serial.print("Borrado. n="); Serial.println(statsCount);
                }
            } else {
                float v = statsInput.toFloat();
                bool ok = (statsInput == "0") || (v != 0.0f) ||
                          statsInput.indexOf('.') >= 0 || statsInput.indexOf('-') >= 0;
                if (ok && statsCount < MAX_STATS) {
                    if (statsCursor >= statsCount) {
                        statsData[statsCount++] = v;
                        statsCursor = statsCount - 1;
                    } else {
                        statsData[statsCursor] = v;
                    }
                    Serial.print("Dato["); Serial.print(statsCursor); Serial.print("]="); Serial.println(v,4);
                    if (statsCursor >= statsViewOff + STATS_VISIBLE)
                        statsViewOff = statsCursor - STATS_VISIBLE + 1;
                } else Serial.println("Error o lista llena");
            }
            statsInput = "";
            drawStatsList();
        } else if (ch == 8 && statsInput.length() > 0) {
            statsInput.remove(statsInput.length() - 1); drawStatsList();
        } else if (ch >= 32 && statsInput.length() < 20) {
            statsInput += ch; drawStatsList();
        }
    }
}

void modoEstadistico() {
    static bool firstEntry = true;
    if (firstEntry) {
        firstEntry = false; statsState = ST_MENU; statsMenuCursor = 0;
        drawStatsMenu();
    }
    if (currentMode != MODE_STATS) { firstEntry = true; return; }

    switch (statsState) {
        case ST_MENU:
            if (consumeUp())   { statsMenuCursor=(statsMenuCursor-1+STATS_MENU_N)%STATS_MENU_N; drawStatsMenu(); }
            if (consumeDown()) { statsMenuCursor=(statsMenuCursor+1)%STATS_MENU_N;               drawStatsMenu(); }
            if (consumeSelect()) {
                switch (statsMenuCursor) {
                    case 0: statsState = ST_LIST;      drawStatsList(); break;
                    case 1: statsState = ST_RESULTS;   drawStatsResults(); break;
                    case 2: statsState = ST_HISTOGRAM; drawHistogram(); break;
                    case 3: statsCount=statsCursor=statsViewOff=0; statsInput="";
                            Serial.println("Datos limpiados."); drawStatsMenu(); break;
                    case 4: firstEntry=true; currentMode=MODE_MENU; drawMenu(); break;
                }
            }
            break;

        case ST_LIST:
            processStatsSerial();
            if (consumeUp()) {
                if (statsCursor > 0) {
                    statsCursor--;
                    if (statsCursor < statsViewOff) statsViewOff--;
                    drawStatsList();
                }
            }
            if (consumeDown()) {
                if (statsCursor < statsCount) {
                    statsCursor++;
                    if (statsCursor >= statsViewOff + STATS_VISIBLE) statsViewOff++;
                    drawStatsList();
                }
            }
            if (consumeSelect()) { statsState = ST_MENU; drawStatsMenu(); }
            break;

        case ST_RESULTS:
        case ST_HISTOGRAM:
            if (consumeSelect()) { statsState = ST_MENU; drawStatsMenu(); }
            if (consumeMode())   {
                /* MODE alterna entre resultados e histograma */
                if (statsState == ST_RESULTS) { statsState = ST_HISTOGRAM; drawHistogram(); }
                else                           { statsState = ST_RESULTS;   drawStatsResults(); }
            }
            break;
    }
}

/* ════════════════════════════════════════════════════════
 *  MODO MATRICES
 * ════════════════════════════════════════════════════════ */

/* Inicializar dimensiones por defecto */
void initMatrices() {
    for (int m = 0; m < MAT_MAX; m++) {
        matRows[m] = 3; matCols[m] = 3;
        for (int r = 0; r < MAT_DIM; r++)
            for (int c = 0; c < MAT_DIM; c++)
                matrices[m][r][c] = 0.0f;
    }
}

/* Dibuja la matriz 'idx' en TFT con cursor en (cr, cc) */
void drawMatrix(int idx, int cr = -1, int cc = -1) {
    tft.fillScreen(ILI9341_BLACK);
    int rows = matRows[idx], cols = matCols[idx];
    char title[16]; snprintf(title, sizeof(title), "MATRIZ M%d (%dx%d)", idx+1, rows, cols);
    tft.setTextSize(1); tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(0, 0); tft.println(title);
    tft.drawLine(0, 10, SCREEN_W, 10, ILI9341_BLUE);

    /* Calcular ancho de celda */
    int cellW = min(60, (SCREEN_W - 4) / cols);
    int cellH = 18;
    int startXm = (SCREEN_W - cellW * cols) / 2;
    int startYm = 16;

    /* Corchetes */
    int h = cellH * rows;
    tft.drawLine(startXm - 6, startYm,     startXm - 6, startYm + h, ILI9341_WHITE);
    tft.drawLine(startXm - 6, startYm,     startXm - 3, startYm,     ILI9341_WHITE);
    tft.drawLine(startXm - 6, startYm + h, startXm - 3, startYm + h, ILI9341_WHITE);
    int ex = startXm + cellW * cols;
    tft.drawLine(ex + 5, startYm,     ex + 5, startYm + h, ILI9341_WHITE);
    tft.drawLine(ex + 2, startYm,     ex + 5, startYm,     ILI9341_WHITE);
    tft.drawLine(ex + 2, startYm + h, ex + 5, startYm + h, ILI9341_WHITE);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int px = startXm + c * cellW;
            int py = startYm + r * cellH;
            bool sel = (r == cr && c == cc);
            if (sel) tft.fillRect(px, py, cellW - 2, cellH - 2, ILI9341_BLUE);

            tft.setTextColor(sel ? ILI9341_BLACK : ILI9341_WHITE);
            char num[12]; snprintf(num, sizeof(num), "%7.3f", matrices[idx][r][c]);
            tft.setCursor(px + 1, py + 4); tft.print(num);
        }
    }

    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(0, SCREEN_H - 20);
    if (cr >= 0) tft.println("UP/DN/UP=mover  Serial=valor  MODE=dim");
    else         tft.println("SELECT=editar  MODE=ops  UP/DN=M");
    tft.setCursor(0, SCREEN_H - 10);
    tft.setTextColor(ILI9341_GREEN);
    if (matInput.length() > 0) { tft.print("> "); tft.print(matInput); }
}

/* Determinante de una submatriz n×n por expansión de Laplace (n≤4) */
float matDet(int idx, int n) {
    if (n == 1) return matrices[idx][0][0];
    if (n == 2) return matrices[idx][0][0]*matrices[idx][1][1] -
                       matrices[idx][0][1]*matrices[idx][1][0];
    /* Para n=3 o n=4: eliminación gaussiana con pivoteo parcial */
    float m[4][4];
    for (int r=0;r<n;r++) for (int c=0;c<n;c++) m[r][c]=matrices[idx][r][c];
    float det = 1.0f;
    for (int col = 0; col < n; col++) {
        int pivot = col;
        for (int row = col+1; row < n; row++)
            if (fabs(m[row][col]) > fabs(m[pivot][col])) pivot = row;
        if (pivot != col) {
            for (int j=0;j<n;j++) { float t=m[col][j]; m[col][j]=m[pivot][j]; m[pivot][j]=t; }
            det *= -1;
        }
        if (fabs(m[col][col]) < 1e-10f) return 0.0f;
        det *= m[col][col];
        for (int row=col+1;row<n;row++) {
            float f = m[row][col] / m[col][col];
            for (int j=col;j<n;j++) m[row][j] -= f * m[col][j];
        }
    }
    return det;
}

/* Traza de la matriz */
float matTrace(int idx) {
    float t = 0; int n=min(matRows[idx],matCols[idx]);
    for (int i=0;i<n;i++) t+=matrices[idx][i][i];
    return t;
}

/* Transpuesta: guarda en M2 si src es M1 */
void matTranspose(int src, int dst) {
    matRows[dst] = matCols[src]; matCols[dst] = matRows[src];
    for (int r=0;r<matRows[src];r++) for (int c=0;c<matCols[src];c++)
        matrices[dst][c][r] = matrices[src][r][c];
}

/* Suma de matrices src1 + src2 → dst (mismo tamaño) */
bool matAdd(int src1, int src2, int dst) {
    if (matRows[src1]!=matRows[src2] || matCols[src1]!=matCols[src2]) return false;
    matRows[dst]=matRows[src1]; matCols[dst]=matCols[src1];
    for (int r=0;r<matRows[dst];r++) for (int c=0;c<matCols[dst];c++)
        matrices[dst][r][c]=matrices[src1][r][c]+matrices[src2][r][c];
    return true;
}

/* Producto de matrices src1 × src2 → dst */
bool matMul(int src1, int src2, int dst) {
    if (matCols[src1]!=matRows[src2]) return false;
    int m=matRows[src1], n=matCols[src2], k=matCols[src1];
    matRows[dst]=m; matCols[dst]=n;
    for (int r=0;r<m;r++) for (int c=0;c<n;c++) {
        float s=0; for (int i=0;i<k;i++) s+=matrices[src1][r][i]*matrices[src2][i][c];
        matrices[dst][r][c]=s;
    }
    return true;
}

void showMatResult(const char *title, int idx) {
    drawMatrix(idx);
    tft.setCursor(0,0); tft.setTextColor(ILI9341_CYAN); tft.println(title);
}

void matOpsMenu() {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(1); tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(0,0); tft.println("OPERACIONES MATRICIALES");
    tft.drawLine(0,10,SCREEN_W,10,ILI9341_BLUE);
    tft.setTextColor(ILI9341_WHITE);
    int y=14;
    #define OPR(txt) tft.setCursor(0,y); tft.println(txt); y+=13;
    OPR("1. det(M1)")
    OPR("2. traza(M1)")
    OPR("3. transpuesta M1 -> M2")
    OPR("4. M1 + M2 -> M3")
    OPR("5. M1 x M2 -> M3")
    OPR("6. norma M1 (Frobenius)")
    #undef OPR
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(0, SCREEN_H-10);
    tft.println("Envie 1-6  SELECT=volver");
}

void processModeMatrix() {
    static bool firstEntry = true;
    static bool inEdit     = false;

    if (firstEntry) {
        firstEntry = false; matState = MAT_MENU; matMenuCursor = 0;
        tft.fillScreen(ILI9341_BLACK);
        tft.setTextSize(1); tft.setTextColor(ILI9341_CYAN);
        tft.setCursor(0,0); tft.println("MATRICES");
        tft.drawLine(0,10,SCREEN_W,10,ILI9341_BLUE);
        tft.setTextColor(ILI9341_WHITE);
        for (int i=0;i<MAT_MENU_N;i++) {
            if (i==matMenuCursor) { tft.fillRect(10,16+i*24-2,SCREEN_W-20,20,ILI9341_BLUE); tft.setTextColor(ILI9341_BLACK); }
            else tft.setTextColor(ILI9341_WHITE);
            tft.setCursor(15, 16+i*24); tft.println(MAT_MENU_OPTS[i]);
        }
    }
    if (currentMode != MODE_MATRIX) { firstEntry = true; return; }

    /* ── Menú principal de matrices ── */
    if (matState == MAT_MENU) {
        if (consumeUp())   { matMenuCursor=(matMenuCursor-1+MAT_MENU_N)%MAT_MENU_N;
                             firstEntry=true; return; }
        if (consumeDown()) { matMenuCursor=(matMenuCursor+1)%MAT_MENU_N;
                             firstEntry=true; return; }
        if (consumeSelect()) {
            if (matMenuCursor < 4) {
                matSel   = matMenuCursor;
                matState = MAT_VIEW;
                matCurR  = 0; matCurC = 0; inEdit = false;
                drawMatrix(matSel);
            } else if (matMenuCursor == 4) {
                matState = MAT_OPS; matOpsMenu();
            } else {
                firstEntry = true; currentMode = MODE_MENU; drawMenu();
            }
        }
        return;
    }

    /* ── Vista / edición de matriz ── */
    if (matState == MAT_VIEW || matState == MAT_EDIT) {
        /* Lectura serial para edición de celda */
        while (Serial.available()) {
            char ch = Serial.read();
            if (ch == '\n') {
                if (matInput.length() > 0) {
                    matrices[matSel][matCurR][matCurC] = matInput.toFloat();
                    /* Avanzar al siguiente campo */
                    matCurC++;
                    if (matCurC >= matCols[matSel]) { matCurC=0; matCurR=(matCurR+1)%matRows[matSel]; }
                    matInput = "";
                    drawMatrix(matSel, matCurR, matCurC);
                }
            } else if (ch == 8 && matInput.length() > 0) {
                matInput.remove(matInput.length()-1); drawMatrix(matSel,matCurR,matCurC);
            } else if (ch >= 32) { matInput += ch; drawMatrix(matSel,matCurR,matCurC); }
        }

        /* Movimiento de cursor */
        if (consumeUp()) {
            matCurR = (matCurR - 1 + matRows[matSel]) % matRows[matSel];
            drawMatrix(matSel, matCurR, matCurC);
        }
        if (consumeDown()) {
            matCurR = (matCurR + 1) % matRows[matSel];
            drawMatrix(matSel, matCurR, matCurC);
        }
        if (consumeMode()) {
            /* Cambiar dimensión de la matriz */
            matRows[matSel] = (matRows[matSel] % MAT_DIM) + 1;
            matCols[matSel] = matRows[matSel]; /* cuadrada por defecto */
            matCurR = matCurC = 0;
            drawMatrix(matSel, matCurR, matCurC);
        }
        if (consumeSelect()) {
            matState = MAT_MENU; firstEntry = true;
        }
        return;
    }

    /* ── Operaciones ── */
    if (matState == MAT_OPS) {
        while (Serial.available()) {
            char ch = Serial.read();
            if (ch >= '1' && ch <= '6') {
                int op = ch - '0';
                tft.fillScreen(ILI9341_BLACK);
                tft.setTextSize(1);
                switch (op) {
                    case 1: {
                        float d = matDet(0, min(matRows[0], matCols[0]));
                        tft.setCursor(0,0); tft.setTextColor(ILI9341_CYAN); tft.println("det(M1):");
                        tft.setTextSize(2); tft.setCursor(0,20); tft.setTextColor(ILI9341_WHITE);
                        tft.println(d, 6);
                        Serial.print("det(M1) = "); Serial.println(d, 8);
                        break;
                    }
                    case 2: {
                        float tr = matTrace(0);
                        tft.setCursor(0,0); tft.setTextColor(ILI9341_CYAN); tft.println("traza(M1):");
                        tft.setTextSize(2); tft.setCursor(0,20); tft.setTextColor(ILI9341_WHITE);
                        tft.println(tr, 6);
                        Serial.print("traza(M1) = "); Serial.println(tr, 8);
                        break;
                    }
                    case 3:
                        matTranspose(0, 1);
                        showMatResult("M1' -> M2:", 1);
                        break;
                    case 4:
                        if (matAdd(0, 1, 2)) showMatResult("M1+M2 -> M3:", 2);
                        else { tft.setCursor(0,0); tft.setTextColor(ILI9341_RED);
                               tft.println("Dimensiones incompatibles"); }
                        break;
                    case 5:
                        if (matMul(0, 1, 2)) showMatResult("M1xM2 -> M3:", 2);
                        else { tft.setCursor(0,0); tft.setTextColor(ILI9341_RED);
                               tft.println("Dimensiones incompatibles"); }
                        break;
                    case 6: {
                        float norm = 0;
                        for (int r=0;r<matRows[0];r++) for (int c=0;c<matCols[0];c++)
                            norm += matrices[0][r][c]*matrices[0][r][c];
                        norm = sqrtf(norm);
                        tft.setCursor(0,0); tft.setTextColor(ILI9341_CYAN); tft.println("||M1|| Frobenius:");
                        tft.setTextSize(2); tft.setCursor(0,20); tft.setTextColor(ILI9341_WHITE);
                        tft.println(norm, 6);
                        Serial.print("||M1|| = "); Serial.println(norm, 8);
                        break;
                    }
                }
                tft.setTextColor(ILI9341_GREEN);
                tft.setCursor(0, SCREEN_H-10); tft.println("SELECT=volver a operaciones");
            }
        }
        if (consumeSelect()) { matState = MAT_MENU; firstEntry = true; }
        return;
    }
}

/* ════════════════════════════════════════════════════════
 *  SETUP
 * ════════════════════════════════════════════════════════ */
void setup() {
    Serial.begin(115200);
    /* NO inicializar Serial1 – pin 32 es el potenciómetro */

    /* Botones: 3V3→botón→GPIO (activo-HIGH). INPUT_PULLDOWN + RISING. */
    pinMode(BUTTON_UP,     INPUT_PULLDOWN);
    pinMode(BUTTON_DOWN,   INPUT_PULLDOWN);
    pinMode(BUTTON_SELECT, INPUT_PULLDOWN);
    pinMode(BUTTON_MODE,   INPUT_PULLDOWN);

    attachInterrupt(digitalPinToInterrupt(BUTTON_UP),     isrUp,     RISING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_DOWN),   isrDown,   RISING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_SELECT), isrSelect, RISING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_MODE),   isrMode,   RISING);

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(ILI9341_BLACK);

    initMatrices();

    /* Splash screen; espera SELECT para continuar */
    splashScreen();
    unsigned long splashStart = millis();
    while (!selectPressed && millis() - splashStart < 5000) {
        yield();
    }
    selectPressed = false;

    drawMenu();

    Serial.println("=== Calculadora Cientifica ESP32 v2.0 ===");
    Serial.println("Funciones: sin cos tan asin acos atan sinh cosh tanh");
    Serial.println("           asinh acosh atanh sqrt ln log log2 exp pow");
    Serial.println("           abs arg re im gamma erf erfc fac ncr npr");
    Serial.println("           floor ceil round sign min max mod");
    Serial.println("Variables: {a} {b} {c} {d} {f}  |  ans = ultimo resultado");
    Serial.println("Modos: MATH GRAPH POLY VARS INT TABLE STATS MATRIX");
    Serial.println("==========================================");
}

/* ════════════════════════════════════════════════════════
 *  LOOP PRINCIPAL
 * ════════════════════════════════════════════════════════ */
void loop() {
    switch (currentMode) {
        case MODE_MENU:   processMenu();        break;
        case MODE_MATH:   processMathMode();    break;
        case MODE_GRAPH:  processGraphMode();   break;
        case MODE_POLY:   processPolyMode();    break;
        case MODE_VARS:   processVarsMode();    break;
        case MODE_INT:    processIntegral();    break;
        case MODE_TABLE:  processTable();       break;
        case MODE_STATS:  modoEstadistico();    break;
        case MODE_MATRIX: processModeMatrix();  break;
    }
    /* Pequeño yield para evitar WDT en loops ocupados */
    yield();
}
