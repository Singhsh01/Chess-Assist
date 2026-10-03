#!/usr/bin/env bash
# Records the scripted 15 s showcase (--demo) and builds the README media:
#   media/chess-assist-promo-4k.mp4   3840x2160 H.264
#   images/promo.gif                  1280x720 GIF for the README (< 10 MB)
#
# Linux (headless, software GL):  tools/record_promo.sh
# macOS (Retina):                 WINDOW=1920x1080 UI_SCALE=1 tools/record_promo.sh
#   (a 1920x1080 window on a Retina display has a 3840x2160 framebuffer)
#
# Needs ffmpeg (and gifsicle for the final GIF squeeze).
set -euo pipefail
cd "$(dirname "$0")/.."

FPS=${FPS:-24}
SECONDS_LEN=${SECONDS_LEN:-15}
WIDTH=3840
HEIGHT=2160
WINDOW=${WINDOW:-${WIDTH}x${HEIGHT}}
UI_SCALE=${UI_SCALE:-2}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkfifo "$WORK/frames"

ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s ${WIDTH}x${HEIGHT} -r "$FPS" \
  -i "$WORK/frames" -c:v libx264 -preset slow -crf 14 -pix_fmt yuv420p "$WORK/master.mp4" &
FF=$!

RUN=(./bin/chess-assist --size "$WINDOW" --ui-scale "$UI_SCALE" --demo --port 0
     --record "$WORK/frames" --fps "$FPS" --duration "$SECONDS_LEN" --movetime 1200)
if command -v xvfb-run >/dev/null && [ -z "${DISPLAY:-}" ]; then
  xvfb-run -a -s "-screen 0 $((WIDTH + 64))x$((HEIGHT + 64))x24" "${RUN[@]}"
else
  "${RUN[@]}"
fi
wait $FF

mkdir -p media images
ffmpeg -y -loglevel error -i "$WORK/master.mp4" -c:v libx264 -preset slow -crf 20 \
  -pix_fmt yuv420p -movflags +faststart media/chess-assist-promo-4k.mp4

# README GIF: 1280x720 @ 15 fps with a per-video palette.
ffmpeg -y -loglevel error -i "$WORK/master.mp4" -vf \
  "fps=15,scale=1280:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=192:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a:diff_mode=rectangle" \
  "$WORK/promo.gif"
if command -v gifsicle >/dev/null; then
  gifsicle -O3 --lossy=40 "$WORK/promo.gif" -o images/promo.gif
else
  cp "$WORK/promo.gif" images/promo.gif
fi
ls -lh media/chess-assist-promo-4k.mp4 images/promo.gif
