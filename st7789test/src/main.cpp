/**
 * Three-Body Problem — Chenciner-Montgomery Figure-8 + Decay Trails
 *
 * Trail: each point born with brightness=1.0, decays by TRAIL_DECAY per frame.
 * No explicit erase — the pixel just gets dimmer until it vanishes.
 * Body needs explicit erase (black circle at old pos) since it moves.
 */

#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

// ============================================================
// Physics (dimensionless)
// ============================================================
#define NUM_BODIES    3
#define G_GRAV        1.0f
#define SOFTENING     0.05f
#define SUBSTEPS      8
#define DT_SUB        0.001f         // Orbit period ≈ 13 s @ 20 fps

// ============================================================
// Display
// ============================================================
#define SCALE         70.0f
#define CX            (TFT_WIDTH  / 2)
#define CY            (TFT_HEIGHT / 2)
#define EJECT_LIMIT   2.8f

// ============================================================
// Trail — decay-based (no explicit erase needed)
// ============================================================
#define TRAIL_LEN     200            // 5× longer buffer
#define TRAIL_DECAY   0.005f         // 1/5th speed (~185 frame lifetime)
#define TRAIL_MIN_BRT 0.08f

struct TrailPoint {
  int16_t sx, sy;                    // Screen position
  float   brightness;                // 1.0 (new) → 0.0 (dead, removed)
};

struct TrailBuf {
  TrailPoint pts[TRAIL_LEN];
  uint16_t   head;                   // Next write index
  uint16_t   count;                  // Valid entries
};

// ============================================================
// Body
// ============================================================
struct Body {
  float x, y, vx, vy;
  float mass;
  float radius;
};

Body     bodies[NUM_BODIES];
uint16_t starColor[NUM_BODIES];
TrailBuf trail[NUM_BODIES];
int16_t  prevSX[NUM_BODIES], prevSY[NUM_BODIES];

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
// Check if a screen pixel lies inside any body's visible disc
// ============================================================
bool insideAnyBody(int16_t sx, int16_t sy) {
  for (int i = 0; i < NUM_BODIES; i++) {
    int16_t bx = scrX(bodies[i].x);
    int16_t by = scrY(bodies[i].y);
    int16_t r  = (int16_t)(bodies[i].radius);       // core only — tight fit
    int16_t dx = sx - bx, dy = sy - by;
    if (dx * dx + dy * dy <= r * r) return true;
  }
  return false;
}

// ============================================================
// Decay all trail points.  When brightness hits 0: paint BLACK
// at that pixel once, then remove the point.  This is the ONLY
// erase the trail needs — one black pixel per dead point.
// Skip erase if the pixel is inside a body (would cause flicker).
// ============================================================
void decayTrail(int idx) {
  TrailBuf &tb = trail[idx];
  for (uint16_t i = 0; i < tb.count; i++) {
    uint16_t pos = (tb.head - tb.count + i + TRAIL_LEN) % TRAIL_LEN;
    tb.pts[pos].brightness -= TRAIL_DECAY;
  }
  // Oldest always die first → erase with black pixel, then remove
  while (tb.count > 0) {
    uint16_t oldest = (tb.head - tb.count + TRAIL_LEN) % TRAIL_LEN;
    if (tb.pts[oldest].brightness <= TRAIL_MIN_BRT) {
      tft.drawPixel(tb.pts[oldest].sx, tb.pts[oldest].sy, TFT_BLACK);
      tb.count--;
    } else {
      break;
    }
  }
}

// ============================================================
// Push current body position onto trail
// ============================================================
void pushTrail(int idx) {
  TrailBuf &tb = trail[idx];
  int16_t sx = scrX(bodies[idx].x);
  int16_t sy = scrY(bodies[idx].y);

  // Skip duplicate positions
  if (tb.count > 0) {
    uint16_t last = (tb.head - 1 + TRAIL_LEN) % TRAIL_LEN;
    if (tb.pts[last].sx == sx && tb.pts[last].sy == sy) return;
  }

  tb.pts[tb.head].sx         = sx;
  tb.pts[tb.head].sy         = sy;
  tb.pts[tb.head].brightness = 1.0f;
  tb.head = (tb.head + 1) % TRAIL_LEN;
  if (tb.count < TRAIL_LEN) tb.count++;
}

