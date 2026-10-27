#!/bin/sh
# A supervisor retry uses the original service after the first permit is consumed.
base=/tsd/var/wireless-aa
# The release is fixed by the cpu.conf path chosen for this boot.
dir=${0%/run.sh}
case "$dir" in
"$base"/releases/????????????????) ;;
*) dir= ;;
esac
case "${dir#"$base"/releases/}" in *[!0-9a-f]*) dir= ;; esac
role=$1
shift
case "$role" in
wa)
	stock=/tsd/bin/wirelessadapter/tsd.mibstd2.system.wirelessadapter.main
	binary=wirelessadapter.waa
	;;
sal)
	stock=/tsd/bin/sal/tsd.mibstd2.sal.main
	binary=sal.waa
	;;
*) exit 1 ;;
esac
permit=$base/run/$role.permit
if [ -f "$permit" ] && [ ! -L "$permit" ] && [ -f "$base/mode" ] && [ ! -L "$base/mode" ]; then
	value=$(cat "$permit")
	mode=$(cat "$base/mode")
	allowed=no
	case "$value" in
	normal) [ "$mode" = normal ] && allowed=yes ;;
	restore:*)
		token=${value#restore:}
		if [ "$role" = wa ] && [ "$mode" = "restoring:$token" ] && [ ${#token} -eq 32 ]; then
			case "$token" in *[!0-9a-f]*) ;; *)
				binary=wirelessadapter.restore
				allowed=yes
				;;
			esac
		fi
		;;
	esac
	if [ "$allowed" = yes ] && [ -n "$dir" ] && [ -x "$dir/$binary" ] && [ ! -L "$dir/$binary" ]; then
		if rm "$permit" && sync && [ ! -e "$permit" ]; then
			exec "$dir/$binary" "$@"
		fi
	fi
fi
exec "$stock" "$@"
