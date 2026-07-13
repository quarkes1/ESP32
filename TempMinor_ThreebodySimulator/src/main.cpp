/**
 * Split-Screen: Three-Body Figure-8 (top) + DHT22 Temp/Humidity Chart (bottom)
 *
 *  0 ┌──────────────────────────┐
 *    │   Three-Body Simulation  │  SCALE=55  CY=87
 *    │   + decay trails         │
 *174 ├──────────────────────────┤  Divider
 *178 │ T:26.5C  H:62%           │  Current readings
 *183 │ ╱╲  Temp (red)  ╱╲       │  0-50°C
 *211 │ ───── 25°C / 50% ─────   │  Grid
 *239 │ ╱╲  Humidity (blue) ╱╲   │  0-100%
 *    └──────────────────────────┘
 *
 * DHT22: GPIO17, read every 2 s.  Chart: 240-pt ring buffer, redrawn on new data.
 */

#include <TFT_eSPI.h>
#include <DHT.h>
#include "animation.h"
#include "badapple.h"

TFT_eSPI tft = TFT_eSPI();

// ============================================================
// Screen layout
//  0 ┌──────────────────┬──────┐
//    │   Three-Body     │      │
//174 ├──────────────────┤ Vals │  ← bottom strip 3:1 ratio
//    │  Chart (180px)   │(60px)│
//239 └──────────────────┴──────┘
// ============================================================
#define SIM_BOTTOM    174
#define DIVIDER_Y     175
#define CHART_TOP     175
#define CHART_BOTTOM  239

// Chart / values panels (3:1 width ratio)
#define CHART_W       180           // Chart width (3/4)
#define VALUE_X       180           // Values panel left edge
#define VALUE_CX      210           // Values panel center (180 + 60/2)

// Three-body display
#define SCALE         55.0f
#define CX            (TFT_WIDTH / 2)
#define CY            87

// DHT22
#define DHTPIN        17
#define DHTTYPE       DHT22
DHT dht(DHTPIN, DHTTYPE);

// Touch (GPIO13 = T4) — toggle display mode
#define TOUCH_PIN       T4
#define TOUCH_SAMPLE    200           // Read every 200 ms
#define TOUCH_COOLDOWN  600           // Min ms between toggles

int  displayMode   = 0;               // 0 = three-body + DHT22, 1 = animation
bool touchWasActive = false;
unsigned long lastToggle = 0;

// Chart sub-areas
#define TEMP_TOP      (CHART_TOP + 8)
#define TEMP_BOT      (CHART_TOP + 36)
#define HUM_TOP       (TEMP_BOT + 1)
#define HUM_BOT       (CHART_BOTTOM)

// ============================================================
// Physics (dimensionless)
// ============================================================
#define NUM_BODIES    3
#define G_GRAV        1.0f
#define SOFTENING     0.05f
#define SUBSTEPS      8
#define DT_SUB        0.001f
#define EJECT_LIMIT   2.8f

// ============================================================
// Trail
// ============================================================
#define TRAIL_LEN     200
#define TRAIL_DECAY   0.005f
#define TRAIL_MIN_BRT 0.08f

struct TrailPoint {
  int16_t sx, sy;
  float   brightness;
};
struct TrailBuf {
  TrailPoint pts[TRAIL_LEN];
  uint16_t   head, count;
};

// ============================================================
// Body
// ============================================================
struct Body {
  float x, y, vx, vy, mass, radius;
};
Body     bodies[NUM_BODIES];
uint16_t starColor[NUM_BODIES];
TrailBuf trail[NUM_BODIES];
int16_t  prevSX[NUM_BODIES], prevSY[NUM_BODIES];

// ============================================================
// DHT22 chart data
// ============================================================
#define CHART_LEN     240
float tempHist[CHART_LEN];
float humHist[CHART_LEN];
uint16_t chartHead  = 0;
uint16_t chartCount = 0;
unsigned long lastDHTRead = 0;


