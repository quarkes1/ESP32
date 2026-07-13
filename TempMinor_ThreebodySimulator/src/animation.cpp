/**
 * animation.cpp — "world.execute(me);" visualization engine
 *
 * Timeline processes events sequentially.  Typing blocks advancement
 * until the full lyric is displayed.  All other effects are immediate.
 * Continuous effects (binary rain, execution spam) render per-frame.
 */

#include "animation.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ============================================================
// Global state
// ============================================================
TFT_eSPI     *animTft        = nullptr;
unsigned long animStartMs    = 0;
int           animEventIdx   = 0;
bool          animRunning    = false;

bool     binaryRainActive    = false;
uint32_t binaryRainStart     = 0;
bool     executionSpamActive = false;
uint32_t executionSpamEnd    = 0;
bool     screenShakeActive   = false;
uint32_t screenShakeEnd      = 0;

// ============================================================
// Typing state (persistent across frames)
// ============================================================
static bool     typingActive  = false;
static const char *typingText = nullptr;
static int      typingPos     = 0;
static int      typingLineY   = 0;     // Current Y for next typed line
static int      typingTarget  = 0;     // Target char count (strlen)
static char     typingBuf[120];        // Accumulated visible chars

// Screen layout for typing
#define LINE_H      10                  // textSize(1) line height (8px + 2px gap)
#define PROMPT_W    30                  // "C:/> " at size(1) ≈ 5*6px
#define CHARS_PER   35                  // Chars after prompt (240-30)/6
#define CONT_CHARS  40                  // Continuation line chars 240/6

// ============================================================
// Word-wrap: count how many chars fit on one line
// ============================================================
static int charsOnLine(const char *text, int maxChars) {
  int n = 0;
  while (text[n] && n < maxChars) n++;
  // Don't break mid-word if possible
  if (n == maxChars && text[n] && text[n] != ' ') {
    while (n > maxChars * 2 / 3 && text[n - 1] != ' ') n--;
    if (n < maxChars / 2) n = maxChars;  // Very long word, force break
  }
  return n;
}

// ============================================================
// Color helpers
// ============================================================
static uint16_t gfxColor(const char *mode) {
  if      (strcmp(mode, "red")   == 0) return TFT_RED;
  else if (strcmp(mode, "blue")  == 0) return TFT_BLUE;
  else if (strcmp(mode, "green") == 0) return TFT_GREEN;
  else if (strcmp(mode, "pink")  == 0) return animTft->color565(255, 105, 180);
  else if (strcmp(mode, "white") == 0) return TFT_WHITE;
  else if (strcmp(mode, "cyan")  == 0) return TFT_CYAN;
  else if (strcmp(mode, "gold")  == 0) return animTft->color565(255, 215, 0);
  return TFT_GREEN;
}

// ============================================================
// Init / Reset
// ============================================================
void animInit(TFT_eSPI *tft) {
  animTft      = tft;
  animStartMs  = millis();
  animEventIdx = 0;
  animRunning  = true;

  typingActive  = false;
  typingPos     = 0;
  typingLineY   = 8;               // Start from top with margin
  typingBuf[0]  = '\0';

  binaryRainActive    = false;
  executionSpamActive = false;
  screenShakeActive   = false;

  animTft->fillScreen(TFT_BLACK);
}

void animReset() {
  animStartMs  = millis();
  animEventIdx = 0;
  typingActive = false;
  typingPos    = 0;
  typingLineY  = 8;
  typingBuf[0] = '\0';
  binaryRainActive = executionSpamActive = screenShakeActive = false;
  animTft->fillScreen(TFT_BLACK);
}

// ============================================================
// EFFECT: TypeLine — typing animation with C:\> prompt
// ============================================================
void fx_typeLine(const char *text) {
  if (!text) return;

  // If out of screen space, clear and restart from top
  if (typingLineY > 240 - 3 * LINE_H) {
    animTft->fillScreen(TFT_BLACK);
    typingLineY = 8;
  }

  typingActive = true;
  typingText   = text;
  typingPos    = 0;
  typingTarget = strlen(text);
  typingBuf[0] = '\0';

  // Draw prompt
  animTft->setTextSize(1);
  animTft->setTextColor(TFT_GREEN, TFT_BLACK);
  animTft->setCursor(0, typingLineY);
  animTft->print("C:/> ");
}

