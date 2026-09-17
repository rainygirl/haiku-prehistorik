#!/bin/sh
# Launches the installed app repeatedly (sound muted) and quits it at
# different moments, through the application and through the window, then
# reports any new crash reports on the Desktop.
HOST=${HAIKU_HOST:?set HAIKU_HOST=user@host (the Haiku machine to build on)}
ssh -o BatchMode=yes "$HOST" '
APP=/boot/home/config/non-packaged/apps/Prehistorik/Prehistorik
before=$(ls /boot/home/Desktop/Prehistorik-*.report 2>/dev/null | wc -l)
for spec in "1 app" "3 window" "6 app" "10 window" "14 app" "20 window" "25 app" "32 window"; do
	set -- $spec
	(PREH_NOSOUND=1 PREH_WINDOW=950,330 PREH_KEYS=5e:9000,63:12000:4000 $APP >/tmp/qs.log 2>&1 &)
	sleep $1
	if [ "$2" = app ]; then hey Prehistorik quit >/dev/null 2>&1; else hey Prehistorik quit Window 0 >/dev/null 2>&1; fi
	n=0; while ps | grep -q "[c]onfig/non-packaged/apps/Prehistorik/Prehistorik" && [ $n -lt 20 ]; do sleep 0.5; n=$((n+1)); done
	alive=$(ps | grep -c "[c]onfig/non-packaged/apps/Prehistorik/Prehistorik")
	echo "quit via $2 after $1 s: still running=$alive, log: $(tail -1 /tmp/qs.log)"
	[ "$alive" != 0 ] && hey Prehistorik quit >/dev/null 2>&1
	sleep 2
done
after=$(ls /boot/home/Desktop/Prehistorik-*.report 2>/dev/null | wc -l)
echo "new crash reports: $((after - before))"
ls -la /boot/home/Desktop/Prehistorik-*.report 2>/dev/null
' 2>&1 | grep -v -E 'WARNING|store now|openssh'
