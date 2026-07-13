/**
 * animation.h — "world.execute(me);" lyric visualization
 *
 * Timeline-driven: millis()-based event sequencer.
 * Effects dispatch at specific timestamps matching the song.
 * Continuous effects (binary rain, execution spam) update per-frame.
 */

#pragma once
#include <TFT_eSPI.h>

// ---- Effect function signature ----
typedef void (*AnimFunc)(const char *arg);

// ---- Timeline event ----
struct AnimEvent {
  uint32_t  timeMs;      // Milliseconds from animation start
  AnimFunc  func;        // Effect to trigger
  const char *text;      // Text argument (lyric string or NULL)
};

// ---- Animation state (extern, defined in animation.cpp) ----
extern TFT_eSPI     *animTft;           // Pointer to main TFT instance
extern unsigned long animStartMs;       // millis() at animation start
extern int           animEventIdx;      // Next timeline event to process
extern bool          animRunning;       // True while animation is active

// Persistent effect state
extern bool     binaryRainActive;
extern uint32_t binaryRainStart;
extern bool     executionSpamActive;
extern uint32_t executionSpamEnd;
extern bool     screenShakeActive;
extern uint32_t screenShakeEnd;

// ---- API ----
void animInit(TFT_eSPI *tft);           // Called once when entering animation mode
void animUpdate();                       // Called every frame in Mode 1
void animReset();                        // Reset state for replay

// ---- Effect functions ----
void fx_typeLine(const char *text);
void fx_emphasis(const char *text);
void fx_clearScreen(const char *text);
void fx_codeSnippet(const char *text);
void fx_progressBar(const char *text);
void fx_binaryRain(const char *text);        // "start" or "stop"
void fx_bsod(const char *text);              // "show" or "hide"
void fx_executionSpam(const char *text);     // "start,DURATION_MS"
void fx_chaosText(const char *text);
void fx_colorMode(const char *text);         // "red" / "blue" / "green" / "pink" / "white"
void fx_shapes(const char *text);            // "points" / "circle" / "sine" / "tangent" / "limit"
void fx_blind(const char *text);             // "on,DURATION_MS"
void fx_shake(const char *text);             // "on,DURATION_MS"
void fx_loveLoop(const char *text);
void fx_finalExecution(const char *text);