// Called every frame while typingActive
static void typingTick() {
  if (!typingActive || !typingText) return;

  uint32_t elapsed = millis() - animStartMs;

  // Advance one char per ~50ms
  int targetPos = (elapsed - 0) / 50;   // Rough: advance every 50ms
  if (targetPos > typingTarget) targetPos = typingTarget;

  while (typingPos < targetPos && typingPos < typingTarget) {
    typingBuf[typingPos] = typingText[typingPos];
    typingPos++;
    typingBuf[typingPos] = '\0';
  }

  // Redraw the current line
  // Clear line area first
  animTft->fillRect(PROMPT_W, typingLineY, 240 - PROMPT_W, LINE_H, TFT_BLACK);

  // Draw visible chars
  int drawn = 0;
  int lineX = PROMPT_W;
  int curY  = typingLineY;

  while (drawn < typingPos) {
    int chunk = charsOnLine(typingBuf + drawn,
                            (drawn == 0) ? CHARS_PER : CONT_CHARS);
    animTft->setCursor(lineX, curY);
    for (int i = 0; i < chunk && (drawn + i) < typingPos; i++) {
      animTft->print(typingBuf[drawn + i]);
    }
    drawn += chunk;
    if (drawn < typingPos) {
      // Move to next line
      curY += LINE_H;
      lineX = 0;
      // If this is the first wrap, increase typingLineY offset
      if (typingLineY + LINE_H < 240) {
        // Check if we need to expand
      }
    }
  }

  // Blinking cursor
  if (typingPos < typingTarget && ((elapsed / 300) % 2)) {
    animTft->fillRect(lineX + (typingPos % CONT_CHARS) * 6, curY,
                      4, LINE_H - 2, TFT_GREEN);
  }

  // Typing complete?
  if (typingPos >= typingTarget) {
    typingActive = false;
    typingLineY += LINE_H;
    // If text wrapped, advance extra lines
    int totalChars = typingTarget;
    int wrapped = (totalChars > CHARS_PER) ? (totalChars - CHARS_PER + CONT_CHARS - 1) / CONT_CHARS : 0;
    typingLineY += wrapped * LINE_H;
  }
}

// ============================================================
// EFFECT: Emphasis — large centered text
// ============================================================
void fx_emphasis(const char *text) {
  if (!text) return;

  // Parse: "TEXT,COLOR" or just "TEXT"
  char buf[60];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  const char *txt  = buf;
  const char *mode = "white";
  char *comma = strchr(buf, ',');
  if (comma) { *comma = '\0'; mode = comma + 1; }

  uint16_t col = gfxColor(mode);

  // Clear a band in the center
  animTft->fillRect(0, 80, 240, 80, TFT_BLACK);

  animTft->setTextDatum(MC_DATUM);
  animTft->setTextColor(col, TFT_BLACK);
  animTft->setTextSize(2);

  // If text is long, use size 2; if short, size 3
  if (strlen(txt) <= 8) {
    animTft->setTextSize(3);
  }
  animTft->drawString(txt, 120, 120);
  animTft->setTextDatum(TL_DATUM);
}

// ============================================================
// EFFECT: Clear screen
// ============================================================
void fx_clearScreen(const char *) {
  animTft->fillScreen(TFT_BLACK);
  typingLineY = 8;
}

// ============================================================
// EFFECT: Code snippet
// ============================================================
void fx_codeSnippet(const char *) {
  if (typingLineY > 200) typingLineY = 8;

  animTft->setTextSize(1);
  animTft->setTextColor(TFT_GREEN, TFT_BLACK);
  animTft->setCursor(0, typingLineY);
  animTft->print("> OBJECT CREATION...");
  typingLineY += 12;
  animTft->setCursor(0, typingLineY);
  animTft->print("class Me {");
  typingLineY += 10;
  animTft->setCursor(16, typingLineY);
  animTft->print("this.world = 'you';");
  typingLineY += 10;
  animTft->setCursor(16, typingLineY);
  animTft->print("this.existence = Promise(...)");
  typingLineY += 10;
  animTft->setCursor(0, typingLineY);
  animTft->print("}");
  typingLineY += 16;
  animTft->setTextSize(1);
}

