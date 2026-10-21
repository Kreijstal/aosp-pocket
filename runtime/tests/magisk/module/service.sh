#!/system/bin/sh
set -eu
printf 'service uid=%s property=%s\n' "$(id -u)" "$(getprop pocket.magisk.fixture)" >> /data/local/tmp/pocket-magisk-fixture.txt
