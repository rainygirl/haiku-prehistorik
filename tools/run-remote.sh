#!/bin/sh
# Runs the port on the Haiku box for N seconds, collects the log and a screenshot.
# Usage: tools/run-remote.sh [seconds] [env assignments]
SECS=${1:-10}
HOST=${HAIKU_HOST:?set HAIKU_HOST=user@host (the Haiku machine to build on)}
DIR=${HAIKU_DIR:-/boot/home/prehistorik}
cd "$(dirname "$0")/.."
ssh -o BatchMode=yes -o ConnectTimeout=20 "$HOST" "cd $DIR && kill \$(ps | grep '[P]rehistorik' | awk '{print \$(NF-3)}') 2>/dev/null; sleep 1; rm -f /tmp/preh.log; (PREH_TRACE=1 $2 ./build/Prehistorik $DIR/original/historik.exe > /tmp/preh.log 2>&1 &); sleep $SECS; screenshot -s /tmp/preh.png >/dev/null 2>&1; echo '--- log:'; head -c 6000 /tmp/preh.log; echo; echo '--- tail:'; tail -20 /tmp/preh.log; echo '--- running:'; ps | grep -c '[P]rehistorik'" 2>&1 | grep -v -E 'WARNING|store now|openssh.com'
scp -o BatchMode=yes -q "$HOST":/tmp/preh.png build/remote-shot.png 2>/dev/null && echo "screenshot: build/remote-shot.png"