// ============================================================
// Helpers
// ============================================================
inline int16_t scrX(float x) { return (int16_t)(x * SCALE + CX); }
inline int16_t scrY(float y) { return (int16_t)(y * SCALE + CY); }

uint16_t fadeColor(uint16_t c, float t) {
  if (t <= 0.0f) return TFT_BLACK;
  if (t >= 1.0f) return c;
  uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * t);
  uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) * t);
  uint8_t b = (uint8_t)(( c        & 0x1F) * t);
  return (r << 11) | (g << 5) | b;
}

// ============================================================
// Clip check: is a screen pixel inside the three-body area?
// ============================================================
inline bool inSimArea(int16_t sy) { return sy >= 0 && sy <= SIM_BOTTOM; }

// ============================================================
// Body intersection for safe erase
// ============================================================
bool insideAnyBody(int16_t sx, int16_t sy) {
  for (int i = 0; i < NUM_BODIES; i++) {
    int16_t bx = scrX(bodies[i].x), by = scrY(bodies[i].y);
    int16_t r  = (int16_t)(bodies[i].radius);
    int16_t dx = sx - bx, dy = sy - by;
    if (dx * dx + dy * dy <= r * r) return true;
  }
  return false;
}

// ============================================================
// Trail: decay, erase dead, push, draw
// ============================================================
void decayTrail(int idx) {
  TrailBuf &tb = trail[idx];
  for (uint16_t i = 0; i < tb.count; i++) {
    uint16_t pos = (tb.head - tb.count + i + TRAIL_LEN) % TRAIL_LEN;
    tb.pts[pos].brightness -= TRAIL_DECAY;
  }
  while (tb.count > 0) {
    uint16_t oldest = (tb.head - tb.count + TRAIL_LEN) % TRAIL_LEN;
    if (tb.pts[oldest].brightness <= TRAIL_MIN_BRT) {
      tft.drawPixel(tb.pts[oldest].sx, tb.pts[oldest].sy, TFT_BLACK);
      tb.count--;
    } else break;
  }
}

void pushTrail(int idx) {
  TrailBuf &tb = trail[idx];
  int16_t sx = scrX(bodies[idx].x);
  int16_t sy = scrY(bodies[idx].y);
  if (tb.count > 0) {
    uint16_t last = (tb.head - 1 + TRAIL_LEN) % TRAIL_LEN;
    if (tb.pts[last].sx == sx && tb.pts[last].sy == sy) return;
  }
  tb.pts[tb.head].sx = sx;
  tb.pts[tb.head].sy = sy;
  tb.pts[tb.head].brightness = 1.0f;
  tb.head = (tb.head + 1) % TRAIL_LEN;
  if (tb.count < TRAIL_LEN) tb.count++;
}

void drawTrail(int idx) {
  TrailBuf &tb = trail[idx];
  if (tb.count < 2) return;
  uint16_t baseColor = starColor[idx];
  for (uint16_t i = 0; i < tb.count; i++) {
    uint16_t pos = (tb.head - tb.count + i + TRAIL_LEN) % TRAIL_LEN;
    float brt = tb.pts[pos].brightness;
    if (brt <= TRAIL_MIN_BRT) continue;
    int16_t sy = tb.pts[pos].sy;
    if (!inSimArea(sy)) continue;                       // Clip to sim area
    uint16_t col = fadeColor(baseColor, brt);
    tft.drawPixel(tb.pts[pos].sx, sy, col);
  }
}

// ============================================================
// Body erase / draw  (clipped to sim area)
// ============================================================
void eraseBody(int idx) {
  int16_t cx = prevSX[idx], cy = prevSY[idx];
  int16_t r  = (int16_t)(bodies[idx].radius) + 2;
  for (int16_t dy = -r; dy <= r; dy++) {
    for (int16_t dx = -r; dx <= r; dx++) {
      if (dx * dx + dy * dy > r * r) continue;
      int16_t sx = cx + dx, sy = cy + dy;
      if (!inSimArea(sy)) continue;
      if (!insideAnyBody(sx, sy))
        tft.drawPixel(sx, sy, TFT_BLACK);
    }
  }
}

