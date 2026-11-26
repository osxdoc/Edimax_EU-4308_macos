#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 077
[[ $EUID -eq 0 ]] || { echo 'Run this script with sudo.' >&2; exit 1; }
case ${1:-} in install|uninstall) ;; *) echo 'Usage: service.sh install|uninstall' >&2; exit 1 ;; esac
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
label=com.edimax.ax88179.driver
plist=/Library/LaunchDaemons/$label.plist
directory=/Library/PrivilegedHelperTools
binary=$directory/edimax_driver
legacy=/usr/local/bin/edimax_driver
helper="$repo_dir/edimax_driver"
action=$1

secure_directory() {
    local attributes owner mode
    [[ -d $1 && ! -L $1 ]] || { echo "Unsafe directory: $1" >&2; return 1; }
    attributes=$(stat -f '%u:%Lp' "$1")
    owner=${attributes%%:*}; mode=${attributes#*:}
    [[ $owner == 0 && $((8#$mode & 022)) == 0 ]] || { echo "Directory must be root-owned and not writable by other users: $1" >&2; return 1; }
}
secure_directory /
secure_directory /Library
if [[ ! -e $directory && ! -L $directory ]]; then mkdir -m 755 "$directory"; fi
secure_directory "$directory"
secure_directory /Library/LaunchDaemons
if [[ ${2:-} != --locked ]]; then
    # Keep one lock inode across invocations; lockf releases the lock even after a crash.
    exec lockf -k -t 0 "$directory/.edimax-service.lock" /bin/bash "$0" "$1" --locked
fi
transaction=$(mktemp -d "$directory/.edimax-install.XXXXXX")
staged_helper="$transaction/helper"
old_loaded=0
rollback=0
using_installed=0

stop_driver() {
    if launchctl print "system/$label" >/dev/null 2>&1 && pgrep -u 0 -x edimax_driver >/dev/null; then
        launchctl kill SIGTERM "system/$label" || true
    fi
    if pgrep -u 0 -x edimax_driver >/dev/null; then pkill -TERM -u 0 -x edimax_driver || true; fi
    for attempt in {1..15}; do
        pgrep -u 0 -x edimax_driver >/dev/null || break
        sleep 1
    done
    if pgrep -u 0 -x edimax_driver >/dev/null; then
        echo 'Driver did not stop; installed files were not changed.' >&2
        return 1
    fi
}

unload_job() {
    if launchctl print "system/$label" >/dev/null 2>&1; then launchctl bootout "system/$label"; fi
}

finish() {
    local status=$? restored=1
    trap - EXIT
    set +e
    if [[ $status != 0 && $rollback == 1 ]]; then
        echo 'Operation failed; restoring the previous installation.' >&2
        if stop_driver && unload_job && "$staged_helper" --cleanup-interfaces; then
            if [[ -f $transaction/old-binary ]]; then
                mv -f "$transaction/old-binary" "$binary" || restored=0
            else
                rm -f "$binary" || restored=0
            fi
            if [[ -f $transaction/old-plist ]]; then
                cp -p "$transaction/old-plist" "$plist" || restored=0
            else
                rm -f "$plist" || restored=0
            fi
            if [[ $old_loaded == 1 && $restored == 1 ]]; then
                if [[ $action == uninstall ]]; then "$staged_helper" --configure-network || restored=0; fi
                launchctl enable "system/$label" && launchctl bootstrap system "$plist" || restored=0
            fi
        else
            restored=0
        fi
        if [[ $restored == 0 ]]; then
            echo "Automatic recovery failed; retained backups in $transaction" >&2
            exit "$status"
        fi
    fi
    rm -rf "$transaction"
    exit "$status"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Stage inputs into root-owned storage before checksums, execution, or stopping the active service.
if [[ -f $helper && ! -L $helper && -x $helper ]]; then
    cp "$helper" "$staged_helper"
    chmod 700 "$staged_helper"
elif [[ $action == uninstall && -f $binary && ! -L $binary && -x $binary ]]; then
    cp "$binary" "$staged_helper"
    chmod 700 "$staged_helper"
    using_installed=1
else
    echo 'Driver missing. Re-extract the binary release or build the source with make.' >&2
    exit 1
fi
if [[ $action == install ]]; then
    [[ -f "$repo_dir/services/$label.plist" && ! -L "$repo_dir/services/$label.plist" ]] || { echo 'Service plist missing.' >&2; exit 1; }
    cp "$repo_dir/services/$label.plist" "$transaction/new-plist"
fi
if [[ $using_installed == 0 && ( -f "$repo_dir/SHA256SUMS" || ! -f "$repo_dir/edimax_driver.c" ) ]]; then
    [[ -f "$repo_dir/SHA256SUMS" && ! -L "$repo_dir/SHA256SUMS" ]] || { echo 'SHA256SUMS missing.' >&2; exit 1; }
    cp "$repo_dir/SHA256SUMS" "$transaction/SHA256SUMS"
    (cd "$repo_dir" && shasum -a 256 -c "$transaction/SHA256SUMS")
    [[ $(shasum -a 256 "$staged_helper" | awk '{print $1}') == $(awk '$2 == "edimax_driver" {print $1}' "$transaction/SHA256SUMS") ]]
    if [[ $action == install ]]; then
        [[ $(shasum -a 256 "$transaction/new-plist" | awk '{print $1}') == $(awk -v f="services/$label.plist" '$2 == f {print $1}' "$transaction/SHA256SUMS") ]]
    fi
fi
if [[ $action == install ]]; then
    plutil -lint "$transaction/new-plist"
    [[ $(plutil -extract ProgramArguments.0 raw -o - "$transaction/new-plist") == "$binary" ]]
fi
# Check the staged helper before stopping an existing installation.
"$staged_helper" --version

old_binary=$binary
if [[ -f $plist ]]; then
    plutil -lint "$plist"
    old_binary=$(plutil -extract ProgramArguments.0 raw -o - "$plist")
    [[ $old_binary == "$binary" || $old_binary == "$legacy" ]] || { echo 'Unexpected previous executable path.' >&2; exit 1; }
    cp -p "$plist" "$transaction/old-plist"
    # Rollback of a legacy install must also use the protected executable location.
    if [[ $old_binary == "$legacy" ]]; then
        plutil -replace ProgramArguments.0 -string "$binary" "$transaction/old-plist"
        chmod 644 "$transaction/old-plist"
    fi
fi
if [[ -e $old_binary || -L $old_binary ]]; then
    [[ -f $old_binary && ! -L $old_binary ]] || { echo 'Unsafe previous executable.' >&2; exit 1; }
    cp "$old_binary" "$transaction/old-binary"
    chown root:wheel "$transaction/old-binary"
    chmod 755 "$transaction/old-binary"
fi
if launchctl print "system/$label" >/dev/null 2>&1; then
    old_loaded=1
    [[ -f $transaction/old-binary && -f $transaction/old-plist ]] || { echo 'Cannot back up the loaded installation.' >&2; exit 1; }
fi
if [[ $action == install ]]; then
    cp "$staged_helper" "$transaction/new-binary"
    chown root:wheel "$transaction/new-binary" "$transaction/new-plist"
    chmod 755 "$transaction/new-binary"
    chmod 644 "$transaction/new-plist"
fi
# A hung old process must retain its existing job and files.
if ! stop_driver; then exit 1; fi
rollback=1
unload_job
"$staged_helper" --cleanup-interfaces
if [[ $action == uninstall ]]; then
    "$staged_helper" --disable-network
    rm -f "$plist" "$binary" "$legacy"
    rollback=0
    echo 'Driver removed. Network service settings are preserved and disabled.'
    exit 0
fi
mv -f "$transaction/new-binary" "$binary"
"$binary" --configure-network
mv -f "$transaction/new-plist" "$plist"
launchctl enable "system/$label"
launchctl bootstrap system "$plist"
for attempt in {1..5}; do
    sleep 1
    if pgrep -u 0 -x edimax_driver >/dev/null; then break; fi
done
pgrep -u 0 -x edimax_driver >/dev/null || { echo 'Installed driver did not stay running.' >&2; exit 1; }
rm -f "$legacy"
rollback=0
echo 'Driver installed. Check LAN connectivity and /var/log/edimax_driver.log.'
