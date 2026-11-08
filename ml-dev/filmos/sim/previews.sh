#!/usr/bin/env bash
# Render the preview images in ../preview from the scripted scenes.
#   make && ./previews.sh
#   PHOTOS=dir ./previews.sh     the darkroom screens show your own photos (mkcard.sh says how)
#   LIVEVIEW=pic.ppm             and the HUD is drawn over this picture (720 x 480) for LiveView
set -euo pipefail
cd "$(dirname "$0")"
OUT=../preview
mkdir -p "$OUT" out/frames

run() { rm -rf "out/frames/$1"; ./out/sim "scenes/$1.txt" "out/frames/$1" 2>/dev/null; }
# the frame on screen at <ms>: the last one presented at or before it
at() { ls out/frames/$1/*.idx | awk -F_ -v t="$2" '{ split($3, a, "."); if (a[1] + 0 <= t) f = $0 } END { print f }'; }
still() { python3 topng.py "$(at "$1" "$2")" >/dev/null; cp "$(at "$1" "$2" | sed 's/idx$/png/')" "$OUT/$3.png"; }
last() { local f; f=$(ls out/frames/$1/*.idx | tail -1); python3 topng.py "$f" >/dev/null; cp "${f%.idx}.png" "$OUT/$2.png"; }

# the shelf's samples on the card, as the EOS M build makes them (PLAY on the shelf)
make -s out/devtest
mkdir -p out/card/ML/SAMPLES
FOS_LUT_DIR=out/lut ./out/devtest -S ../../films/sample/SAMPLE.CR2 out/card/ML/SAMPLES

run shelf;  still shelf 660 shelf; still shelf 1110 shelf-gliding
run sample; still sample 1600 sample
run load;   still load 1300 boot; still load 5300 loaded; still load 5800 shutter
run home;   still home 1250 home
run roll;   still roll 2400 roll; still roll 3000 rewind
run pages;  still pages 1330 viewfinder; still pages 6190 lens; last pages system
run screen; last screen screen          # its palette has the warmth it ends with

# no rolls (roll limit Off, the camera's default): the screens that change
run free_shelf;    still free_shelf 660 free-shelf; still free_shelf 4600 free-ready
run free_home;     still free_home 1500 free-home
run free_roll;     still free_roll 2800 free-frames
run free_settings; still free_settings 2200 free-system; still free_settings 3300 free-system-36

# the darkroom, on a card of test rolls (mkcard.sh): the list, developing, the prints
./mkcard.sh >/dev/null
run endroll; last endroll end-of-roll
run darkroom; still darkroom 2900 darkroom; still darkroom 12400 developing; still darkroom 22500 developing-line
run prints; still prints 1300 prints-sheet; still prints 3500 prints-frame
run free_darkroom; still free_darkroom 2800 free-darkroom

# the HUD over a made-up LiveView picture
gcc -O2 -std=gnu99 -o out/hudtest hudtest.c fonts.c ../fos_gfx.c ../fos_film.c ../fos_hud.c -lm
./out/hudtest 1 ${LIVEVIEW:-} && python3 -c "from PIL import Image; Image.open('out/hud.ppm').save('$OUT/hud.png')"
# and the top plate a DSLR shows instead, shooting through the viewfinder
./out/hudtest p && python3 -c "from PIL import Image; Image.open('out/hud.ppm').save('$OUT/plate.png')"
# the same with no rolls: a count of 143, no "/36", no frames-left bar
./out/hudtest 1f ${LIVEVIEW:-} && python3 -c "from PIL import Image; Image.open('out/hud.ppm').save('$OUT/hud-free.png')"
./out/hudtest pf && python3 -c "from PIL import Image; Image.open('out/hud.ppm').save('$OUT/plate-free.png')"

# browsing the shelf, half size
run browse
python3 - <<'PY'
from pathlib import Path
from PIL import Image
import numpy as np
pal = np.fromfile("out/pal.bin", dtype="<u4")
rgb = np.stack([(pal >> 16) & 255, (pal >> 8) & 255, pal & 255], axis=1).astype(np.uint8)
frames = sorted(Path("out/frames/browse").glob("*.idx"))
imgs, times = [], []
for f in frames:
    idx = np.fromfile(f, dtype=np.uint8).reshape(480, 720)
    imgs.append(Image.fromarray(rgb[idx], "RGB").resize((360, 240), Image.LANCZOS))
    times.append(int(f.stem.split("_")[-1]))
durs = [max(20, b - a) for a, b in zip(times, times[1:])] + [600]
pal_img = imgs[len(imgs) // 2].quantize(colors=255, method=Image.MEDIANCUT)
q = [im.quantize(palette=pal_img, dither=Image.NONE) for im in imgs]
q[0].save("../preview/shelf.gif", save_all=True, append_images=q[1:], duration=durs, loop=0)
print(len(q), "frames")
PY
ls -la "$OUT"
