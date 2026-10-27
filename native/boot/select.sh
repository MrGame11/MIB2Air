#!/bin/sh
# Called only for the normal CPU configuration, once per boot, with the
# release named by $base/current.
base=/tsd/var/wireless-aa
umask 077
release=$1
case "$release" in
????????????????) ;;
*) exit 0 ;;
esac
case "$release" in *[!0-9a-f]*) exit 0 ;; esac
dir=$base/releases/$release
[ -d "$base" ] && [ ! -L "$base" ] && [ -d "$base/run" ] && [ ! -L "$base/run" ] || exit 0
[ -d "$base/releases" ] && [ ! -L "$base/releases" ] && [ -d "$dir" ] && [ ! -L "$dir" ] || exit 0
[ -f "$base/current" ] && [ ! -L "$base/current" ] || exit 0
[ "$(cat "$base/current")" = "$release" ] || exit 0
[ -f "$base/mode" ] && [ ! -L "$base/mode" ] || exit 0
for item in cpu.conf run.sh wirelessadapter.waa sal.waa wirelessadapter.restore; do
	[ -f "$dir/$item" ] && [ ! -L "$dir/$item" ] || exit 0
done
mode=$(cat "$base/mode") || exit 0
case "$mode" in
normal) value=normal ;;
restore:*)
	token=${mode#restore:}
	[ ${#token} -eq 32 ] || exit 0
	case "$token" in *[!0-9a-f]*) exit 0 ;; esac
	[ -f "$base/run/restore-token" ] && [ ! -L "$base/run/restore-token" ] || exit 0
	[ "$(cat "$base/run/restore-token")" = "$token" ] || exit 0
	[ -f "$base/run/restore-result" ] && [ ! -L "$base/run/restore-result" ] && [ ! -s "$base/run/restore-result" ] || exit 0
	value=$mode
	;;
*) exit 0 ;;
esac
# QNX /dev/shmem supports flat files, not directories.
[ ! -e /dev/shmem/wireless-aa.boot ] || exit 0
(
	set -C
	echo selected >/dev/shmem/wireless-aa.boot
) || exit 0
# Never clear recovery proof or the durable installation journal.
rm -f "$base/run/wa2sal.fifo" "$base/run/sal2wa.fifo" "$base/run/wa.permit" "$base/run/sal.permit" || exit 0
if [ "$value" != normal ]; then
	[ ! -e "$base/mode.boot" ] && [ ! -L "$base/mode.boot" ] || exit 0
	(
		set -C
		echo "restoring:$token" >"$base/mode.boot"
	) && sync && mv "$base/mode.boot" "$base/mode" && sync || exit 0
fi
for role in wa sal; do
	[ "$value" = normal ] || [ "$role" = wa ] || continue
	(
		set -C
		echo "$value" >"$base/run/$role.permit"
	) || exit 0
done
sync || exit 0
echo "$dir/cpu.conf"
