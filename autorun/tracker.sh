#!/bin/sh
# MPC Sample tracker - SD card autorun. Runs as root at every power-on with the card in.
# Copies the static binary to /run (RAM, gone at power-off; nothing on the internal storage is
# touched), stops the stock sampler app cleanly, runs the tracker and restarts the sampler
# app when the tracker exits, however it exits. Songs and samples stay on the card in
# tracker/. Card out + power-cycle = stock.
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)       # .../autorun on the card (the path has a space on the device)
CARD=$(dirname -- "$D")
RUN=/run/tracker
LOG=/run/tracker.log

[ "$(id -u)" = 0 ] || exit 1
[ -s "$D/tracker" ] || exit 1
grep -q ' /run tmpfs ' /proc/mounts || exit 1

mkdir -p "$RUN"
# new inode, then rename: never overwrite a binary that a running copy still has mapped
cp "$D/tracker" "$RUN/tracker.new" && chmod 755 "$RUN/tracker.new" && mv -f "$RUN/tracker.new" "$RUN/tracker"
if pidof tracker >/dev/null 2>&1; then echo "tracker: already running" >> "$LOG"; exit 0; fi

# Whatever happens from here on, the sampler app comes back.
trap 'systemctl start ac50' EXIT
systemctl stop ac50
cd "$RUN"
echo "tracker: starting, card at $CARD" > "$LOG"
./tracker --card "$CARD" >> "$LOG" 2>&1 || echo "tracker: exit status $?" >> "$LOG"
