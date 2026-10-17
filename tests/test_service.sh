#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/edimax-service-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
export TEST_ROOT="$test_root"
mkdir -p "$test_root/mock" "$test_root/repo with spaces/tools" "$test_root/repo with spaces/services" \
    "$test_root/Library/LaunchDaemons" "$test_root/Library/PrivilegedHelperTools" "$test_root/local/bin"
chmod 755 "$test_root/Library" "$test_root/Library/LaunchDaemons" "$test_root/Library/PrivilegedHelperTools"
# Replace privileges and system paths; retain real lockf and permission-mode checks.
sed -e '/^\[\[ \$EUID -eq 0 \]\]/d' \
    -e 's|^export PATH=.*|export PATH="$TEST_ROOT/mock:/usr/bin:/bin:/usr/sbin:/sbin"|' \
    -e 's|/Library|$TEST_ROOT/Library|g' -e 's|/usr/local/bin|$TEST_ROOT/local/bin|g' \
    "$repo_dir/tools/service.sh" > "$test_root/repo with spaces/tools/service.sh"
sed "s|/Library|$TEST_ROOT/Library|g" "$repo_dir/services/com.edimax.ax88179.driver.plist" > "$test_root/template.plist"
cp "$test_root/template.plist" "$test_root/repo with spaces/services/"
mv "$test_root/repo with spaces/services/template.plist" "$test_root/repo with spaces/services/com.edimax.ax88179.driver.plist"
touch "$test_root/repo with spaces/edimax_driver.c"
cat > "$test_root/mock/command" <<'MOCK'
#!/bin/bash
set -eu
name=${0##*/}
if [[ $name == stat ]]; then
    [[ $1 == -f && $2 == '%u:%Lp' ]]
    owner=0
    [[ ${BAD_OWNER:-0} == 0 ]] || owner=501
    printf '%s:%s\n' "$owner" "$(/usr/bin/stat -f '%Lp' "$3")"
    exit
fi
printf '%s %s\n' "$name" "$*" >> "$TEST_ROOT/events"
fail_once() {
    if [[ ${FAIL_AT:-} == "$1" && ! -f $TEST_ROOT/failed ]]; then touch "$TEST_ROOT/failed"; exit 7; fi
}
case "$name" in
    launchctl)
        fail_once "$1"
        case "$1" in
            print) [[ -f "$TEST_ROOT/loaded" ]] ;;
            kill) [[ ${STUCK:-0} == 1 ]] || rm -f "$TEST_ROOT/running" ;;
            bootout) [[ ! -f "$TEST_ROOT/running" ]]; rm -f "$TEST_ROOT/loaded" ;;
            bootstrap)
                path=$(/usr/bin/plutil -extract ProgramArguments.0 raw -o - "$3")
                [[ -x $path ]]
                touch "$TEST_ROOT/loaded"
                if [[ ${FAIL_AT:-} == no-process && ! -f $TEST_ROOT/failed ]]; then touch "$TEST_ROOT/failed"; else touch "$TEST_ROOT/running"; fi ;;
            enable) : ;;
            *) exit 1 ;;
        esac ;;
    pgrep) [[ -f "$TEST_ROOT/running" ]] ;;
    pkill) [[ ${STUCK:-0} == 1 ]] || rm -f "$TEST_ROOT/running" ;;
    sleep|chown) : ;;
    *) exit 1 ;;
esac
MOCK
chmod +x "$test_root/mock/command"
for command in launchctl pgrep pkill sleep chown stat; do ln -s command "$test_root/mock/$command"; done
cat > "$test_root/repo with spaces/edimax_driver" <<'HELPER'
#!/bin/bash
set -eu
printf 'helper %s\n' "$*" >> "$TEST_ROOT/events"
if [[ ${FAIL_AT:-} == "$1" && ! -f $TEST_ROOT/failed ]]; then touch "$TEST_ROOT/failed"; exit 7; fi
case "$1" in
    --version) [[ ${BAD_VERSION:-0} == 0 ]] ;;
    --cleanup-interfaces|--disable-network) ;;
    --configure-network)
        if [[ ${HOLD_CONFIG:-0} == 1 ]]; then
            touch "$TEST_ROOT/waiting"
            for retry in {1..500}; do [[ ! -f $TEST_ROOT/resume ]] || exit 0; /bin/sleep 0.01; done
            exit 8
        fi ;;
    *) exit 1 ;;
esac
HELPER
chmod +x "$test_root/repo with spaces/edimax_driver"
service="$test_root/repo with spaces/tools/service.sh"
binary="$test_root/Library/PrivilegedHelperTools/edimax_driver"
legacy="$test_root/local/bin/edimax_driver"
plist="$test_root/Library/LaunchDaemons/com.edimax.ax88179.driver.plist"
source_plist="$test_root/repo with spaces/services/com.edimax.ax88179.driver.plist"
previous_install() {
    printf '#!/bin/bash\n# previous working driver\nexit 0\n' > "$binary"
    chmod 755 "$binary"
    cp "$test_root/template.plist" "$plist"
    touch "$test_root/loaded" "$test_root/running"
    rm -f "$test_root/failed"
    : > "$test_root/events"
}

