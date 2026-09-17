#!/bin/sh
# Development helper: regenerates the translated sources here (needs Python 3
# with iced-x86), copies the tree to a Haiku machine over SSH and builds
# there with the x86 (32-bit) toolchain.
# Usage: HAIKU_HOST=user@host tools/deploy.sh [sync-data]
#   sync-data  also copies the game files from original/ to the host
set -e
HOST=${HAIKU_HOST:?set HAIKU_HOST=user@host (the Haiku machine to build on)}
DIR=${HAIKU_DIR:-/boot/home/prehistorik}
cd "$(dirname "$0")/.."
python3 tools/translate.py original/historik.exe src/gen | head -2
if [ "$1" = "sync-data" ]; then
	COPYFILE_DISABLE=1 tar czf - --no-xattrs -C original historik.exe filesa.cur filesa.vga filesb.cur filesb.vga | ssh -o BatchMode=yes "$HOST" "mkdir -p $DIR/original && cd $DIR/original && tar xzf -" 2>&1 | grep -v -E 'WARNING|store now|openssh.com' || true
fi
COPYFILE_DISABLE=1 tar czf - --no-xattrs Makefile src resources tools/make_icon.py | ssh -o BatchMode=yes -o ConnectTimeout=20 "$HOST" "mkdir -p $DIR && cd $DIR && tar xzf - && touch resources/*.rdef && setarch x86 make -j4 GAME=original 2>&1 | grep -E 'error|Error|note:|icon' | head -40; ls -la build/Prehistorik" 2>&1 | grep -v -E 'WARNING|store now|openssh.com'
