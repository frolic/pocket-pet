#!/bin/bash
# Grab one frame from the FaceTime HD camera. Warms up a few frames first so
# exposure settles, then keeps the last one. Output: $1 (default cam.jpg).
OUT="${1:-cam.jpg}"
ffmpeg -y -f avfoundation -framerate 15 -video_size 1280x720 -i "0" \
  -frames:v 8 -update 1 "$OUT" >/dev/null 2>&1
echo "$OUT"