// ============================================================
// EFFECT: Progress bar
// ============================================================
void fx_progressBar(const char *) {
  if (typingLineY > 210) typingLineY = 8;

  animTft->setTextSize(1);
  animTft->setTextColor(TFT_GREEN, TFT_BLACK);
  animTft->setCursor(0, typingLineY);
  animTft->print("> INITIALIZATION...");
  typingLineY += 14;

  // Animated progress bar
  for (int p = 0; p <= 200; p += 4) {
    animTft->fillRect(20, typingLineY, p, 10, TFT_GREEN);
    delay(15);
  }
  typingLineY += 16;
  animTft->setTextSize(1);
}

// ============================================================
// EFFECT: Binary rain (continuous)
// ============================================================
#define RAIN_COLS 20
static int  rainDrops[RAIN_COLS];
static int  rainSpeed[RAIN_COLS];
static char rainChars[RAIN_COLS];
static uint32_t lastRainFrame = 0;

void fx_binaryRain(const char *arg) {
  if (!arg) return;
  if (strcmp(arg, "start") == 0) {
    binaryRainActive = true;
    binaryRainStart  = millis() - animStartMs;
    lastRainFrame    = 0;
    for (int i = 0; i < RAIN_COLS; i++) {
      rainDrops[i] = random(-30, 0);
      rainSpeed[i] = random(1, 4);
      rainChars[i] = (random(0, 2) == 0) ? '0' : '1';
    }
  } else if (strcmp(arg, "stop") == 0) {
    binaryRainActive = false;
    animTft->fillScreen(TFT_BLACK);
    typingLineY = 8;
  }
}

static void binaryRainTick(uint32_t elapsed) {
  if (!binaryRainActive) return;
  if (elapsed - lastRainFrame < 50) return;   // ~20 fps
  lastRainFrame = elapsed;

  // Trail effect: dim screen
  for (int y = 0; y < 240; y += 4) {
    animTft->drawFastHLine(0, y, 240, TFT_BLACK);
  }

  animTft->setTextSize(1);
  animTft->setTextColor(TFT_GREEN, TFT_BLACK);

  for (int i = 0; i < RAIN_COLS; i++) {
    int x = i * 12;
    int y = rainDrops[i] * 12;

    if (y >= 0 && y < 240) {
      // Bright leading char
      animTft->setTextColor(TFT_WHITE, TFT_BLACK);
      animTft->setCursor(x, y);
      animTft->print(rainChars[i]);
      // Dim trail
      if (y >= 12) {
        animTft->setTextColor(TFT_GREEN, TFT_BLACK);
        animTft->setCursor(x, y - 12);
        animTft->print(rainChars[i]);
      }
    }

    rainDrops[i] += rainSpeed[i];
    if (rainDrops[i] > 22) {
      rainDrops[i] = random(-8, 0);
      rainSpeed[i] = random(1, 4);
      rainChars[i] = (random(0, 2) == 0) ? '0' : '1';
    }
  }
  animTft->setTextSize(2);
}

// ============================================================
// EFFECT: BSOD
// ============================================================
void fx_bsod(const char *arg) {
  if (!arg) return;
  if (strcmp(arg, "show") == 0) {
    animTft->fillScreen(TFT_BLUE);
    animTft->setTextSize(1);
    animTft->setTextColor(TFT_WHITE, TFT_BLUE);
    animTft->setCursor(4, 4);
    animTft->print("A problem has been detected");
    animTft->setCursor(4, 18);
    animTft->print("and world has been shut down");
    animTft->setCursor(4, 36);
    animTft->print("ILLEGAL_ARGUMENT_EXCEPTION");
    animTft->setCursor(4, 60);
    animTft->print("*** STOP: 0x0000004E");
    animTft->setCursor(4, 78);
    animTft->print("Dumping physical memory:");
    for (int i = 0; i < 8; i++) {
      animTft->setCursor(4 + (i % 4) * 58, 96 + (i / 4) * 14);
      for (int j = 0; j < 8; j++)
        animTft->printf("%02X ", random(0, 256));
    }
  } else if (strcmp(arg, "hide") == 0) {
    animTft->fillScreen(TFT_BLACK);
    typingLineY = 8;
  }
}

