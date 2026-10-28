#!/bin/sh
# Run the interactive installer from any working directory.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
command -v docker >/dev/null 2>&1 || {
    echo 'Docker is required. Install and start Docker, then run this command again.' >&2
    exit 1
}
docker info >/dev/null 2>&1 || {
    echo 'Docker is not running or is not accessible. Start Docker, then try again.' >&2
    exit 1
}
image=mib-std2-wirelessbridge-waa
umask 077
mkdir -p local
chmod 700 local
log=local/setup-build.log
if [ -t 1 ]; then
    bold=$(printf '\033[1m') dim=$(printf '\033[2m') off=$(printf '\033[0m')
    accent=$(printf '\033[36m') ok=$(printf '\033[32m') bad=$(printf '\033[31m')
else
    bold='' dim='' off='' accent='' ok='' bad=''
fi
elapsed() {
    if [ "$1" -lt 60 ]; then printf '%ss' "$1"; else printf '%sm %02ds' $(($1 / 60)) $(($1 % 60)); fi
}
printf '\n %swirelessbridge%s %s›%s setup\n\n' "$bold" "$off" "$dim" "$off"
# The image holds only the toolchain, so it is rebuilt only when the Dockerfile
# changes. Skipping the build entirely lets setup run without internet access.
deps=$(cksum < Dockerfile | awk '{print $1 "-" $2}')
built=$(docker image inspect -f '{{index .Config.Labels "org.wirelessbridge.deps"}}' "$image" 2>/dev/null || :)
if [ -n "$built" ] && [ "$built" = "$deps" ]; then
    printf ' %s✓%s %-26s%s%-31s%s\n' "$ok" "$off" 'Setup image ready' "$dim" 'already prepared' "$off"
else
    docker build --target runtime --build-arg DEPS="$deps" -t "$image" . >"$log" 2>&1 &
    build_pid=$!
    trap 'kill "$build_pid" 2>/dev/null || :; wait "$build_pid" 2>/dev/null || :; printf "\n"; exit 130' INT TERM
    [ -t 1 ] || printf ' Preparing setup image (log: %s)\n' "$log"
    ticks=0
    while kill -0 "$build_pid" 2>/dev/null; do
        if [ -t 1 ]; then
            case $((ticks % 10)) in
                0) frame='⠋' ;; 1) frame='⠙' ;; 2) frame='⠹' ;; 3) frame='⠸' ;; 4) frame='⠼' ;;
                5) frame='⠴' ;; 6) frame='⠦' ;; 7) frame='⠧' ;; 8) frame='⠇' ;; *) frame='⠏' ;;
            esac
            seconds=$((ticks / 5))
            printf '\r %s%s%s %s%-26s%s%s%-31s%s %s%s:%02d%s' "$accent" "$frame" "$off" \
                "$bold" 'Preparing setup image' "$off" "$dim" 'first launch takes a few min' "$off" \
                "$dim" $((seconds / 60)) $((seconds % 60)) "$off"
        fi
        sleep 0.2
        ticks=$((ticks + 1))
    done
    [ -t 1 ] && printf '\r\033[K'
    if wait "$build_pid"; then
        printf ' %s✓%s %-26s%s%-31s %8s%s\n' "$ok" "$off" 'Setup image ready' "$dim" '' "$(elapsed $((ticks / 5)))" "$off"
    else
        printf ' %s✗%s %sCould not prepare setup image.%s\n' "$bad" "$off" "$bold" "$off" >&2
        printf '   %sIt is downloaded once and needs internet access; afterwards setup works offline.%s\n' "$dim" "$off" >&2
        printf '   %sLast lines of %s:%s\n\n' "$dim" "$log" "$off" >&2
        tail -n 15 "$log" >&2
        exit 1
    fi
    trap - INT TERM
fi
# Read-only container and code; only local/ persists. A TTY is requested only when there is one.
tty=
[ -t 0 ] && [ -t 1 ] && tty=-t
# Recorded on the MIB at install/update so later runs can show what is installed.
version=$(git describe --tags --always --dirty 2>/dev/null || :)
# Default MIB address when the computer is on the MIB access point: its gateway.
gateway=$(route -n get default 2>/dev/null | awk '/gateway:/ {print $2}' || :)
[ -n "$gateway" ] || gateway=$(ip route show default 2>/dev/null | awk '{print $3; exit}' || :)
# The MIB connects back to these ports for fast active-mode FTP downloads.
exec docker run --rm -i $tty --read-only --tmpfs /tmp -v "$PWD/local:/app/local" \
    -v "$PWD/installer:/app/installer:ro" -v "$PWD/native:/app/native:ro" \
    -v "$PWD/profiles:/app/profiles:ro" \
    -v "$PWD/THIRD_PARTY_NOTICES.md:/app/THIRD_PARTY_NOTICES.md:ro" \
    -p 41000-41031:41000-41031 -e WAA_VERSION="$version" -e WAA_GATEWAY="$gateway" \
    "$image" "$@"
