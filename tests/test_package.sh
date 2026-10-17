#!/bin/bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/edimax-package-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
version=$(cat "$repo_dir/VERSION")
name="Edimax-EU-4308-macOS-$version"
make -C "$repo_dir" package
(cd "$repo_dir/dist" && shasum -a 256 -c "$name.zip.sha256")
ditto -x -k "$repo_dir/dist/$name.zip" "$test_root"
package="$test_root/$name"
[[ -x "$package/edimax_driver" && -x "$package/Install.command" && -x "$package/Uninstall.command" ]]
[[ -f "$package/tools/service.sh" && -f "$package/services/com.edimax.ax88179.driver.plist" ]]
[[ ! -e "$package/edimax_driver.c" && ! -e "$package/Makefile" ]]
(cd "$package" && shasum -a 256 -c SHA256SUMS)
codesign --verify --strict --all-architectures "$package/edimax_driver"
[[ $("$package/edimax_driver" --version) == "edimax_driver $version (experimental)" ]]

# Exercise both downloaded entry points with no compiler and no administrator changes.
mkdir "$test_root/mock"
export EDIMAX_TEST_EVENTS="$test_root/events"
cat > "$test_root/mock/make" <<'MOCK'
#!/bin/bash
echo 'Compiler unexpectedly requested' >&2
exit 99
MOCK
cat > "$test_root/mock/sudo" <<'MOCK'
#!/bin/bash
exec "$@"
MOCK
cat > "$package/tools/service.sh" <<'MOCK'
#!/bin/bash
printf '%s\n' "$1" >> "$EDIMAX_TEST_EVENTS"
MOCK
chmod +x "$test_root/mock/make" "$test_root/mock/sudo"
PATH="$test_root/mock:/usr/bin:/bin:/usr/sbin:/sbin" bash "$package/Install.command" </dev/null
PATH="$test_root/mock:/usr/bin:/bin:/usr/sbin:/sbin" bash "$package/Uninstall.command" </dev/null
[[ $(cat "$EDIMAX_TEST_EVENTS") == $'install\nuninstall' ]]
echo 'Universal package, signatures, checksums, and compiler-free entry points passed.'
