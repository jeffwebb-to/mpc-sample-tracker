#!/bin/sh
# Build the static aarch64 tracker binary with the emulator's cross toolchain and assemble
# the SD card package dist/tracker-autorun.zip. Needs Docker with the mpclab-tools image
# (built by `python3 lab.py build` in virtual-mpc-sample).
set -eu
cd "$(dirname "$0")"
mkdir -p build
docker run --rm -v "$PWD":/w -w /w mpclab-tools aarch64-linux-gnu-gcc -O2 -static -std=gnu11 \
    -Wall -Wextra -Wno-unused-result -Wno-format-truncation \
    -Isrc src/main.c src/intro.c src/ui.c src/hw.c src/player.c src/song.c -o build/tracker -lpthread -lm
ls -la build/tracker
rm -rf dist && mkdir -p dist/card/autorun dist/card/tracker/songs dist/card/tracker/samples
cp autorun/autorun.json autorun/tracker.sh build/tracker dist/card/autorun/
[ -f "READ ME.txt" ] && cp "READ ME.txt" dist/card/
# an optional demo song (see demo/README.txt): the tracker loads the first .mod in tracker/songs on a first run
for m in demo/*.mod; do [ -f "$m" ] && cp "$m" dist/card/tracker/songs/ && echo "demo song: $m" && break; done || true
python3 - <<'PY'
import hashlib
h = hashlib.sha256()
h.update(open("dist/card/autorun/autorun.json", "rb").read())
h.update(open("dist/card/autorun/tracker.sh", "rb").read())
h.update(b"f7bed1c9-7e90-4731-9194-c996666b4331")
open("dist/card/autorun/autorun.check", "w").write(h.hexdigest())
PY
(cd dist/card && rm -f ../tracker-autorun.zip && zip -qr ../tracker-autorun.zip .)
ls -la dist/tracker-autorun.zip
