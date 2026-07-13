/**
 * badapple.h — Bad Apple RLE frame playback engine
 *
 * Frame data stored as one SPIFFS file: /badapple.rle
 *   Header: uint32_t frameCount, uint32_t offsets[frameCount]
 *   Body:   per-frame RLE data (binary runs, alternating black/white)
 */

#pragma once
#include <TFT_eSPI.h>

// ---- RLE format ----
// Frame: sequence of uint8_t run lengths.
//   Run 0 = white, run 1 = black, run 2 = white, ...
//   Max run: 255.  Longer runs use consecutive entries.
//   Sum of all run lengths per frame = 240 * 240 = 57600.
//
// File layout:
//   uint32_t frameCount
//   uint32_t offsets[frameCount]   (byte offset of each frame, 0-based from file start)
//   [frame 0 RLE data]
//   [frame 1 RLE data]
//   ...

// ---- API ----
bool  badappleInit(TFT_eSPI *tft);          // Mount SPIFFS, open file, return true on success
void  badappleClose();                       // Clean up
bool  badappleNextFrame();                   // Draw next frame, return false at end
int   badappleFrameCount();                  // Total frames in file
int   badappleCurrentFrame();                // Current frame index (0-based)
void  badappleReset();                       // Rewind to frame 0
