#!/bin/sh
# Dev loop against the virtual-mpc-sample emulator: copy the binary to /run/tracker in the
# guest, stop the stock app, run the tracker, restart the stock app when it exits.
#   ./lab-run.sh          start (detached in the guest); the "card" is /run/tracker/card
#   ./lab-run.sh --log    show the tracker's log
#   ./lab-run.sh --stop   quit the tracker (SIGTERM, it autosaves) and let the stock app return
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
LAB=${LAB_DIR:-$HERE/../virtual-mpc-sample}
OPTS="-i $LAB/out/keys/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
SSH="ssh $OPTS -p 2022 root@127.0.0.1"
case "${1:-}" in
  --log)  exec $SSH "tail -n 40 /run/tracker/tracker.log" ;;
  --stop) exec $SSH "killall -TERM tracker || true" ;;
esac
$SSH "mkdir -p /run/tracker/card; if pidof tracker >/dev/null; then killall -TERM tracker; sleep 2; fi"
scp $OPTS -P 2022 "$HERE/build/tracker" root@127.0.0.1:/run/tracker/tracker.new
$SSH "mv -f /run/tracker/tracker.new /run/tracker/tracker && chmod +x /run/tracker/tracker"
$SSH "nohup sh -c 'trap \"systemctl start ac50\" EXIT; systemctl stop ac50; cd /run/tracker && TRACKER_PERIOD=${TRACKER_PERIOD:-} ./tracker --card /run/tracker/card ${TRACKER_FLAGS:-}' > /run/tracker/tracker.log 2>&1 < /dev/null &"
echo "tracker started in the guest; ./lab-run.sh --log for output, ./lab-run.sh --stop to quit"