# Reject unsafe directory ownership, modes, and symlinks before any helper or service mutation.
if BAD_OWNER=1 bash "$service" install > "$test_root/output" 2>&1; then exit 1; fi
[[ ! -e $test_root/events ]]
chmod 777 "$test_root/Library/PrivilegedHelperTools"
if bash "$service" install > "$test_root/output" 2>&1; then exit 1; fi
chmod 755 "$test_root/Library/PrivilegedHelperTools"
mv "$test_root/Library/PrivilegedHelperTools" "$test_root/safe-tools"
ln -s "$test_root/safe-tools" "$test_root/Library/PrivilegedHelperTools"
if bash "$service" install > "$test_root/output" 2>&1; then exit 1; fi
rm "$test_root/Library/PrivilegedHelperTools"
mv "$test_root/safe-tools" "$test_root/Library/PrivilegedHelperTools"
[[ ! -e $test_root/events ]]

printf 'invalid plist\n' > "$source_plist"
if bash "$service" install > "$test_root/output" 2>&1; then exit 1; fi
[[ ! -e $test_root/events ]]
cp "$test_root/template.plist" "$source_plist"
previous_install
if STUCK=1 bash "$service" install > "$test_root/output" 2>&1; then exit 1; fi
grep -q 'Driver did not stop' "$test_root/output"
grep -q 'previous working driver' "$binary"
[[ -f $test_root/loaded ]]
! grep -q '^launchctl bootout' "$test_root/events"
: > "$test_root/events"
if BAD_VERSION=1 bash "$service" install > "$test_root/output" 2>&1; then exit 1; fi
[[ -f $test_root/running ]]
! grep -q '^launchctl ' "$test_root/events"

# Faults after stopping must restore the files and the previous loaded job.
for failure in bootout --cleanup-interfaces --configure-network enable bootstrap no-process; do
    previous_install
    if FAIL_AT="$failure" bash "$service" install > "$test_root/output" 2>&1; then echo "Accepted $failure failure"; exit 1; fi
    grep -q 'previous working driver' "$binary"
    cmp "$plist" "$test_root/template.plist"
    [[ -f $test_root/loaded && -f $test_root/running ]]
    grep -q 'restoring the previous installation' "$test_root/output"
done
previous_install
if FAIL_AT=--disable-network bash "$service" uninstall > "$test_root/output" 2>&1; then exit 1; fi
[[ -x $binary && -f $test_root/running ]]

# Migration and rollback never execute the old binary from the writable legacy directory.
previous_install
mv "$binary" "$legacy"
plutil -replace ProgramArguments.0 -string "$legacy" "$plist"
if FAIL_AT=bootstrap bash "$service" install > "$test_root/output" 2>&1; then exit 1; fi
grep -q 'previous working driver' "$binary"
[[ $(plutil -extract ProgramArguments.0 raw -o - "$plist") == "$binary" && -f $test_root/running ]]
bash "$service" install > "$test_root/output" 2>&1
cmp "$binary" "$test_root/repo with spaces/edimax_driver"
[[ ! -e $legacy && -f $test_root/running ]]

# A second operation cannot touch a transaction in progress; use the real OS file lock.
HOLD_CONFIG=1 bash "$service" install > "$test_root/install-output" 2>&1 &
installer=$!
for retry in {1..200}; do [[ ! -f $test_root/waiting ]] || break; /bin/sleep 0.01; done
[[ -f $test_root/waiting ]]
if bash "$service" uninstall > "$test_root/output" 2>&1; then exit 1; fi
[[ -x $binary ]]
touch "$test_root/resume"
wait "$installer"
[[ -f $test_root/loaded && -f $test_root/running && -x $binary ]]
bash "$service" uninstall > "$test_root/output" 2>&1
[[ ! -e $binary && ! -e $plist && ! -e $test_root/loaded ]]
bash "$service" uninstall > "$test_root/output" 2>&1

# Binary releases fail before running helpers if contents were changed.
rm "$test_root/repo with spaces/edimax_driver.c"
(cd "$test_root/repo with spaces" && shasum -a 256 edimax_driver tools/service.sh services/*.plist > SHA256SUMS)
bash "$service" install > "$test_root/output" 2>&1
printf '\n# corrupt download\n' >> "$test_root/repo with spaces/edimax_driver"
: > "$test_root/events"
if bash "$service" install > "$test_root/output" 2>&1; then exit 1; fi
[[ ! -s $test_root/events && -f $test_root/running ]]
echo 'Protected paths, locking, rollback, migration, preflight, and release checksums passed.'