void drawBody(int idx) {
  int16_t sx = scrX(bodies[idx].x);
  int16_t sy = scrY(bodies[idx].y);
  int16_t r  = (int16_t)(bodies[idx].radius);
  if (!inSimArea(sy)) return;
  if (r >= 3) tft.fillCircle(sx, sy, r + 1, tft.color565(35, 30, 18));
  tft.fillCircle(sx, sy, r, starColor[idx]);
  prevSX[idx] = sx;
  prevSY[idx] = sy;
}

// ============================================================
// Physics
// ============================================================
void stepSub(float dt) {
  for (int i = 0; i < NUM_BODIES; i++) {
    for (int j = i + 1; j < NUM_BODIES; j++) {
      float dx = bodies[j].x - bodies[i].x;
      float dy = bodies[j].y - bodies[i].y;
      float d2 = dx * dx + dy * dy + SOFTENING * SOFTENING;
      float d  = sqrtf(d2);
      float f  = G_GRAV * bodies[i].mass * bodies[j].mass / d2;
      float fx = f * dx / d, fy = f * dy / d;
      bodies[i].vx += fx / bodies[i].mass * dt;
      bodies[i].vy += fy / bodies[i].mass * dt;
      bodies[j].vx -= fx / bodies[j].mass * dt;
      bodies[j].vy -= fy / bodies[j].mass * dt;
    }
  }
  for (int i = 0; i < NUM_BODIES; i++) {
    bodies[i].x += bodies[i].vx * dt;
    bodies[i].y += bodies[i].vy * dt;
  }
}

bool isEjected() {
  float mx = 0, my = 0, mt = 0;
  for (int i = 0; i < NUM_BODIES; i++) {
    mx += bodies[i].x * bodies[i].mass;
    my += bodies[i].y * bodies[i].mass;
    mt += bodies[i].mass;
  }
  mx /= mt; my /= mt;
  for (int i = 0; i < NUM_BODIES; i++) {
    float dx = bodies[i].x - mx, dy = bodies[i].y - my;
    if (dx * dx + dy * dy > EJECT_LIMIT * EJECT_LIMIT) return true;
  }
  return false;
}

// ============================================================
// DHT22: read sensor, push to chart buffer
// ============================================================
void readDHT22() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    Serial.println("DHT22 read failed");
    return;
  }
  if (t < -10) t = -10; if (t > 60) t = 60;
  if (h < 0)   h = 0;   if (h > 100) h = 100;

  tempHist[chartHead] = t;
  humHist[chartHead]  = h;
  chartHead = (chartHead + 1) % CHART_LEN;
  if (chartCount < CHART_LEN) chartCount++;

  Serial.printf("DHT22: %.1fC  %.1f%%\n", t, h);
}

