#!/bin/sh
# Installs the built port on the Haiku box: binary, game data and the default
# configuration (VGA, Sound Blaster, keyboard) into
# ~/config/non-packaged/apps/Prehistorik, plus Deskbar and Desktop links.
# Usage: tools/install.sh   (run from the Mac; builds must already exist on the host)
set -e
HOST=${HAIKU_HOST:?set HAIKU_HOST=user@host (the Haiku machine to build on)}
DIR=${HAIKU_DIR:-/boot/home/prehistorik}
cd "$(dirname "$0")/.."
COPYFILE_DISABLE=1 tar czf - --no-xattrs -C resources grawaga.cfg | ssh -o BatchMode=yes "$HOST" "cat > /tmp/preh-cfg.tgz" 2>&1 | grep -v -E 'WARNING|store now|openssh.com' || true
ssh -o BatchMode=yes "$HOST" "
set -e
APP=/boot/home/config/non-packaged/apps/Prehistorik
# A running instance must stop first: overwriting the file it is paged from
# corrupts the running code and crashes it.
hey Prehistorik quit >/dev/null 2>&1 || true
sleep 1
mkdir -p \$APP
cp $DIR/build/Prehistorik \$APP/Prehistorik
for f in historik.exe filesa.cur filesa.vga filesb.cur filesb.vga; do cp $DIR/original/\$f \$APP/\$f; done
tar xzf /tmp/preh-cfg.tgz -C \$APP && rm -f /tmp/preh-cfg.tgz
chmod 666 \$APP/grawaga.cfg   # the game rewrites it when the setup screens run
mimeset -f \$APP/Prehistorik
# mimeset types our binary as application/octet-stream, which makes Tracker
# draw a generic icon for it and for links to it.
addattr -t mime BEOS:TYPE application/x-vnd.be-elfexecutable \$APP/Prehistorik
addattr -t string SYS:NAME Prehistorik \$APP/Prehistorik
mkdir -p /boot/home/config/settings/deskbar/menu/Applications
# Keep links that already point here: replacing one makes Tracker drop the
# Desktop icon until it restarts.
mkdir -p /boot/home/config/non-packaged/bin
for l in /boot/home/config/settings/deskbar/menu/Applications/Prehistorik /boot/home/Desktop/Prehistorik /boot/home/config/non-packaged/bin/prehistorik; do
	[ \"\$(readlink \"\$l\")\" = \"\$APP/Prehistorik\" ] || ln -sf \$APP/Prehistorik \"\$l\"
done
# The icon Tracker shows for the application comes from the MIME database
# entry for its signature, which the registrar writes the first time the app
# runs and never refreshes. Drop it and register again, after the links exist:
# Tracker redraws their icons when that entry changes.
rm -f /boot/home/config/settings/mime_db/application/x-vnd.rainygirl-prehistorik
sleep 1
\$APP/Prehistorik --register
sleep 2
# Tracker caches the icon it resolved for an application signature and keeps
# using it for links until it restarts, so a newly installed icon only shows
# up after this. It closes Tracker's own windows and nothing else.
hey Tracker quit >/dev/null 2>&1
sleep 2
(/boot/system/Tracker >/dev/null 2>&1 &)
sleep 3
ls -la \$APP
" 2>&1 | grep -v -E 'WARNING|store now|openssh.com'
