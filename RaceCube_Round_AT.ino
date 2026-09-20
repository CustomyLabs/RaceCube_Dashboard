/*
   PROJECT: RACECUBE - STABLE VERSION 7.2 (ULTIMATE WIRELESS + FIXED MENUS + V6.0 GEARBOX UI)
   - Fixed: ESP-NOW Packet struct updated to SuperTelemetry v7.2 (matches OTA Transmission Module)
   - Fixed: Restored all original gauge functions (No more blank screens)
   - Fixed: Swipe Trap resolved (Menu logic mapped UP/DOWN correctly)
   - Feature: ESP-NOW Channel Lock for guaranteed 0-latency connection
   - Feature: V6.0 Aggressive Gearbox Design (F1-Style Sweep, Crosshair Telemetry, Massive Chevrons)
   - Feature: Added massive flashing "LOW OIL PRESS" warning overlay
*/

#include <Arduino.h>
#include <math.h>
#include <vector>
#include <WiFi.h>
#include <esp_now.h> 
#include <esp_wifi.h> 
#include <ESP_Panel_Library.h>
#include <Preferences.h>
#include "nvs_flash.h"

// ===================== 1. SETTINGS & GLOBALS =====================

#define LCD_NAME ST77916
#define LCD_WIDTH (360)
#define LCD_HEIGHT (360)
#define LCD_COLOR_BITS (16)
#define LCD_SPI_FREQ_HZ (40 * 1000 * 1000) 

#define LCD_PIN_SPI_CS (10)
#define LCD_PIN_SPI_SCK (9)
#define LCD_PIN_SPI_DATA0 (11)
#define LCD_PIN_SPI_DATA1 (12)
#define LCD_PIN_SPI_DATA2 (13)
#define LCD_PIN_SPI_DATA3 (14)
#define LCD_PIN_RST (47)
#define LCD_PIN_BK_LIGHT (15)

#define TOUCH_SCL (8)
#define TOUCH_SDA (7)
#define TOUCH_RST (40)
#define TOUCH_INT (41)

#define _LCD_CLASS(name, ...) ESP_PanelLcd_##name(__VA_ARGS__)
#define LCD_CLASS(name, ...) _LCD_CLASS(name, ##__VA_ARGS__)
#define _EXAMPLE_TOUCH_CLASS(name, ...) ESP_PanelTouch_##name(__VA_ARGS__)
#define EXAMPLE_TOUCH_CLASS(name, ...) _EXAMPLE_TOUCH_CLASS(name, ##__VA_ARGS__)
#define EXAMPLE_TOUCH_NAME CST816S
#define EXAMPLE_TOUCH_WIDTH (360)
#define EXAMPLE_TOUCH_HEIGHT (360)
#define EXAMPLE_TOUCH_I2C_FREQ_HZ (400 * 1000)

enum AppState { STATE_DASHBOARD = 0, STATE_SETTINGS, STATE_DIAGNOSTICS };
enum GaugeType { GAUGE_VOLT=0, GAUGE_BOOST, GAUGE_WATER, GAUGE_AFR, GAUGE_OIL, GAUGE_GEARBOX, GAUGE_EXHAUST, GAUGE_FUEL, GAUGE_FUEL_PRESS, GAUGE_CUSTOM, GAUGE_COUNT };
enum Theme { THEME_YELLOW=0, THEME_BLUE, THEME_WHITE, THEME_RED, THEME_DARKGRAY, THEME_COUNT };
enum DataSource { SRC_OBD=0, SRC_EXT };
enum Gesture { GESTURE_NONE=0, GESTURE_TAP, GESTURE_SWIPE_UP, GESTURE_SWIPE_DOWN, GESTURE_SWIPE_LEFT, GESTURE_SWIPE_RIGHT };

struct PIDDef { const char* name; const char* unit; float minV; float maxV; float step; };
const PIDDef PID_LIST[] = {
    {"RPM", "x1000", 0.0f, 9.0f, 1.0f}, {"SPEED", "KM/H", 0.0f, 280.0f, 20.0f},
    {"INTAKE T", "C", 0.0f, 80.0f, 10.0f}, {"THROTTLE", "%", 0.0f, 100.0f, 10.0f},
    {"LOAD", "%", 0.0f, 100.0f, 10.0f}, {"TIMING", "DEG", 0.0f, 50.0f, 10.0f}
};
const int PID_LIST_COUNT = 6;

struct Glyph5x7 { char c; uint8_t rows[7]; };
struct GaugeConfig { bool isVisible = true; DataSource source = SRC_OBD; int customPidIndex = 0; };

struct SharedData {
  float volt = 0.0f; float boost = 0.0f; float water = 0.0f; float afr = 14.7f; float oil = 0.0f;
  float exh = 0.0f; float fuel = 0.0f; float fuelPress = 0.0f; float customVal = 0.0f;
  float rpm = 0.0f; float speed = 0.0f; float intakeT = 0.0f; float throttle = 0.0f; 
  float load = 0.0f; float timing = 0.0f; float oilT = 0.0f;
  
  bool rfConnected = false; 
  bool canConnected = false; 
  bool checkEngine = false;
  bool oilLamp = false; // ЛОКАЛЬНАЯ ПЕРЕМЕННАЯ ДЛЯ МАСЛЕНКИ
  
  volatile AppState appState = STATE_DASHBOARD; volatile GaugeType currentGauge = GAUGE_GEARBOX;
  Theme currentTheme = THEME_RED; 
  int settingsPageIndex = 0; int globalRotation = 0;
} data;

// --- СТРУКТУРА ESP-NOW (ОБНОВЛЕННАЯ СИНХРОНИЗАЦИЯ v7.2) ---
#pragma pack(push, 1)
typedef struct {
    uint8_t sigR; uint8_t sigN; uint8_t sigD;
    uint8_t sig2; uint8_t sig3; uint8_t sig4; uint8_t sig5;
    uint8_t isSport;
    float atfTempNTC; float oilT; float rpm; float speed;
    float water; float volt; float boost; float afr; float exh;        
    float fuel; float fuelPress; float intakeT; float throttle;    
    float load; float timing; uint8_t canConnected; 
    uint8_t checkEngine;
    uint8_t rawP; uint8_t raw1; uint8_t rawPlus; uint8_t rawMinus; // НОВЫЕ ПОЛЯ ДЛЯ ДИАГНОСТИКИ (v7.1)
    uint8_t oilLamp; // НОВОЕ ПОЛЕ МАСЛЕНКИ (v7.1)
} SuperTelemetry;
#pragma pack(pop)