// ============================================================
// Draw DHT22 chart (3:1 split — chart : values)
// ============================================================
void drawChart() {
  // --- Clear entire bottom strip ---
  tft.fillRect(0, DIVIDER_Y, TFT_WIDTH, CHART_BOTTOM - DIVIDER_Y + 1, TFT_BLACK);
  tft.drawFastHLine(0, DIVIDER_Y, CHART_W, tft.color565(40, 40, 40));
  // Vertical divider between chart and values
  tft.drawFastVLine(VALUE_X, DIVIDER_Y, CHART_BOTTOM - DIVIDER_Y + 1,
                    tft.color565(40, 40, 40));

  // --- Determine visible data window ---
  int displayCount = chartCount;
  if (displayCount > CHART_W) displayCount = CHART_W;

  // --- Auto-scale: scan visible window for min/max ---
  if (displayCount >= 2) {
    float tMin = 999, tMax = -999, hMin = 999, hMax = -999;
    for (int i = 0; i < displayCount; i++) {
      uint16_t idx = (chartHead - displayCount + i + CHART_LEN) % CHART_LEN;
      if (tempHist[idx] < tMin) tMin = tempHist[idx];
      if (tempHist[idx] > tMax) tMax = tempHist[idx];
      if (humHist[idx]  < hMin) hMin  = humHist[idx];
      if (humHist[idx]  > hMax) hMax  = humHist[idx];
    }
    // Fixed 3px padding from edges; if all values equal, give ±0.1 range
    float tRange = tMax - tMin; if (tRange < 0.0001f) { tMin -= 0.1f; tMax += 0.1f; tRange = 0.2f; }
    float hRange = hMax - hMin; if (hRange < 0.0001f) { hMin -= 1.0f; hMax += 1.0f; hRange = 2.0f; }

    const int16_t PAD = 1;
    int16_t tDrawTop = TEMP_TOP + PAD;
    int16_t tDrawBot = TEMP_BOT - PAD;
    int16_t hDrawTop = HUM_TOP  + PAD;
    int16_t hDrawBot = HUM_BOT  - PAD;

    // Grid
    uint16_t gridCol = tft.color565(35, 35, 35);
    tft.drawFastHLine(0, tDrawBot, CHART_W, gridCol);
    tft.drawFastHLine(0, tDrawTop + (tDrawBot - tDrawTop) / 2, CHART_W, gridCol);
    tft.drawFastHLine(0, hDrawBot, CHART_W, gridCol);
    tft.drawFastHLine(0, hDrawTop + (hDrawBot - hDrawTop) / 2, CHART_W, gridCol);

    // Dynamic labels
    tft.setTextColor(tft.color565(80, 80, 80), TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(2, tDrawTop);           tft.printf("%.1f", tMax);
    tft.setCursor(2, tDrawBot - 8);       tft.printf("%.1f", tMin);
    tft.setCursor(2, hDrawTop);           tft.printf("%.0f", hMax);
    tft.setCursor(2, hDrawBot - 8);       tft.printf("%.0f", hMin);

    // Data lines
    float tPx = (tDrawBot - tDrawTop) / tRange;
    float hPx = (hDrawBot - hDrawTop) / hRange;
    int16_t prevTX = 0, prevTY = 0, prevHX = 0, prevHY = 0;
    bool firstT = true, firstH = true;

    for (int i = 0; i < displayCount; i++) {
      uint16_t idx = (chartHead - displayCount + i + CHART_LEN) % CHART_LEN;
      int16_t  x   = (int16_t)i;

      int16_t ty = tDrawBot - (int16_t)((tempHist[idx] - tMin) * tPx);
      ty = constrain(ty, TEMP_TOP, TEMP_BOT);
      tft.drawPixel(x, ty, TFT_RED);
      if (!firstT) tft.drawLine(prevTX, prevTY, x, ty, TFT_RED);
      prevTX = x; prevTY = ty; firstT = false;

      int16_t hy = hDrawBot - (int16_t)((humHist[idx] - hMin) * hPx);
      hy = constrain(hy, HUM_TOP, HUM_BOT);
      tft.drawPixel(x, hy, TFT_CYAN);
      if (!firstH) tft.drawLine(prevHX, prevHY, x, hy, TFT_CYAN);
      prevHX = x; prevHY = hy; firstH = false;
    }
  }

  // --- Values panel (right 60px): large text ---
  uint16_t lastIdx = (chartHead - 1 + CHART_LEN) % CHART_LEN;
  float curTemp = tempHist[lastIdx];
  float curHum  = humHist[lastIdx];

  // Temperature — size 2, red, centered
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(2);
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1fC", curTemp);
  tft.drawString(buf, VALUE_CX, DIVIDER_Y + 22);    // ~y=197

  // Small °C label
  tft.setTextSize(1);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString("TEMP", VALUE_CX, DIVIDER_Y + 7);   // ~y=182

  // Humidity — size 2, cyan, centered
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.1f%%", curHum);
  tft.drawString(buf, VALUE_CX, DIVIDER_Y + 50);      // ~y=225

  // Small % label
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("HUM", VALUE_CX, DIVIDER_Y + 36);    // ~y=211

  tft.setTextDatum(TL_DATUM);   // Restore default alignment
}

// ============================================================
// Init three-body
// ============================================================
void initSimulation() {
  const float x1  =  0.97000436f,  y1  = -0.24308753f;
  const float vx1 =  0.466203685f, vy1 =  0.43236573f;
  float m1 = random(85, 116) / 100.0f;
  float m2 = random(85, 116) / 100.0f;
  float m3 = random(85, 116) / 100.0f;
  float mAvg = (m1 + m2 + m3) / 3.0f;
  m1 /= mAvg; m2 /= mAvg; m3 /= mAvg;

  bodies[0] = {  x1,  y1,  vx1,  vy1, m1, 3.5f + m1 * 1.5f };
  bodies[1] = { -x1, -y1,  vx1,  vy1, m2, 3.5f + m2 * 1.5f };
  bodies[2] = { 0.0f, 0.0f, -2.0f*vx1, -2.0f*vy1, m3, 3.5f + m3 * 1.5f };

  for (int i = 0; i < NUM_BODIES; i++) {
    bodies[i].vx += random(-60, 61) / 1000.0f;
    bodies[i].vy += random(-60, 61) / 1000.0f;
  }
  float px = 0, py = 0, mt = 0;
  for (int i = 0; i < NUM_BODIES; i++) {
    px += bodies[i].vx * bodies[i].mass;
    py += bodies[i].vy * bodies[i].mass;
    mt += bodies[i].mass;
  }
  for (int i = 0; i < NUM_BODIES; i++) {
    bodies[i].vx -= px / mt;
    bodies[i].vy -= py / mt;
  }

  starColor[0] = tft.color565(255, 248, 210);
  starColor[1] = tft.color565(255, 225, 145);
  starColor[2] = tft.color565(255, 240, 180);

  for (int i = 0; i < NUM_BODIES; i++) {
    memset(&trail[i], 0, sizeof(TrailBuf));
    prevSX[i] = scrX(bodies[i].x);
    prevSY[i] = scrY(bodies[i].y);
  }

  Serial.println("\n--- Simulation Reset ---");
  for (int i = 0; i < NUM_BODIES; i++)
    Serial.printf("  S%d: m=%.2f  pos=(%+.2f,%+.2f)\n",
                  i, bodies[i].mass, bodies[i].x, bodies[i].y);
}

// ============================================================
// Setup Animation (do not edit but notice the font config that may effect following steps)
// ============================================================
void setupAnimation(){
    Serial.println("Playing SetupAnimation");
    
    int MAG = 2 ;
    int dpixel = 8*MAG;
    int C_x = TFT_WIDTH / 2;
    int C_y = TFT_HEIGHT / 2 ;
    
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(MAG);
    tft.drawString("El Psy Congroo", C_x, C_y - dpixel);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Developed by" , C_x , C_y + 2*dpixel);
    tft.drawString("Aurolystant" ,  C_x, C_y + 3*dpixel);
    tft.setTextDatum(TL_DATUM);
    
    delay(2000);
    tft.fillScreen(TFT_BLACK);
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Three-Body + DHT22 Monitor ===");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  #if (TFT_BL >= 0)
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  #endif

  randomSeed(analogRead(0) + micros());
  
  //Setup Animation
  setupAnimation();  
  
  // DHT22
  dht.begin();
  Serial.println("DHT22 sensor started");

  // Three-body
  initSimulation();
  for (int i = 0; i < NUM_BODIES; i++) drawBody(i);

  // Draw initial chart frame
  drawChart();
}

// ============================================================
// Switch display mode
// ============================================================
void switchMode() {
  displayMode = 1 - displayMode;
  tft.fillScreen(TFT_BLACK);

  Serial.printf("\n=== Mode: %s ===\n", displayMode == 0 ? "Monitor" : "Animation");

  if (displayMode == 0) {
    badappleClose();     // Release SPIFFS
    initSimulation();
    for (int i = 0; i < NUM_BODIES; i++) drawBody(i);
    drawChart();
  } else {
    if (!badappleInit(&tft)) {
      // Fallback if no SPIFFS/badapple.rle
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setTextSize(2);
      tft.drawString("No badapple.rle", 120, 100);
      tft.setTextSize(1);
      tft.drawString("Run: pio run -t uploadfs", 120, 130);
      tft.setTextDatum(TL_DATUM);
    }
  }
}

// ============================================================
// Touch handler: detect tap → toggle mode
// ============================================================
void handleTouch() {
  static uint16_t baseline = 0;
  uint16_t raw = touchRead(TOUCH_PIN);
  if (raw > baseline) baseline = raw;

  int16_t delta = (int16_t)baseline - (int16_t)raw;
  bool touched  = (delta > 15);
  unsigned long now = millis();

  if (touched && !touchWasActive && (now - lastToggle > TOUCH_COOLDOWN)) {
    switchMode();
    lastToggle = now;
  }
  touchWasActive = touched;

  Serial.printf("[Touch] raw=%d  baseline=%d  delta=%d  mode=%d\n",
                raw, baseline, delta, displayMode);
}

// ============================================================
// Main Loop
// ============================================================
void loop() {
  static unsigned long lastReport = 0, frameCount = 0;
  static unsigned long lastTouch  = 0;
  unsigned long now = millis();

  // ---- Touch: sample every 200 ms (both modes) ----
  if (now - lastTouch >= TOUCH_SAMPLE) {
    handleTouch();
    lastTouch = now;
  }

  // ---- DHT22: collect data in both modes (silent in mode 1) ----
  if (now - lastDHTRead >= 2000) {
    readDHT22();
    lastDHTRead = now;
  }

  // ---- Mode 0: Three-body + DHT22 display ----
  if (displayMode == 0) {
    for (int i = 0; i < NUM_BODIES; i++) decayTrail(i);
    for (int i = 0; i < NUM_BODIES; i++) eraseBody(i);
    for (int s = 0; s < SUBSTEPS; s++) stepSub(DT_SUB);
    for (int i = 0; i < NUM_BODIES; i++) pushTrail(i);
    for (int i = 0; i < NUM_BODIES; i++) drawTrail(i);
    for (int i = 0; i < NUM_BODIES; i++) drawBody(i);

    if (isEjected()) {
      Serial.println(">>> Ejection — reset <<<");
      tft.fillRect(0, 0, TFT_WIDTH, SIM_BOTTOM + 1, TFT_BLACK);
      randomSeed(micros());
      initSimulation();
      for (int i = 0; i < NUM_BODIES; i++) drawBody(i);
    }

    // Redraw chart only in monitor mode (data collected in both modes)
    static unsigned long lastChartDraw = 0;
    if (now - lastChartDraw >= 2000) {
      drawChart();
      lastChartDraw = now;
    }

    // Info branding (monitor mode only)
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Aurolystant Console", 2, 2);

    frameCount++;
    if (now - lastReport >= 5000) {
      float fps = frameCount * 1000.0f / (now - lastReport);
      Serial.printf("FPS: %.1f\n", fps);
      lastReport = now;
      frameCount = 0;
    }
  } else {
    // Mode 1: Bad Apple playback
    static unsigned long baLastFrame = 0;
    if (now - baLastFrame >= 62) {   // ~16 fps
      if (!badappleNextFrame()) {
        badappleReset();             // Loop
      }
      baLastFrame = now;
    }
  }
}