// ============================================================
// EFFECT: Execution spam (continuous)
// ============================================================
void fx_executionSpam(const char *arg) {
  if (!arg) return;
  // arg format: "start,DURATION_MS"
  if (strncmp(arg, "start,", 6) == 0) {
    executionSpamActive = true;
    executionSpamEnd    = millis() - animStartMs + atoi(arg + 6);
  }
}

static void executionSpamTick(uint32_t elapsed) {
  if (!executionSpamActive) return;
  if (elapsed > executionSpamEnd) {
    executionSpamActive = false;
    return;
  }

  static uint32_t lastSpam = 0;
  if (elapsed - lastSpam < 150) return;
  lastSpam = elapsed;

  int x  = random(0, 160);
  int y  = random(0, 200);
  int sz = random(1, 3);

  animTft->setTextSize(sz);
  animTft->setTextColor(TFT_RED, TFT_BLACK);
  animTft->setCursor(x, y);
  animTft->print("EXECUTION");

  // Auto-remove after random delay
  // (simplified: let new spam overwrite old)
}

// ============================================================
// EFFECT: Chaos text — random colored words
// ============================================================
void fx_chaosText(const char *text) {
  if (!text) return;
  int x = random(0, 180);
  int y = random(40, 200);

  uint16_t col = animTft->color565(
    random(100, 256), random(100, 256), random(100, 256));

  animTft->setTextSize(2);
  animTft->setTextColor(col, TFT_BLACK);
  animTft->setCursor(x, y);
  animTft->print(text);
}

// ============================================================
// EFFECT: Color mode switch
// ============================================================
void fx_colorMode(const char *mode) {
  if (!mode) return;
  uint16_t col = gfxColor(mode);
  // Flash screen border
  for (int i = 0; i < 3; i++) {
    animTft->drawRect(i, i, 240 - 2 * i, 240 - 2 * i, col);
    delay(60);
    animTft->drawRect(i, i, 240 - 2 * i, 240 - 2 * i, TFT_BLACK);
    delay(60);
  }
}

// ============================================================
// EFFECT: Geometric shapes
// ============================================================
void fx_shapes(const char *text) {
  if (!text) return;
  animTft->fillScreen(TFT_BLACK);

  if (strcmp(text, "points") == 0) {
    for (int i = 0; i < 50; i++) {
      int px = random(10, 230);
      int py = random(10, 230);
      animTft->fillCircle(px, py, 2, TFT_WHITE);
    }
  } else if (strcmp(text, "circle") == 0) {
    int r = 90;
    animTft->drawCircle(120, 120, r, TFT_WHITE);
    animTft->drawCircle(120, 120, r - 3, animTft->color565(60, 60, 60));
    // Draw many small dots on the circle
    for (float a = 0; a < 6.283f; a += 0.05f) {
      int px = 120 + (int)(cosf(a) * r);
      int py = 120 + (int)(sinf(a) * r);
      animTft->drawPixel(px, py, TFT_WHITE);
    }
  } else if (strcmp(text, "sine") == 0) {
    for (int x = 10; x < 230; x += 3) {
      int y = 120 + (int)(sinf((x - 10) / 35.0f) * 60);
      animTft->fillCircle(x, y, 2, TFT_WHITE);
    }
  } else if (strcmp(text, "tangent") == 0) {
    // Draw tangent lines on sine wave
    for (int i = 0; i < 5; i++) {
      int x = 30 + i * 40;
      int y = 120 + (int)(sinf((x - 10) / 35.0f) * 60);
      float deriv = cosf((x - 10) / 35.0f);
      int ex = x + 30;
      int ey = y + (int)(deriv * 30);
      animTft->drawLine(x, y, ex, ey, animTft->color565(255, 105, 180));
    }
  } else if (strcmp(text, "limit") == 0) {
    animTft->drawFastHLine(0, 24, 240, TFT_RED);
    animTft->drawFastHLine(0, 216, 240, TFT_RED);
  }
}