SuperTelemetry incomingShift;
unsigned long lastPacketTime = 0;

ESP_PanelBusQSPI *panel_bus_qspi = nullptr; ESP_PanelLcd *lcd = nullptr;
ESP_PanelBusI2C *touch_bus = nullptr; ESP_PanelTouch *touch = nullptr;
Preferences prefs; uint16_t *frame = nullptr; SemaphoreHandle_t dataMutex; 
GaugeConfig gaugeConfigs[GAUGE_COUNT];

int touchStartX = 0, touchStartY = 0, lastTouchX = 0, lastTouchY = 0;
bool isTouching = false; uint32_t touchStartTime = 0;
Gesture currentGesture = GESTURE_NONE;

static const Glyph5x7 font5x7[] = {
  { '0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, { '1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x1F}},
  { '2',{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}}, { '3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
  { '4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, { '5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
  { '6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, { '7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
  { '8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, { '9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
  { '.',{0x00,0x00,0x00,0x00,0x00,0x06,0x06}}, { '-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
  { 'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, { 'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
  { 'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}}, { 'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
  { 'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}}, { 'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
  { 'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}}, { 'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
  { 'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}, { 'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
  { 'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, { 'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
  { 'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, { 'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  { 'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, { 'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
  { 'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, { 'W',{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
  { 'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}}, { 'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
  { 'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}, { 'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
  { 'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, { '%',{0x19,0x1A,0x04,0x08,0x10,0x0B,0x13}},
  { '/', {0x01,0x01,0x02,0x04,0x08,0x10,0x10}}, { ' ',{0,0,0,0,0,0,0}}
};
const int FONT_ROWS = 7, FONT_COLS = 5;

const float ALARM_LIMITS[GAUGE_COUNT][2] = {
    {11.5f, 15.2f}, {0.0f, 1.5f}, {0.0f, 105.0f}, {10.0f, 17.0f}, {0.8f, 0.0f}, 
    {-50.0f, 95.0f}, {0.0f, 9.0f}, {10.0f, 0.0f}, {2.5f, 0.0f}, {0.0f, 0.0f}
};

static inline uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
const uint16_t COL_BLACK=color565(0,0,0), COL_GRAY=color565(85,85,85), COL_WHITE=color565(255,255,255);
const uint16_t COL_RED=color565(224,0,0), COL_YELLOW=color565(255,216,0), COL_BLUE=color565(0,64,255), COL_GREEN=color565(0,200,0), COL_DIM_RED=color565(70,0,0);
const uint16_t THEME_BG[] = { COL_YELLOW, COL_BLUE, COL_WHITE, COL_RED, color565(20,20,20) };
const uint16_t THEME_FG[] = { COL_BLACK, COL_WHITE, COL_BLACK, COL_WHITE, COL_WHITE };

const float DEG2RAD = 3.14159265f / 180.0f;
int gaugeSize=0, cx=0, cy=0, radius=0;
float currentSmoothVal = 0.0f;
GaugeType lastGaugeType = GAUGE_COUNT; 
bool isStartupAnim = true; int animPhase = 0; float animProgress = 0.0f; 

float sinRot = 0.0f, cosRot = 1.0f;
bool useRotation = false;

template<typename T> inline void iswap(T &a, T &b) { T t = a; a = b; b = t; }
inline int strLen(const char *s) { int n=0; if(!s)return 0; while(*s++)++n; return n; }
const Glyph5x7* findGlyph(char c) { for (size_t i = 0; i < sizeof(font5x7)/sizeof(font5x7[0]); ++i) if (font5x7[i].c == c) return &font5x7[i]; return nullptr; }

// ===================== 2. GRAPHICS ENGINE =====================

inline void putPixel(int x, int y, uint16_t color) {
  if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
  int rx = y;
  int ry = LCD_WIDTH - 1 - x;
  frame[ry * LCD_WIDTH + rx] = (color >> 8) | (color << 8); 
}

void fillScreen(uint16_t color) { 
    uint16_t c = (color >> 8) | (color << 8); 
    int len = LCD_WIDTH * LCD_HEIGHT; 
    for (int i = 0; i < len; ++i) frame[i] = c; 
}

void drawHLine(int x0, int x1, int y, uint16_t color) {
  if (y < 0 || y >= LCD_HEIGHT) return;
  if (x0 > x1) iswap(x0, x1);
  for(int x=x0; x<=x1; x++) putPixel(x, y, color);
}

void drawRect(int x, int y, int w, int h, uint16_t color) {
  drawHLine(x, x+w-1, y, color); drawHLine(x, x+w-1, y+h-1, color);
  for(int j=y; j<y+h; j++) { putPixel(x, j, color); putPixel(x+w-1, j, color); }
}

void fillRect(int x, int y, int w, int h, uint16_t color) { 
    for(int j=0; j<h; j++) drawHLine(x, x+w-1, y+j, color); 
}

void fillCircle(int cx0, int cy0, int r, uint16_t color) {
  int x = 0, y = r, d = 3 - 2 * r;
  while (y >= x) {
    drawHLine(cx0 - x, cx0 + x, cy0 + y, color); drawHLine(cx0 - x, cx0 + x, cy0 - y, color);
    drawHLine(cx0 - y, cx0 + y, cy0 + x, color); drawHLine(cx0 - y, cx0 + y, cy0 - x, color);
    if (d < 0) d += 4 * x + 6; else { d += 4 * (x - y) + 10; y--; } x++;
  }
}

void drawCircleOutline(int cx0, int cy0, int r, uint16_t color) {
  int x = 0, y = r, d = 3 - 2 * r;
  while (y >= x) {
    putPixel(cx0+x, cy0+y, color); putPixel(cx0-x, cy0+y, color); putPixel(cx0+x, cy0-y, color); putPixel(cx0-x, cy0-y, color);
    putPixel(cx0+y, cy0+x, color); putPixel(cx0-y, cy0+x, color); putPixel(cx0+y, cy0-x, color); putPixel(cx0-y, cy0-x, color);
    if (d < 0) d += 4 * x + 6; else { d += 4 * (x - y) + 10; y--; } x++;
  }
}

void drawChar5x7(int x, int y, char c, uint16_t color, int scale) {
  const Glyph5x7* g = findGlyph(c); if (!g) g = findGlyph(' '); if (!g) return;
  for (int row = 0; row < FONT_ROWS; ++row) {
    uint8_t bits = g->rows[row];
    for (int col = 0; col < FONT_COLS; ++col) {
      if (bits & (1 << (FONT_COLS - 1 - col))) {
        for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx) putPixel(x + col * scale + dx, y + row * scale + dy, color);
      }
    }
  }
}

void drawTextBold(int x, int y, const char* s, uint16_t color, int scale) {
  int cursorX = x; while (*s) { drawChar5x7(cursorX, y, *s, color, scale); if(scale > 1) drawChar5x7(cursorX + 1, y, *s, color, scale); cursorX += (FONT_COLS + 2) * scale; ++s; }
}

void rotatePoint(int x, int y, int &outX, int &outY) {
    float tx = x - cx; float ty = y - cy;
    outX = (int)(tx * cosRot - ty * sinRot) + cx;
    outY = (int)(tx * sinRot + ty * cosRot) + cy;
}

inline void putPixelRotated(int x, int y, uint16_t color) {
    if(!useRotation) { putPixel(x, y, color); return; }
    int rx, ry; rotatePoint(x, y, rx, ry); putPixel(rx, ry, color);
}

void drawThickLineRotated(int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1; int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1; int err = dx + dy, e2; 
    while (true) {
        int r = thickness / 2; for(int i=-r; i<=r; i++) for(int j=-r; j<=r; j++) putPixelRotated(x0+i, y0+j, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fillTriangleRotated(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
   int rx0, ry0, rx1, ry1, rx2, ry2;
   if(useRotation) { rotatePoint(x0, y0, rx0, ry0); rotatePoint(x1, y1, rx1, ry1); rotatePoint(x2, y2, rx2, ry2); } 
   else { rx0=x0; ry0=y0; rx1=x1; ry1=y1; rx2=x2; ry2=y2; }
   
    int a, b, y, last;
    if (ry0 > ry1) { iswap(ry0, ry1); iswap(rx0, rx1); }
    if (ry1 > ry2) { iswap(ry1, ry2); iswap(rx1, rx2); }
    if (ry0 > ry1) { iswap(ry0, ry1); iswap(rx0, rx1); }

    if(ry0 == ry2) { 
       a = b = rx0; if(rx1 < a) a = rx1; else if(rx1 > b) b = rx1; if(rx2 < a) a = rx2; else if(rx2 > b) b = rx2;
       drawHLine(a, b, ry0, color); return;
    }

    int dx01 = rx1 - rx0, dy01 = ry1 - ry0, dx02 = rx2 - rx0, dy02 = ry2 - ry0, dx12 = rx2 - rx1, dy12 = ry2 - ry1;
    int32_t sa = 0, sb = 0; if(ry1 == ry2) last = ry1; else last = ry1-1;
    for(y=ry0; y<=last; y++) {
        a = rx0 + sa / dy01; b = rx0 + sb / dy02; sa += dx01; sb += dx02; drawHLine(a, b, y, color);
    }
    sa = dx12 * (y - ry1); sb = dx02 * (y - ry0);
    for(; y<=ry2; y++) {
        a = rx1 + sa / dy12; b = rx0 + sb / dy02; sa += dx12; sb += dx02; drawHLine(a, b, y, color);
    }
}

void fillRectRotated(int x, int y, int w, int h, uint16_t color) {
    if(!useRotation) { fillRect(x, y, w, h, color); return; }
    for(int j=0; j<h; j++) for(int i=0; i<w; i++) putPixelRotated(x+i, y+j, color);
}

void drawRectRotated(int x, int y, int w, int h, uint16_t color) {
    drawThickLineRotated(x, y, x+w-1, y, 1, color); drawThickLineRotated(x+w-1, y, x+w-1, y+h-1, 1, color);
    drawThickLineRotated(x+w-1, y+h-1, x, y+h-1, 1, color); drawThickLineRotated(x, y+h-1, x, y, 1, color);
}

void drawCharRotated(int x, int y, char c, uint16_t color, int scale) {
  if(!useRotation) { drawChar5x7(x, y, c, color, scale); return; }
  const Glyph5x7* g = findGlyph(c); if (!g) g = findGlyph(' '); if (!g) return;
  for (int row = 0; row < FONT_ROWS; ++row) {
    uint8_t bits = g->rows[row];
    for (int col = 0; col < FONT_COLS; ++col) {
      if (bits & (1 << (FONT_COLS - 1 - col))) {
        for (int dy = 0; dy < scale; ++dy) 
            for (int dx = 0; dx < scale; ++dx) putPixelRotated(x + col * scale + dx, y + row * scale + dy, color);
      }
    }
  }
}

void drawTextBoldRotated(int x, int y, const char* s, uint16_t color, int scale) {
  if(!useRotation) { drawTextBold(x, y, s, color, scale); return; }
  int cursorX = x; while (*s) { drawCharRotated(cursorX, y, *s, color, scale); if(scale > 1) drawCharRotated(cursorX + 1, y, *s, color, scale); cursorX += (FONT_COLS + 2) * scale; ++s; }
}

void drawCharSmooth(int x, int y, char c, uint16_t color, int scale) {
    const Glyph5x7* g = findGlyph(c); if (!g) g = findGlyph(' '); if (!g) return;
    int r = scale / 2 + 1; 
    for (int row = 0; row < FONT_ROWS; ++row) {
        uint8_t bits = g->rows[row];
        for (int col = 0; col < FONT_COLS; ++col) {
            if (bits & (1 << (FONT_COLS - 1 - col))) {
                int px = x + col * scale + scale/2;
                int py = y + row * scale + scale/2;
                for(int i=-r; i<=r; i++) {
                    for(int j=-r; j<=r; j++) {
                        if(i*i + j*j <= r*r) putPixelRotated(px+i, py+j, color);
                    }
                }
            }
        }
    }
}

void drawTextSmooth(int x, int y, const char* s, uint16_t color, int scale) {
    int cursorX = x; 
    while (*s) { 
        drawCharSmooth(cursorX, y, *s, color, scale); 
        cursorX += (FONT_COLS + 2) * scale; 
        ++s; 
    }
}

// ===================== 3. DRAW & LOGIC =====================

void drawGaugeCommon(const char* title, const char* units, float val, float minV, float maxV, 
                     float ticksStart, float ticksEnd, float ticksStep, 
                     float peakMin, float peakMax, int precision, bool showHalfTicks) {
  
  uint16_t bg = THEME_BG[data.currentTheme], fg = THEME_FG[data.currentTheme];
  bool warningState = false;
  
  if(!isStartupAnim && data.rfConnected) {
      float limitLow = ALARM_LIMITS[data.currentGauge][0];
      float limitHigh = ALARM_LIMITS[data.currentGauge][1];
      if((limitLow != 0.0f && val < limitLow) || (limitHigh != 0.0f && val > limitHigh)) {
          warningState = true;
          if((millis() / 250) % 2 == 0) { bg = COL_RED; fg = COL_WHITE; }
      }
  }

  float rad = data.globalRotation * DEG2RAD;
  sinRot = sin(rad); cosRot = cos(rad);
  useRotation = (data.globalRotation != 0);

  fillScreen(bg);
  int rOuter = radius, rInner = radius - 8;
  
  fillCircle(cx, cy, rOuter, COL_GRAY); 
  fillCircle(cx, cy, rInner, bg); 
  drawCircleOutline(cx, cy, rInner, fg);
  
  int rTicks = rInner - 2; float step = showHalfTicks ? (ticksStep / 2.0f) : ticksStep;
  
  for(float v = ticksStart; v <= ticksEnd + 0.001; v += step) {
      float frac = (v - minV) / (maxV - minV); if(frac < 0 || frac > 1) continue;
      float ang = (270.0f - frac * 270.0f) * DEG2RAD;
      bool isMajor = (fmod(fabs(v), ticksStep) < 0.001); 
      int len = isMajor ? 20 : 12, width = isMajor ? 3 : 1;
      
      int x0 = cx + cos(ang) * rTicks, y0 = cy - sin(ang) * rTicks;
      int x1 = cx + cos(ang) * (rTicks - len), y1 = cy - sin(ang) * (rTicks - len);
      
      drawThickLineRotated(x1, y1, x0, y0, width, fg);
      if(isMajor) {
          int tx = cx + cos(ang) * (rTicks - 35), ty = cy - sin(ang) * (rTicks - 35);
          char buf[10]; if(ticksStep >= 1.0f) snprintf(buf, sizeof(buf), "%.0f", v); else snprintf(buf, sizeof(buf), "%.1f", v);
          int w = strLen(buf) * 7 * 2; 
          drawTextBoldRotated(tx - w/2, ty - 7, buf, fg, 2);
      }
  }
  
  int winW = 86, winH = 32, winX = cx - winW/2, winY = cy - 100;
  fillRectRotated(winX, winY, winW, winH, COL_GRAY); 
  drawRectRotated(winX, winY, winW, winH, fg);
  
  char valBuf[10]; 
  float displayVal = val; int dispPrec = precision;
  if(dispPrec == 0) snprintf(valBuf, sizeof(valBuf), "%.0f", displayVal); else snprintf(valBuf, sizeof(valBuf), "%.1f", displayVal);
  int digScale = 2, digW = strLen(valBuf) * 7 * digScale;
  drawTextBoldRotated(cx - digW/2, winY + 8, valBuf, COL_WHITE, digScale);
  
  drawTextBoldRotated(cx - (strLen("RACE CUBE")*7*3)/2, winY + winH + 10, "RACE CUBE", fg, 3);
  drawTextBoldRotated(cx - (strLen(title)*7*2)/2, cy + 55, title, fg, 2);
  drawTextBoldRotated(cx - (strLen(units)*7*2)/2, cy + 90, units, fg, 2);

  float clampV = val; if(clampV < minV) clampV = minV; if(clampV > maxV) clampV = maxV;
  float frac = (clampV - minV) / (maxV - minV); float ang = (270.0f - frac * 270.0f) * DEG2RAD;
  int rArrow = rInner - 25; int tipX = cx + cos(ang) * rArrow, tipY = cy - sin(ang) * rArrow;
  float perp = ang + 3.14159f/2.0f; int baseW = 10;
  int bx1 = cx + cos(perp) * baseW, by1 = cy - sin(perp) * baseW, bx2 = cx - cos(perp) * baseW, by2 = cy + sin(perp) * baseW;
  uint16_t arrowCol = COL_RED; if(data.currentTheme == THEME_RED) arrowCol = COL_BLACK;
  
  fillTriangleRotated(tipX, tipY, bx1, by1, bx2, by2, arrowCol);
  fillCircle(cx, cy, 30, COL_GRAY); fillCircle(cx, cy, 27, COL_BLACK); drawCircleOutline(cx, cy, 30, fg);

  int pdX = cx + 90, pdY = cy + 75; 
  uint16_t peakColor = warningState ? COL_RED : COL_DIM_RED; 

  if(useRotation) {
    int rotPDX, rotPDY; rotatePoint(pdX, pdY, rotPDX, rotPDY);
    fillCircle(rotPDX, rotPDY, 14, peakColor); drawCircleOutline(rotPDX, rotPDY, 14, fg);
  } else { fillCircle(pdX, pdY, 14, peakColor); drawCircleOutline(pdX, pdY, 14, fg); }
  drawTextBoldRotated(pdX - 20, pdY + 20, "PEAK", fg, 2);
  
  useRotation = false; 
}

void drawVoltGauge(float v) { drawGaugeCommon("BATTERY", "VOLTS", v, 8, 18, 8, 18, 1.0f, 15.0f, 18.0f, 1, true); }
void drawBoostGauge(float v) { drawGaugeCommon("BOOST", "BAR", v, -1.0f, 3.0f, -1.0f, 3.0f, 0.5f, 2.0f, 3.0f, 1, true); }
void drawWaterGauge(float v) { drawGaugeCommon("WATER", "TEMP", v, 20, 130, 20, 130, 10.0f, 100.0f, 130.0f, 0, true); }
void drawAfrGauge(float v) { drawGaugeCommon("AIR/FUEL", "RATIO", v, 8, 20, 8, 20, 1.0f, 8.0f, 10.0f, 1, true); }
void drawOilGauge(float v) { drawGaugeCommon("OIL PRESS", "BAR", v, 0, 10, 0, 10, 1.0f, 0.0f, 0.5f, 1, true); }
void drawExhaustGauge(float v) { drawGaugeCommon("EXHAUST", "x100 C", v, 0, 12, 0, 12, 1.0f, 9.0f, 12.0f, 0, true); }
void drawFuelGauge(float v) { drawGaugeCommon("FUEL", "%", v, 0, 100, 0, 100, 10.0f, 0.0f, 10.0f, 0, true); }
void drawFuelPressGauge(float v) { drawGaugeCommon("FUEL PRESS", "BAR", v, 0, 10, 0, 10, 1.0f, 0.0f, 2.5f, 1, true); }

void drawCustomGauge(float v) {
    int pidIdx = gaugeConfigs[GAUGE_CUSTOM].customPidIndex;
    if(pidIdx >= PID_LIST_COUNT) pidIdx = 0; const PIDDef* pid = &PID_LIST[pidIdx];
    int prec = (pid->maxV - pid->minV < 10.0f) ? 1 : 0;
    drawGaugeCommon(pid->name, pid->unit, v, pid->minV, pid->maxV, pid->minV, pid->maxV, pid->step, 0, 0, prec, true);
}

// ===================== ЛОГИКА ПЕРЕДАЧ И ОТРИСОВКА GEARBOX (SUPER SPORT ИЗ V6) =====================

void drawGearboxGauge(float rpm) {
    uint16_t bg = THEME_BG[data.currentTheme];
    uint16_t fg = THEME_FG[data.currentTheme];
    fillScreen(bg);
    
    float rad = data.globalRotation * DEG2RAD;
    sinRot = sin(rad); cosRot = cos(rad);
    useRotation = (data.globalRotation != 0);

    char mainGear = 'P'; char subGear = ' ';
    bool showUp = false; bool showDn = false; bool isSport = false;
    float atfTemp = 0.0f;

    if (isStartupAnim) {
        showUp = true; showDn = true; isSport = true; atfTemp = 85.0f;
        if (rpm < 1000) { mainGear = 'P'; subGear = ' '; }
        else if (rpm < 2000) { mainGear = 'R'; subGear = ' '; }
        else if (rpm < 3000) { mainGear = 'N'; subGear = ' '; }
        else if (rpm < 4000) { mainGear = 'D'; subGear = '1'; }
        else if (rpm < 5000) { mainGear = 'D'; subGear = '2'; }
        else if (rpm < 6000) { mainGear = 'S'; subGear = '3'; }
        else { mainGear = 'S'; subGear = '4'; }
    } else {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        SuperTelemetry localData = incomingShift;
        xSemaphoreGive(dataMutex);

        isSport = localData.isSport;
        atfTemp = localData.atfTempNTC; 

        if (!localData.sigR && !localData.sigN && !localData.sigD) { mainGear = 'P'; } 
        else if (localData.sigR) { mainGear = 'R'; } 
        else if (localData.sigN) { mainGear = 'N'; } 
        else if (localData.sigD || localData.isSport) {
            mainGear = localData.isSport ? 'S' : 'D';
            if (localData.sig5) subGear = '5';
            else if (localData.sig4) subGear = '4';
            else if (localData.sig3) subGear = '3';
            else if (localData.sig2) subGear = '2';
            else subGear = '1'; 
        } else { mainGear = '-'; }

        if (isSport && data.rfConnected) {
            if (rpm > 5500) showUp = true;
            else if (rpm < 1500 && subGear != '1' && mainGear != 'P' && mainGear != 'N' && mainGear != 'R') showDn = true;
        }
    }

    // ==========================================
    // 1. АГРЕССИВНЫЙ ТАХОМЕТР (F1-STYLE SWEEP)
    // ==========================================
    for (float v = 0; v <= 7000; v += 125) {
        float frac = v / 7000.0f; 
        float ang = (210.0f - frac * 240.0f) * DEG2RAD; 
        
        uint16_t tickCol = COL_GRAY;
        if (v <= rpm) {
            tickCol = (v >= 6000) ? COL_RED : (v >= 4500) ? COL_YELLOW : COL_GREEN;
            if (data.currentTheme == THEME_RED && tickCol == COL_RED) tickCol = COL_WHITE;
        }

        int rOuter = radius - 5; 
        int rInner = (v <= rpm) ? radius - 35 : radius - 20;
        
        int x0 = cx + cos(ang) * rOuter, y0 = cy - sin(ang) * rOuter;
        int x1 = cx + cos(ang) * rInner, y1 = cy - sin(ang) * rInner;

        drawThickLineRotated(x1, y1, x0, y0, (v <= rpm) ? 5 : 2, tickCol);
    }

    // ==========================================
    // 2. БЛОЧНЫЙ (ГЕОМЕТРИЧЕСКИЙ) ШРИФТ ПЕРЕДАЧ
    // ==========================================
    uint16_t gearColor = (mainGear == 'S') ? COL_RED : fg;
    if (data.currentTheme == THEME_RED && gearColor == COL_RED) gearColor = COL_WHITE;

    char mainGearStr[2] = {mainGear, '\0'};
    char subGearStr[2] = {subGear, '\0'};

    if (subGear == ' ') {
        drawTextBoldRotated(cx - 35, cy - 45, mainGearStr, gearColor, 10);
    } else {
        drawTextBoldRotated(cx - 55, cy - 45, mainGearStr, gearColor, 10);
        drawTextBoldRotated(cx + 25, cy - 20, subGearStr, gearColor, 6); 
    }

    if (isSport) {
        fillRectRotated(cx - 40, cy - 90, 80, 22, COL_RED);
        drawTextBoldRotated(cx - 31, cy - 86, "SPORT", COL_WHITE, 2);
    } else {
        drawTextBoldRotated(cx - 28, cy - 86, "AUTO", COL_GRAY, 2);
    }

    // ==========================================
    // 3. ДИНАМИЧНЫЕ ПРЕДУПРЕЖДЕНИЯ (ШЕВРОНЫ)
    // ==========================================
    bool fastBlink = (millis() / 100) % 2 == 0;
    bool slowBlink = (millis() / 300) % 2 == 0;

    if (showUp && fastBlink) {
        drawThickLineRotated(cx - 140, cy + 20, cx - 110, cy - 20, 6, COL_RED);
        drawThickLineRotated(cx - 110, cy - 20, cx - 80, cy + 20, 6, COL_RED);
        drawThickLineRotated(cx + 80, cy + 20, cx + 110, cy - 20, 6, COL_RED);
        drawThickLineRotated(cx + 110, cy - 20, cx + 140, cy + 20, 6, COL_RED);

        if (rpm > 6200) {
            fillRectRotated(cx - 60, cy - 10, 120, 30, COL_RED);
            drawTextBoldRotated(cx - 45, cy - 3, "SHIFT", COL_WHITE, 3);
        }
    }
    
    if (showDn && slowBlink) {
        drawThickLineRotated(cx - 140, cy - 20, cx - 110, cy + 20, 6, COL_YELLOW);
        drawThickLineRotated(cx - 110, cy + 20, cx - 80, cy - 20, 6, COL_YELLOW);
        drawThickLineRotated(cx + 80, cy - 20, cx + 110, cy + 20, 6, COL_YELLOW);
        drawThickLineRotated(cx + 110, cy + 20, cx + 140, cy - 20, 6, COL_YELLOW);
    }

    // ==========================================
    // 4. РАЗДЕЛЕННАЯ ТЕЛЕМЕТРИЯ (CROSSHAIR)
    // ==========================================
    drawThickLineRotated(cx - 100, cy + 55, cx + 100, cy + 55, 2, COL_GRAY);
    drawThickLineRotated(cx, cy + 55, cx, cy + 120, 2, COL_GRAY);

    drawTextBoldRotated(cx - 70, cy + 65, "RPM", COL_GRAY, 2);
    char rpmBuf[10]; snprintf(rpmBuf, sizeof(rpmBuf), "%.0f", rpm);
    drawTextBoldRotated(cx - 90, cy + 85, rpmBuf, fg, 3);

    uint16_t atfColor = (atfTemp > 105.0f) ? COL_RED : COL_GRAY;
    drawTextBoldRotated(cx + 20, cy + 65, "ATF", atfColor, 2);
    char atfBuf[10]; snprintf(atfBuf, sizeof(atfBuf), "%.0f", atfTemp);
    drawTextBoldRotated(cx + 20, cy + 85, atfBuf, fg, 3);
    drawTextBoldRotated(cx + 70, cy + 90, "C", atfColor, 2);

    if (!isStartupAnim && atfTemp >= 105.0f && (millis() / 250) % 2 == 0) {
        drawCircleOutline(cx, cy, radius-2, COL_RED);
        drawCircleOutline(cx, cy, radius-3, COL_RED);
        drawCircleOutline(cx, cy, radius-4, COL_RED);
    }

    useRotation = false;
}

// ===================== МЕНЮ И НАСТРОЙКИ =====================

void drawSettingsScreen() {
    useRotation = false; fillScreen(COL_BLACK);
    int idx = data.settingsPageIndex;
    drawTextBold(180 - (strLen("SETTINGS")*7*2)/2, 30, "SETTINGS", COL_YELLOW, 2);
    int totalPages = GAUGE_COUNT + 2; 
    int startDotX = 180 - (totalPages * 10) / 2;
    for(int i=0; i<totalPages; i++) { uint16_t c = (i == idx) ? COL_WHITE : COL_GRAY; fillCircle(startDotX + i*12, 330, 3, c); }
    
    if (idx < GAUGE_COUNT) {
        GaugeType g = (GaugeType)idx;
        const char* title;
        if (g == GAUGE_CUSTOM) title = "CUSTOM";
        else { const char* titles[] = {"VOLT", "BOOST", "WATER", "AFR", "OIL P", "GEARBOX", "EXHAUST", "FUEL", "FUEL P"}; title = titles[g]; }
        drawTextBold(180 - (strLen(title)*7*3)/2, 60, title, COL_WHITE, 3);
        int y1 = 130; bool vis = gaugeConfigs[g].isVisible;
        drawTextBold(40, y1, "SHOW:", COL_GRAY, 2);
        uint16_t colVis = vis ? COL_GREEN : COL_RED; const char* txtVis = vis ? "ON" : "OFF";
        drawRect(160, y1-10, 140, 40, colVis); drawTextBold(200, y1, txtVis, colVis, 2);
    } 
    else if(idx == GAUGE_COUNT) {
        drawTextBold(180 - (strLen("ROTATION")*7*2)/2, 80, "ROTATION", COL_WHITE, 2);
        char buf[10]; snprintf(buf, sizeof(buf), "%d deg", data.globalRotation);
        drawTextBold(180 - (strLen(buf)*7*3)/2, 150, buf, COL_YELLOW, 3);
        int yBtn = 220;
        drawRect(60, yBtn, 80, 50, COL_WHITE); drawTextBold(90, yBtn+15, "<", COL_WHITE, 3);
        drawRect(220, yBtn, 80, 50, COL_WHITE); drawTextBold(250, yBtn+15, ">", COL_WHITE, 3);
    }
}

// ЭКРАН ДИАГНОСТИКИ СИСТЕМЫ
void drawDiagnosticsScreen() {
    useRotation = false; fillScreen(color565(20, 0, 0)); 
    drawTextBold(180 - (strLen("SYSTEM STATUS")*7*2)/2, 30, "SYSTEM STATUS", COL_WHITE, 2);

    drawTextBold(40, 120, "RF LINK:", COL_GRAY, 2);
    if(data.rfConnected) drawTextBold(180, 120, "OK", COL_GREEN, 2);
    else drawTextBold(180, 120, "FAIL", COL_RED, 2);

    drawTextBold(40, 180, "CAN BUS:", COL_GRAY, 2);
    if(data.canConnected) drawTextBold(180, 180, "OK", COL_GREEN, 2);
    else drawTextBold(180, 180, "FAIL", COL_RED, 2);

    // Дополнительный вывод статуса лампы Check Engine
    drawTextBold(40, 240, "MIL (CHECK):", COL_GRAY, 2);
    if(data.checkEngine) drawTextBold(200, 240, "ON", COL_RED, 2);
    else drawTextBold(200, 240, "OFF", COL_GREEN, 2);

    drawTextBold(180 - (strLen("SWIPE UP TO EXIT")*7)/2, 320, "SWIPE UP TO EXIT", COL_GRAY, 1);
}

void updateGaugeLogic() {
    float targetVal = 0.0f, minV=0, maxV=0; GaugeType gType;
    if(xSemaphoreTake(dataMutex, 5) == pdTRUE) {
        gType = data.currentGauge;
        switch(gType) {
            case GAUGE_VOLT: targetVal=data.volt; minV=8; maxV=18; break;
            case GAUGE_BOOST: targetVal=data.boost; minV=-1; maxV=3; break;
            case GAUGE_WATER: targetVal=data.water; minV=20; maxV=130; break;
            case GAUGE_AFR: targetVal=data.afr; minV=8; maxV=20; break;
            case GAUGE_OIL: targetVal=data.oil; minV=0; maxV=10; break;
            case GAUGE_EXHAUST: targetVal=data.exh; minV=0; maxV=12; break;
            case GAUGE_FUEL: targetVal=data.fuel; minV=0; maxV=100; break;
            case GAUGE_FUEL_PRESS: targetVal=data.fuelPress; minV=0; maxV=10; break;
            case GAUGE_GEARBOX: targetVal = data.rpm; minV = 0; maxV = 7000; break;
            case GAUGE_CUSTOM: {
                 targetVal = data.customVal; 
                 int pidIdx = gaugeConfigs[GAUGE_CUSTOM].customPidIndex;
                 if(pidIdx < PID_LIST_COUNT) { minV = PID_LIST[pidIdx].minV; maxV = PID_LIST[pidIdx].maxV; }
                 break; 
            }
        }
        xSemaphoreGive(dataMutex);
    } else return;
    
    if(gType != lastGaugeType) { currentSmoothVal = targetVal; lastGaugeType = gType; }
    
    if(isStartupAnim) {
        float step = 0.04f; 
        if(animPhase == 0) { animProgress += step; if(animProgress >= 1.0f) { animProgress = 1.0f; animPhase = 1; } } 
        else if(animPhase == 1) { animProgress -= step; if(animProgress <= 0.0f) { animProgress = 0.0f; animPhase = 2; isStartupAnim = false; } }
        currentSmoothVal = minV + (maxV - minV) * animProgress;
    } else {
        currentSmoothVal += (targetVal - currentSmoothVal) * 0.15f;
    }
    
    switch(gType) {
        case GAUGE_VOLT:     drawVoltGauge(currentSmoothVal); break;
        case GAUGE_BOOST:    drawBoostGauge(currentSmoothVal); break;
        case GAUGE_WATER:    drawWaterGauge(currentSmoothVal); break;
        case GAUGE_AFR:      drawAfrGauge(currentSmoothVal); break;
        case GAUGE_OIL:      drawOilGauge(currentSmoothVal); break;
        case GAUGE_EXHAUST:  drawExhaustGauge(currentSmoothVal); break;
        case GAUGE_FUEL:     drawFuelGauge(currentSmoothVal); break;
        case GAUGE_FUEL_PRESS: drawFuelPressGauge(currentSmoothVal); break;
        case GAUGE_GEARBOX:  drawGearboxGauge(currentSmoothVal); break;
        case GAUGE_CUSTOM:   drawCustomGauge(currentSmoothVal); break;
    }

    // --- КРИТИЧЕСКОЕ УВЕДОМЛЕНИЕ: ДАВЛЕНИЕ МАСЛА ---
    if (!isStartupAnim && data.rfConnected && data.oilLamp && data.appState != STATE_SETTINGS) {
        if ((millis() / 200) % 2 == 0) {
            useRotation = false; 
            fillRect(0, cy - 30, LCD_WIDTH, 60, COL_RED);
            drawTextBold(cx - (strLen("LOW OIL PRESS")*7*3)/2, cy - 10, "LOW OIL PRESS", COL_WHITE, 3);
            drawCircleOutline(cx, cy, radius-2, COL_RED);
            drawCircleOutline(cx, cy, radius-3, COL_RED);
            drawCircleOutline(cx, cy, radius-4, COL_RED);
        }
    }
}

// ===================== 5. NETWORK CALLBACK =====================

void OnDataRecv(const esp_now_recv_info *recv_info, const uint8_t *incomingDataPtr, int len) {
    if (len == sizeof(SuperTelemetry)) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        memcpy(&incomingShift, incomingDataPtr, sizeof(incomingShift));
        
        data.rpm = incomingShift.rpm;
        data.water = incomingShift.water;
        data.boost = incomingShift.boost;
        data.volt = incomingShift.volt;
        data.afr = incomingShift.afr;
        data.exh = incomingShift.exh;
        data.fuel = incomingShift.fuel;
        data.fuelPress = incomingShift.fuelPress;
        data.speed = incomingShift.speed;
        data.intakeT = incomingShift.intakeT;
        data.throttle = incomingShift.throttle;
        data.load = incomingShift.load;
        data.timing = incomingShift.timing;
        data.oilT = incomingShift.oilT;

        data.canConnected = (incomingShift.canConnected == 1);
        data.checkEngine = (incomingShift.checkEngine == 1);
        data.oilLamp = (incomingShift.oilLamp == 1); // Читаем статус масленки
        
        data.rfConnected = true;
        lastPacketTime = millis();
        xSemaphoreGive(dataMutex);
    }
}

void WatchdogTask(void *parameter) {
    while(1) {
        if (millis() - lastPacketTime > 2000) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            data.rfConnected = false;
            data.canConnected = false;
            xSemaphoreGive(dataMutex);
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

void saveConfig(int idx) {
    char key[10]; snprintf(key, sizeof(key), "cfg_%d", idx);
    int val = (gaugeConfigs[idx].isVisible ? 1 : 0) | (gaugeConfigs[idx].source << 1);
    if(idx == GAUGE_CUSTOM) { val |= (gaugeConfigs[idx].customPidIndex << 2); }
    prefs.putInt(key, val);
}

// ===================== SYSTEM & TOUCH =====================

void processTouchInput() {
  if(!touch) return;
  ESP_PanelTouchPoint point; int p = touch->readPoints(&point, 1, 0);
  currentGesture = GESTURE_NONE;
  if (p > 0) {
      int tx = LCD_WIDTH - point.y;
      int ty = point.x;
      
      if (!isTouching) { isTouching = true; touchStartX = tx; touchStartY = ty; touchStartTime = millis(); }
      lastTouchX = tx; lastTouchY = ty;
  } else {
      if (isTouching) {
          isTouching = false;
          int dx = lastTouchX - touchStartX; int dy = lastTouchY - touchStartY; int dt = millis() - touchStartTime;
          int swipeThresh = 60; 
          if (dt < 500) { 
              if (abs(dx) < 15 && abs(dy) < 15) currentGesture = GESTURE_TAP;
              else if (abs(dx) > abs(dy) && abs(dx) > swipeThresh) currentGesture = (dx > 0) ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
              else if (abs(dy) > abs(dx) && abs(dy) > swipeThresh) currentGesture = (dy > 0) ? GESTURE_SWIPE_DOWN : GESTURE_SWIPE_UP;
          }
      }
  }
}

void setup() {
  Serial.begin(115200);
  nvs_flash_init(); 
  dataMutex = xSemaphoreCreateMutex();

  prefs.begin("settings", false);
  data.currentGauge = (GaugeType)prefs.getInt("gauge", 0);
  data.currentTheme = (Theme)prefs.getInt("theme", 0);
  data.globalRotation = prefs.getInt("rot", 0);
  
  ESP_Panel *panel = new ESP_Panel(); panel->init(); panel->begin();
  size_t frameSize = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
  frame = (uint16_t*)heap_caps_aligned_alloc(64, frameSize, MALLOC_CAP_SPIRAM);
  memset(frame, 0, frameSize); 
  ledcAttach(LCD_PIN_BK_LIGHT, 5000, 8); ledcWrite(LCD_PIN_BK_LIGHT, 0); 
  panel_bus_qspi = new ESP_PanelBusQSPI(LCD_PIN_SPI_CS, LCD_PIN_SPI_SCK, LCD_PIN_SPI_DATA0, LCD_PIN_SPI_DATA1, LCD_PIN_SPI_DATA2, LCD_PIN_SPI_DATA3);
  panel_bus_qspi->configQspiFreqHz(LCD_SPI_FREQ_HZ); panel_bus_qspi->begin();
  lcd = new LCD_CLASS(LCD_NAME, panel_bus_qspi, LCD_COLOR_BITS, LCD_PIN_RST); lcd->init(); lcd->reset(); lcd->begin(); lcd->displayOn();
  gaugeSize = 360; cx = 180; cy = 180; radius = 178;
  touch_bus = new ESP_PanelBusI2C(TOUCH_SCL, TOUCH_SDA, ESP_PANEL_TOUCH_I2C_PANEL_IO_CONFIG(EXAMPLE_TOUCH_NAME));
  touch_bus->begin();
  touch = new EXAMPLE_TOUCH_CLASS(EXAMPLE_TOUCH_NAME, touch_bus, 360, 360, TOUCH_RST, TOUCH_INT); touch->init(); touch->begin();
  
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  
  if (esp_now_init() == ESP_OK) {
      esp_now_register_recv_cb(OnDataRecv);
  }
  xTaskCreatePinnedToCore(WatchdogTask, "Watchdog", 4096, NULL, 1, NULL, 0);
  
  delay(500); ledcWrite(LCD_PIN_BK_LIGHT, 200);
}

void loop() {
    processTouchInput();
    xSemaphoreTake(dataMutex, portMAX_DELAY); AppState state = data.appState; xSemaphoreGive(dataMutex);

    switch (state) {
        case STATE_DASHBOARD:
            updateGaugeLogic();
            
            if (currentGesture == GESTURE_SWIPE_UP) { data.appState = STATE_SETTINGS; data.settingsPageIndex = 0; } 
            else if (currentGesture == GESTURE_SWIPE_DOWN) { data.appState = STATE_DIAGNOSTICS; }
            else if (currentGesture == GESTURE_SWIPE_LEFT) {
                int next = data.currentGauge; int attempts = 0;
                do { next = (next + 1) % GAUGE_COUNT; attempts++; } while (!gaugeConfigs[next].isVisible && attempts < GAUGE_COUNT);
                data.currentGauge = (GaugeType)next; prefs.putInt("gauge", (int)data.currentGauge);
            }
            else if (currentGesture == GESTURE_SWIPE_RIGHT) { 
                data.currentTheme = (Theme)((data.currentTheme + 1) % THEME_COUNT); 
                prefs.putInt("theme", (int)data.currentTheme); 
            }
            break;
            
        case STATE_SETTINGS:
            drawSettingsScreen();
            
            if (currentGesture == GESTURE_SWIPE_DOWN) { data.appState = STATE_DASHBOARD; }
            else if (currentGesture == GESTURE_SWIPE_LEFT) { data.settingsPageIndex++; if (data.settingsPageIndex > GAUGE_COUNT + 1) data.settingsPageIndex = 0; }
            else if (currentGesture == GESTURE_SWIPE_RIGHT) { data.settingsPageIndex--; if (data.settingsPageIndex < 0) data.settingsPageIndex = GAUGE_COUNT + 1; }
            else if (currentGesture == GESTURE_TAP) {
                int idx = data.settingsPageIndex;
                if (idx < GAUGE_COUNT) {
                    if (lastTouchY > 120 && lastTouchY < 190) { gaugeConfigs[idx].isVisible = !gaugeConfigs[idx].isVisible; saveConfig(idx); }
                } 
                else if (idx == GAUGE_COUNT) {
                    if (lastTouchY > 220 && lastTouchY < 270) {
                        if(lastTouchX < 180) { data.globalRotation -= 5; if(data.globalRotation < -45) data.globalRotation = -45; } 
                        else { data.globalRotation += 5; if(data.globalRotation > 45) data.globalRotation = 45; }
                        prefs.putInt("rot", data.globalRotation);
                    }
                }
            }
            break;
            
        case STATE_DIAGNOSTICS:
            drawDiagnosticsScreen();
            
            if (currentGesture == GESTURE_SWIPE_UP) { data.appState = STATE_DASHBOARD; }
            break;
    }
    
    int chunkHeight = 40;
    for (int y = 0; y < LCD_HEIGHT; y += chunkHeight) {
        int h = (y + chunkHeight > LCD_HEIGHT) ? (LCD_HEIGHT - y) : chunkHeight;
        lcd->drawBitmap(0, y, LCD_WIDTH, h, (const uint8_t*)&frame[y * LCD_WIDTH]);
    }
}