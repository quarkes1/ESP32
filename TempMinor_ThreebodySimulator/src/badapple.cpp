/**
 * badapple.cpp — SPIFFS RLE playback engine
 */

#include "badapple.h"
#include <SPIFFS.h>

static TFT_eSPI *tft   = nullptr;
static fs::File  file;
static uint32_t  frameCount = 0;
static uint32_t *offsets    = nullptr;
static int       currentFrame = 0;
static bool      initialized  = false;

// Decode buffer
#define RLE_BUF_SIZE  4096
static uint8_t rleBuf[RLE_BUF_SIZE];

bool badappleInit(TFT_eSPI *display) {
  tft = display;
  if (!SPIFFS.begin(true)) {
    Serial.println("[BadApple] SPIFFS mount failed");
    return false;
  }

  file = SPIFFS.open("/badapple.rle", "r");
  if (!file) {
    Serial.println("[BadApple] badapple.rle not found");
    return false;
  }

  // Read header: frameCount
  file.read((uint8_t *)&frameCount, 4);
  Serial.printf("[BadApple] %u frames\n", frameCount);

  // Read offset table
  size_t  offSize = frameCount * sizeof(uint32_t);
  offsets = (uint32_t *)malloc(offSize);
  if (!offsets) {
    Serial.println("[BadApple] malloc failed");
    file.close();
    return false;
  }
  file.read((uint8_t *)offsets, offSize);

  currentFrame = 0;
  initialized  = true;
  return true;
}

void badappleClose() {
  if (offsets) { free(offsets); offsets = nullptr; }
  if (file) file.close();
  SPIFFS.end();
  initialized = false;
}

int badappleFrameCount()  { return (int)frameCount; }
int badappleCurrentFrame() { return currentFrame; }

void badappleReset() {
  currentFrame = 0;
}

bool badappleNextFrame() {
  if (!initialized || currentFrame >= (int)frameCount) return false;

  // ---- Seek to frame data ----
  uint32_t dataStart = 4 + frameCount * sizeof(uint32_t);
  uint32_t frameOff  = dataStart + offsets[currentFrame];

  // Next frame offset (or EOF)
  uint32_t nextOff;
  if (currentFrame + 1 < (int)frameCount)
    nextOff = dataStart + offsets[currentFrame + 1];
  else
    nextOff = file.size();

  uint32_t frameSize = nextOff - frameOff;
  if (frameSize > RLE_BUF_SIZE) {
    Serial.printf("[BadApple] Frame %d too large: %u bytes\n", currentFrame, frameSize);
    currentFrame++;
    return true;  // Skip corrupted frame
  }

  file.seek(frameOff);
  file.read(rleBuf, frameSize);

  // ---- Decode RLE into TFT ----
  // First byte: initial color (0=black, 1=white)
  bool white = (rleBuf[0] == 1);

  tft->startWrite();

  int x = 0, y = 0;
  uint16_t color;

  for (uint32_t i = 1; i < frameSize; i++) {
    uint8_t run = rleBuf[i];

    if (run > 0) {
      color = white ? TFT_WHITE : TFT_BLACK;
      int remain = run;

      while (remain > 0) {
        int space = 240 - x;
        int seg   = (remain < space) ? remain : space;

        tft->drawFastHLine(x, y, seg, color);

        x += seg;
        remain -= seg;

        if (x >= 240) {
          x = 0;
          y++;
          if (y >= 240) { remain = 0; y = 239; }
        }
      }
    }

    white = !white;   // Always toggle — encoder inserts [0] for long runs
  }

  tft->endWrite();
  currentFrame++;
  return true;
}
