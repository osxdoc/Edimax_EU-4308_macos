#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/edimax-service-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
export TEST_ROOT="$test_root"
mkdir -p "$test_root/mock" "$test_root/repo with spaces/tools" "$test_root/repo with spaces/services" "$test_root/LaunchDaemons" "$test_root/local/bin"
# Redirect only privileges, command lookup, and system paths; execute the real script logic.
sed -e '/^\[\[ \$EUID -eq 0 \]\]/d' \
    -e 's|^export PATH=.*|export PATH="$TEST_ROOT/mock:/usr/bin:/bin:/usr/sbin:/sbin"|' \
    -e 's|/usr/local/bin|$TEST_ROOT/local/bin|g' \
    -e 's|/Library/LaunchDaemons|$TEST_ROOT/LaunchDaemons|g' \
    "$repo_dir/tools/service.sh" > "$test_root/repo with spaces/tools/service.sh"
cp "$repo_dir/services/com.edimax.ax88179.driver.plist" "$test_root/repo with spaces/services/"
touch "$test_root/repo with spaces/edimax_driver.c"
cat > "$test_root/mock/command" <<'MOCK'
#!/bin/bash
set -eu
name=${0##*/}
printf '%s %s\n' "$name" "$*" >> "$TEST_ROOT/events"
case "$name" in
    launchctl)
        case "$1" in
            print) [[ -f "$TEST_ROOT/loaded" ]] ;;
            kill) [[ ${STUCK:-0} == 1 ]] || rm -f "$TEST_ROOT/running" ;;
            bootout) [[ ! -f "$TEST_ROOT/running" ]]; rm -f "$TEST_ROOT/loaded" ;;
            bootstrap) touch "$TEST_ROOT/loaded" "$TEST_ROOT/running" ;;
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
for command in launchctl pgrep pkill sleep chown; do ln -s command "$test_root/mock/$command"; done
cat > "$test_root/repo with spaces/edimax_driver" <<'HELPER'
#!/bin/bash
printf 'helper %s\n' "$*" >> "$TEST_ROOT/events"
case "$1" in
    --version) [[ ${BAD_VERSION:-0} == 0 ]] ;;
    --configure-network|--disable-network) ;;
    *) exit 1 ;;
esac
HELPER
chmod +x "$test_root/repo with spaces/edimax_driver"
service="$test_root/repo with spaces/tools/service.sh"
binary="$test_root/local/bin/edimax_driver"
plist="$test_root/LaunchDaemons/com.edimax.ax88179.driver.plist"

# Invalid input must fail before stopping the current service.
source_plist="$test_root/repo with spaces/services/com.edimax.ax88179.driver.plist"
printf 'invalid plist\n' > "$source_plist"
if bash "$service" install > "$test_root/output" 2>&1; then echo 'Invalid plist accepted'; exit 1; fi
[[ ! -e "$test_root/events" ]]
cp "$repo_dir/services/com.edimax.ax88179.driver.plist" "$source_plist"

# A hung process must leave the loaded job and installed files alone.
printf 'old binary\n' > "$binary"
printf 'old plist\n' > "$plist"
touch "$test_root/loaded" "$test_root/running"
if STUCK=1 bash "$service" install > "$test_root/output" 2>&1; then echo 'Hung stop incorrectly succeeded'; exit 1; fi
grep -q 'Driver did not stop' "$test_root/output"
grep -qx 'old binary' "$binary"
grep -qx 'old plist' "$plist"
[[ -f "$test_root/loaded" ]]
! grep -q '^launchctl bootout' "$test_root/events"
! grep -Eq '^helper --(configure|disable)-network' "$test_root/events"

# A binary that cannot run must not stop the current service.
: > "$test_root/events"
if BAD_VERSION=1 bash "$service" install > "$test_root/output" 2>&1; then echo 'Incompatible binary accepted'; exit 1; fi
[[ -f "$test_root/loaded" && -f "$test_root/running" ]]
! grep -q '^launchctl ' "$test_root/events"

# Installation stops before unloading and uses the staged current executable.
: > "$test_root/events"
bash "$service" install > "$test_root/output" 2>&1
cmp "$binary" "$test_root/repo with spaces/edimax_driver"
[[ -f "$test_root/loaded" && -f "$test_root/running" ]]
grep -q '^launchctl kill SIGTERM ' "$test_root/events"
grep -q '^launchctl bootout ' "$test_root/events"
grep -qx 'helper --configure-network' "$test_root/events"

# Legacy executables do not understand --disable-network and must never run.
cat > "$binary" <<'LEGACY'
#!/bin/bash
echo 'LEGACY EXECUTED' >> "$TEST_ROOT/events"
exit 99
LEGACY
chmod +x "$binary"
: > "$test_root/events"
bash "$service" uninstall > "$test_root/output" 2>&1
grep -qx 'helper --disable-network' "$test_root/events"
! grep -q 'LEGACY EXECUTED' "$test_root/events"
[[ ! -e "$binary" && ! -e "$plist" && ! -f "$test_root/loaded" ]]

# A partially removed installation still disables the saved network service.
: > "$test_root/events"
bash "$service" uninstall > "$test_root/output" 2>&1
grep -qx 'helper --disable-network' "$test_root/events"

# Binary releases verify their contents before executing the helper or stopping services.
rm "$test_root/repo with spaces/edimax_driver.c"
(cd "$test_root/repo with spaces" && shasum -a 256 edimax_driver tools/service.sh services/*.plist > SHA256SUMS)
bash "$service" install > "$test_root/output" 2>&1
printf '\n# corrupt download\n' >> "$test_root/repo with spaces/edimax_driver"
: > "$test_root/events"
if bash "$service" install > "$test_root/output" 2>&1; then echo 'Corrupt binary accepted'; exit 1; fi
[[ ! -s "$test_root/events" && -f "$test_root/running" ]]
echo 'Installer preflight, stop failures, update, uninstall, and release checksums passed.'