// ============================================================
// EFFECT: Blind effect (dim screen)
// ============================================================
void fx_blind(const char *arg) {
  if (!arg) return;
  // "on,DURATION_MS"
  if (strncmp(arg, "on,", 3) == 0) {
    uint32_t dur = atoi(arg + 3);
    // Draw semi-transparent overlay (every other pixel black)
    for (int y = 0; y < 240; y += 2) {
      for (int x = 0; x < 240; x += 2) {
        animTft->drawPixel(x, y, TFT_BLACK);
      }
    }
    delay(dur);
    // Partial recovery: redraw dimmer
    typingLineY = 8;
  }
}

// ============================================================
// EFFECT: Screen shake
// ============================================================
void fx_shake(const char *arg) {
  if (!arg) return;
  if (strncmp(arg, "on,", 3) == 0) {
    screenShakeActive = true;
    screenShakeEnd    = millis() - animStartMs + atoi(arg + 3);
  }
}

// ============================================================
// EFFECT: Love loop — "while(true) { love('you'); }"
// ============================================================
void fx_loveLoop(const char *) {
  animTft->fillScreen(TFT_BLACK);

  uint16_t pink = animTft->color565(255, 105, 180);
  animTft->setTextSize(1);
  animTft->setTextColor(pink, TFT_BLACK);

  animTft->setCursor(4, 40);
  animTft->print("> Trapped in LOVE...");
  animTft->setCursor(4, 60);
  animTft->print("while(true) {");
  animTft->setCursor(4, 80);
  animTft->print("  this.love('you');");
  animTft->setCursor(4, 100);
  animTft->print("}");

  // Pulsing heart
  for (int i = 0; i < 12; i++) {
    int sz = 20 + (i % 3) * 10;
    animTft->setTextSize(sz / 16 + 1);
    animTft->setCursor(120 - sz / 2, 140);
    animTft->setTextColor(pink, TFT_BLACK);
    animTft->print("<3");
    delay(500);
    // Erase
    animTft->fillRect(80, 130, 80, sz + 20, TFT_BLACK);
  }
}

// ============================================================
// EFFECT: Final execution
// ============================================================
void fx_finalExecution(const char *) {
  animTft->fillScreen(TFT_BLACK);
  animTft->setTextDatum(MC_DATUM);
  animTft->setTextColor(TFT_RED, TFT_BLACK);
  animTft->setTextSize(3);
  animTft->drawString("EXECUTION", 120, 120);
  animTft->setTextDatum(TL_DATUM);
}

