#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
[[ $EUID -eq 0 ]] || { echo 'Run this script with sudo.' >&2; exit 1; }
case ${1:-} in install|uninstall) ;; *) echo 'Usage: service.sh install|uninstall' >&2; exit 1 ;; esac
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
label=com.edimax.ax88179.driver
plist=/Library/LaunchDaemons/$label.plist
binary=/usr/local/bin/edimax_driver
helper="$repo_dir/edimax_driver"
[[ -x "$helper" ]] || { echo 'Driver missing. Re-extract the binary release or build the source with make.' >&2; exit 1; }
if [[ ! -f "$repo_dir/edimax_driver.c" ]]; then
    (cd "$repo_dir" && /usr/bin/shasum -a 256 -c SHA256SUMS)
fi
if [[ $1 == install ]]; then
    [[ -f "$repo_dir/services/$label.plist" ]]
    plutil -lint "$repo_dir/services/$label.plist"
fi
# Check that macOS can execute this build before stopping a working installation.
"$helper" --version
if [[ $1 == install ]]; then
    mkdir -p /usr/local/bin
    staged=$(mktemp /usr/local/bin/.edimax_driver.XXXXXX)
    trap 'rm -f "$staged"' EXIT
    cp "$helper" "$staged"
    chown root:wheel "$staged"
    chmod 755 "$staged"
fi
# bootout may escalate to SIGKILL; request a clean exit before unloading the job.
if launchctl print "system/$label" >/dev/null 2>&1 && pgrep -u 0 -x edimax_driver >/dev/null; then
    launchctl kill SIGTERM "system/$label" || true
fi
if pgrep -u 0 -x edimax_driver >/dev/null; then
    pkill -TERM -u 0 -x edimax_driver || true
fi
for attempt in {1..15}; do
    pgrep -u 0 -x edimax_driver >/dev/null || break
    sleep 1
done
if pgrep -u 0 -x edimax_driver >/dev/null; then
    echo 'Driver did not stop; installation was not changed.' >&2
    exit 1
fi
if launchctl print "system/$label" >/dev/null 2>&1; then
    launchctl bootout "system/$label"
fi
if [[ $1 == uninstall ]]; then
    "$helper" --disable-network
    rm -f "$plist" "$binary"
    echo 'Driver removed. Network service settings are preserved and disabled.'
    exit 0
fi
mv -f "$staged" "$binary"
"$binary" --configure-network
cp "$repo_dir/services/$label.plist" "$plist"
chown root:wheel "$plist"
chmod 644 "$plist"
launchctl enable "system/$label"
launchctl bootstrap system "$plist"
echo 'Driver installed. Check LAN connectivity and /var/log/edimax_driver.log.'