// ============================================================
// Draw trail: each point at its stored brightness (no erase)
// ============================================================
void drawTrail(int idx) {
  TrailBuf &tb = trail[idx];
  if (tb.count < 2) return;

  uint16_t baseColor = starColor[idx];

  for (uint16_t i = 0; i < tb.count; i++) {
    uint8_t pos = (tb.head - tb.count + i + TRAIL_LEN) % TRAIL_LEN;
    float   brt = tb.pts[pos].brightness;
    if (brt <= TRAIL_MIN_BRT) continue;

    // Always radius=1 — no transition artifacts between frames
    uint16_t col = fadeColor(baseColor, brt);
    tft.drawPixel(tb.pts[pos].sx, tb.pts[pos].sy, col);
  }
}

// ============================================================
// Erase / draw body
// ============================================================
void eraseBody(int idx) {
  int16_t cx = prevSX[idx];
  int16_t cy = prevSY[idx];
  int16_t r  = (int16_t)(bodies[idx].radius) + 2;

  // Pixel-by-pixel: skip anything inside a body → zero body flicker
  for (int16_t dy = -r; dy <= r; dy++) {
    for (int16_t dx = -r; dx <= r; dx++) {
      if (dx * dx + dy * dy > r * r) continue;
      int16_t sx = cx + dx;
      int16_t sy = cy + dy;
      if (sx < 0 || sx >= TFT_WIDTH || sy < 0 || sy >= TFT_HEIGHT) continue;
      if (!insideAnyBody(sx, sy))
        tft.drawPixel(sx, sy, TFT_BLACK);
    }
  }
}

void drawBody(int idx) {
  int16_t sx = scrX(bodies[idx].x);
  int16_t sy = scrY(bodies[idx].y);
  int16_t r  = (int16_t)(bodies[idx].radius);

  if (r >= 3) {
    tft.fillCircle(sx, sy, r + 1, tft.color565(35, 30, 18));   // Glow
  }
  tft.fillCircle(sx, sy, r, starColor[idx]);                    // Core

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
      float fx = f * dx / d;
      float fy = f * dy / d;
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

// ============================================================
// Ejection check
// ============================================================
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
// Init: Chenciner-Montgomery figure-8 (±15% mass)
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
  bodies[2] = { 0.0f, 0.0f, -2.0f*vx1, -2.0f*vy1, m3,
                3.5f + m3 * 1.5f };

  for (int i = 0; i < NUM_BODIES; i++) {
    bodies[i].vx += random(-60, 61) / 1000.0f;
    bodies[i].vy += random(-60, 61) / 1000.0f;
  }

  // Zero net momentum
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
  for (int i = 0; i < NUM_BODIES; i++) {
    Serial.printf("  S%d: m=%.2f  pos=(%+.2f,%+.2f)\n",
                  i, bodies[i].mass, bodies[i].x, bodies[i].y);
  }
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Three-Body Figure-8 + Decay Trails ===");
  Serial.printf("Decay: %.3f/frame  |  buffer: %d pts\n", TRAIL_DECAY, TRAIL_LEN);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  #if (TFT_BL >= 0)
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  #endif

  randomSeed(analogRead(0) + micros());
  initSimulation();

  for (int i = 0; i < NUM_BODIES; i++) drawBody(i);
}

// ============================================================
// Main Loop
// ============================================================
void loop() {
  static unsigned long lastReport = 0, frameCount = 0;

  // ---- 1. Decay trail brightness + remove dead points ----
  for (int i = 0; i < NUM_BODIES; i++) decayTrail(i);

  // ---- 2. Erase old body positions ----
  for (int i = 0; i < NUM_BODIES; i++) eraseBody(i);

  // ---- 3. Physics ----
  for (int s = 0; s < SUBSTEPS; s++) stepSub(DT_SUB);

  // ---- 4. Push new trail points ----
  for (int i = 0; i < NUM_BODIES; i++) pushTrail(i);

  // ---- 5. Draw trail (no erase — auto-fade) + body ----
  for (int i = 0; i < NUM_BODIES; i++) drawTrail(i);
  for (int i = 0; i < NUM_BODIES; i++) drawBody(i);

  // ---- 6. Ejection → reset ----
  if (isEjected()) {
    Serial.println(">>> Ejection — reset <<<");
    tft.fillScreen(TFT_BLACK);
    randomSeed(micros());
    initSimulation();
    for (int i = 0; i < NUM_BODIES; i++) drawBody(i);
  }

  // ---- 7. FPS ----
  frameCount++;
  unsigned long now = millis();
  if (now - lastReport >= 5000) {
    float fps = frameCount * 1000.0f / (now - lastReport);
    Serial.printf("FPS: %.1f\n", fps);
    lastReport = now;
    frameCount = 0;
  }
}