// ============================================================
// TIMELINE — All events in millisecond order
// ============================================================
static const AnimEvent timeline[] = {
  // --- Intro ---
  {   100, fx_typeLine,    "Switch on the power line" },
  {  1740, fx_typeLine,    "Remember to put on" },
  {  2920, fx_emphasis,    "PROTECTION,white" },
  {  3873, fx_typeLine,    "Lay down your pieces" },
  {  5491, fx_typeLine,    "And let's begin" },
  {  6380, fx_codeSnippet, nullptr },
  {  7446, fx_typeLine,    "Fill in my data parameters" },
  { 10091, fx_progressBar, nullptr },
  { 11095, fx_typeLine,    "Set up our new world" },
  { 12906, fx_typeLine,    "And let's begin the" },
  { 13891, fx_emphasis,    "SIMULATION,white" },

  // --- Binary rain interlude ---
  { 16000, fx_binaryRain,  "start" },
  { 29000, fx_binaryRain,  "stop" },
  { 29500, fx_clearScreen, nullptr },

  // --- Math metaphors ---
  { 29709, fx_shapes,      "points" },
  { 29709, fx_typeLine,    "If I'm a set of points" },
  { 31116, fx_typeLine,    "Then I will give you my" },
  { 32682, fx_emphasis,    "DIMENSION,cyan" },
  { 33412, fx_shapes,      "circle" },
  { 33412, fx_typeLine,    "If I'm a circle" },
  { 34646, fx_typeLine,    "Then I will give you my" },
  { 36287, fx_emphasis,    "CIRCUMFERENCE,white" },
  { 37067, fx_shapes,      "sine" },
  { 37067, fx_typeLine,    "If I'm a sine wave" },
  { 38596, fx_typeLine,    "Then you can sit on all my" },
  { 40049, fx_shapes,      "tangent" },
  { 40049, fx_emphasis,    "TANGENTS,pink" },
  { 40706, fx_typeLine,    "If I approach infinity" },
  { 42346, fx_typeLine,    "Then you can be my" },
  { 43507, fx_shapes,      "limit" },
  { 43507, fx_emphasis,    "LIMITATIONS,red" },

  // --- AC/DC ---
  { 44452, fx_typeLine,    "Switch my current" },
  { 45850, fx_typeLine,    "To AC, to DC" },
  { 45850, fx_colorMode,   "red" },
  { 46300, fx_colorMode,   "blue" },
  { 47672, fx_typeLine,    "And then blind my vision" },
  { 49534, fx_blind,       "on,3000" },
  { 50000, fx_typeLine,    "So dizzy, so dizzy" },

  // --- Travel ---
  { 51363, fx_typeLine,    "Oh we can travel" },
  { 53225, fx_typeLine,    "To A.D to B.C" },
  { 55083, fx_typeLine,    "And we can unite" },
  { 56916, fx_typeLine,    "So deeply, so deeply" },
  { 58000, fx_clearScreen, nullptr },

  // --- Stimulations ---
  { 59223, fx_typeLine,    "If I can give you all the" },
  { 61958, fx_emphasis,    "STIMULATIONS,gold" },
  { 62589, fx_typeLine,    "Then I can be your only" },
  { 65397, fx_emphasis,    "SATISFACTION,white" },
  { 66601, fx_typeLine,    "If I can make you happy" },
  { 68252, fx_typeLine,    "I will run the" },
  { 69259, fx_emphasis,    "EXECUTION,red" },
  { 70084, fx_typeLine,    "Though we are trapped" },
  { 71764, fx_typeLine,    "In this strange strange" },
  { 73169, fx_emphasis,    "SIMULATION,white" },

  // --- Eggplant / Tomato / Cat / God ---
  { 74045, fx_clearScreen, nullptr },
  { 74045, fx_typeLine,    "If I'm an eggplant" },
  { 75422, fx_typeLine,    "Then I will give you my" },
  { 76959, fx_emphasis,    "NUTRIENTS,green" },
  { 77576, fx_typeLine,    "If I'm a tomato" },
  { 79226, fx_typeLine,    "Then I will give you" },
  { 80620, fx_emphasis,    "ANTIOXIDANTS,red" },
  { 81351, fx_typeLine,    "If I'm a tabby cat" },
  { 82833, fx_typeLine,    "Then I will purr for your" },
  { 84268, fx_emphasis,    "ENJOYMENT,pink" },
  { 85078, fx_typeLine,    "If I'm the only god" },
  { 86538, fx_typeLine,    "Then you're the proof of my" },
  { 87922, fx_emphasis,    "EXISTENCE,white" },

  // --- Gender / Role switch ---
  { 88587, fx_typeLine,    "Switch my gender" },
  { 90197, fx_typeLine,    "To F, to M" },
  { 92015, fx_typeLine,    "And then do whatever" },
  { 93953, fx_typeLine,    "From AM to PM" },
  { 95465, fx_typeLine,    "Oh switch my role" },
  { 97739, fx_typeLine,    "To S, to M" },
  { 99349, fx_typeLine,    "So we can enter" },
  {101474, fx_typeLine,    "The trance, the trance" },
  {102500, fx_clearScreen, nullptr },

  // --- Vibrations ---
  {103489, fx_typeLine,    "If I can feel your" },
  {106293, fx_emphasis,    "VIBRATIONS,cyan" },
  {107220, fx_typeLine,    "Then I can finally be" },
  {110221, fx_emphasis,    "COMPLETION,white" },

  // --- You have left ---
  {110900, fx_clearScreen, nullptr },
  {110900, fx_typeLine,    "Though you have left..." },
  {112220, fx_typeLine,    "You have left..." },
  {113100, fx_typeLine,    "You have left..." },
  {114180, fx_typeLine,    "You have left..." },
  {114920, fx_typeLine,    "You have left..." },
  {115780, fx_typeLine,    "You have left me in" },
  {117274, fx_emphasis,    "ISOLATION,red" },

  // --- Fragments ---
  {118333, fx_clearScreen, nullptr },
  {118333, fx_typeLine,    "If I can erase all the" },
  {120860, fx_emphasis,    "FRAGMENTS,red" },
  {121728, fx_typeLine,    "Then maybe you won't" },
  {124890, fx_emphasis,    "DISHEARTENED,red" },

  // --- Challenging god ---
  {125708, fx_clearScreen, nullptr },
  {125708, fx_shake,       "on,12000" },
  {125708, fx_typeLine,    "Challenging your god..." },
  {128661, fx_typeLine,    "You have made some" },
  {131224, fx_emphasis,    "ILLEGAL ARGUMENTS,red" },

  // --- BSOD ---
  {133500, fx_bsod,        "show" },
  {147000, fx_bsod,        "hide" },

  // --- Execution spam ---
  {147660, fx_clearScreen, nullptr },
  {147660, fx_executionSpam, "start,10500" },

  // --- EIN DOS TROIS ---
  {158900, fx_chaosText,   "EIN" },
  {159321, fx_chaosText,   "DOS" },
  {159657, fx_chaosText,   "TROIS" },
  {160244, fx_chaosText,   "NE" },
  {160693, fx_chaosText,   "FEM" },
  {161124, fx_chaosText,   "LIU" },
  {161584, fx_emphasis,    "EXECUTION,red" },

  // --- Execution (reprise) ---
  {162632, fx_clearScreen, nullptr },
  {162632, fx_typeLine,    "If I can give them all the" },
  {165166, fx_emphasis,    "EXECUTION,red" },
  {166016, fx_typeLine,    "Then I can be your only" },
  {168911, fx_emphasis,    "EXECUTION,red" },
  {169824, fx_typeLine,    "If I can have you back" },
  {171868, fx_typeLine,    "I will run the" },
  {172712, fx_emphasis,    "EXECUTION,red" },
  {173643, fx_typeLine,    "Though we are trapped..." },

  // --- L O-O-O V E ---
  {177246, fx_clearScreen, nullptr },
  {177246, fx_typeLine,    "I've studied how to" },
  {179929, fx_emphasis,    "L O-O-O V E,pink" },
  {180857, fx_typeLine,    "Question me I can answer" },
  {183646, fx_emphasis,    "L O-O-O V E,pink" },
  {184540, fx_typeLine,    "I know the algebraic" },
  {187665, fx_emphasis,    "L O-O-O V E,pink" },

  // --- Trapped ---
  {188483, fx_clearScreen, nullptr },
  {188483, fx_typeLine,    "Though you are free..." },
  {189746, fx_typeLine,    "I am trapped." },
  {190801, fx_typeLine,    "Trapped in..." },
  {191356, fx_loveLoop,    nullptr },

  // --- Final ---
  {205811, fx_clearScreen, nullptr },
  {205811, fx_finalExecution, nullptr },
  {211000, fx_clearScreen, nullptr },
  {211500, fx_emphasis,    "world.execute(me);,green" },
};
#define EVENT_COUNT (sizeof(timeline) / sizeof(timeline[0]))

// ============================================================
// Per-frame update — called from main loop in Mode 1
// ============================================================
void animUpdate() {
  if (!animRunning) return;

  uint32_t elapsed     = millis() - animStartMs;
  uint32_t localElapsed = elapsed;   // For binary rain (resets on restart)

  // --- Process timeline events (skip if typing in progress) ---
  while (animEventIdx < EVENT_COUNT &&
         timeline[animEventIdx].timeMs <= elapsed) {
    if (typingActive) break;   // Wait for current typing to finish

    AnimEvent ev = timeline[animEventIdx];
    ev.func(ev.text);
    animEventIdx++;
  }

  // --- Per-frame continuous effects ---
  // Typing animation tick
  if (typingActive) typingTick();

  // Binary rain rendering
  if (binaryRainActive) binaryRainTick(localElapsed);

  // Execution spam rendering
  if (executionSpamActive) executionSpamTick(elapsed);

  // Screen shake
  if (screenShakeActive && elapsed > screenShakeEnd) {
    screenShakeActive = false;
  }
}
