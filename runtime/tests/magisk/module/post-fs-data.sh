#!/system/bin/sh
set -eu
printf 'post-fs-data uid=%s version=%s\n' "$(id -u)" "$(magisk -V)" > /data/local/tmp/pocket-magisk-fixture.txt
resetprop -n pocket.magisk.fixture post-fs-data
