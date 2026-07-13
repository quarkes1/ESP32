#!/usr/bin/env python3
"""
Convert Bad Apple video to RLE frame file for ESP32 SPIFFS playback.

Usage:
  python tools/convert_badapple.py <input_video> [--fps 8] [--width 240] [--height 240]

Output: data/badapple.rle  (ready for `pio run -t uploadfs`)

Requirements: ffmpeg, Pillow
  pip install Pillow
"""

import subprocess
import struct
import sys
import os
import argparse
from io import BytesIO

def _flush_run(runs, count):
    """Flush a run, inserting [255,0] pairs for segments >255 so decoder toggles correctly."""
    while count >= 255:
        runs.append(255)
        runs.append(0)       # Zero-length run toggles color back
        count -= 255
    if count > 0:
        runs.append(count)


def rle_encode_frame(img, width, height):
    """Convert PIL Image (L mode) to RLE bytes.  First byte = initial color (0=black, 1=white)."""
    pixels = img.tobytes()
    runs = []

    # First byte: initial color
    first_is_white = (pixels[0] >= 128)
    runs.append(1 if first_is_white else 0)

    white = first_is_white
    count = 0

    for y in range(height):
        for x in range(width):
            px = pixels[y * width + x]
            is_white = px >= 128

            if is_white == white:
                count += 1
            else:
                _flush_run(runs, count)
                white = not white
                count = 1

    _flush_run(runs, count)

    return bytes(runs)


def main():
    parser = argparse.ArgumentParser(description="Convert video to RLE frames")
    parser.add_argument("video", help="Input video file")
    parser.add_argument("--fps", type=int, default=8, help="Output frame rate (default: 8)")
    parser.add_argument("--width", type=int, default=240, help="Frame width (default: 240)")
    parser.add_argument("--height", type=int, default=240, help="Frame height (default: 240)")
    parser.add_argument("--threshold", type=int, default=128, help="Binarization threshold (default: 128)")
    parser.add_argument("--start", type=float, default=0, help="Start time in seconds")
    parser.add_argument("--duration", type=float, default=220, help="Duration in seconds (default: 220)")
    args = parser.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("ERROR: Pillow not installed.  Run: pip install Pillow")
        sys.exit(1)

    w, h = args.width, args.height
    fps = args.fps

    # ---- Extract frames via ffmpeg (manual letterbox) ----
    # Fit 4:3 into 1:1 → width=240, height=180, pad 30px top+bottom
    fit_w = w
    fit_h = w * 3 // 4
    fit_x = 0
    fit_y = (h - fit_h) // 2
    print(f"Letterbox: draw={fit_w}x{fit_h}, pad_y={fit_y}")

    cmd = [
        "ffmpeg", "-ss", str(args.start), "-t", str(args.duration),
        "-i", args.video,
        "-vf", (f"fps={fps},scale={fit_w}:{fit_h}:flags=lanczos,"
                f"pad={w}:{h}:{fit_x}:{fit_y}:black,format=gray"),
        "-f", "rawvideo", "-pix_fmt", "gray",
        "-v", "quiet", "pipe:1"
    ]

    print(f"Extracting frames at {fps} fps, {w}x{h}...")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    frames_rle = []
    offsets = []
    total_bytes = 0
    frame_idx = 0
    frame_size = w * h

    while True:
        raw = proc.stdout.read(frame_size)
        if len(raw) < frame_size:
            break

        img = Image.frombytes("L", (w, h), raw)
        rle = rle_encode_frame(img, w, h)

        frames_rle.append(rle)
        offsets.append(total_bytes)
        total_bytes += len(rle)
        frame_idx += 1

        if frame_idx % 100 == 0:
            print(f"  Frame {frame_idx}: {len(rle)} bytes (avg {total_bytes/frame_idx:.0f})")

    proc.stdout.close()
    proc.wait()

    if frame_idx == 0:
        print("ERROR: No frames extracted. Check ffmpeg and video file.")
        sys.exit(1)

    # ---- Write output file ----
    out_path = os.path.join(os.path.dirname(__file__), "..", "data", "badapple.rle")
    out_path = os.path.abspath(out_path)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    with open(out_path, "wb") as f:
        # Header: frameCount (uint32)
        f.write(struct.pack("<I", frame_idx))
        # Offset table: uint32 per frame
        for off in offsets:
            f.write(struct.pack("<I", off))
        # Frame data
        for rle in frames_rle:
            f.write(rle)

    file_size = os.path.getsize(out_path)
    avg_rle = total_bytes / frame_idx
    compression = (1 - avg_rle / frame_size) * 100

    print(f"\nDone: {frame_idx} frames → {out_path}")
    print(f"  File size: {file_size / 1024:.0f} KB  ({file_size / 1024 / 1024:.2f} MB)")
    print(f"  Avg RLE:   {avg_rle:.0f} bytes/frame  ({compression:.1f}% compression)")
    print(f"\nNext: pio run -t uploadfs  (uploads to ESP32 SPIFFS)")


if __name__ == "__main__":
    main()
