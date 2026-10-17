#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"
version=$(cat VERSION)
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid VERSION.' >&2; exit 1; }
name="Edimax-EU-4308-macOS-$version"
stage=$(mktemp -d "${TMPDIR:-/tmp}/edimax-package.XXXXXX")
trap 'rm -rf "$stage"' EXIT
package="$stage/$name"
mkdir -p "$package/tools" "$package/services" dist
lipo edimax_driver -verify_arch arm64
lipo edimax_driver -verify_arch x86_64
xcrun vtool -show-build edimax_driver | awk '/minos/ { if ($2 != "14.0") exit 1; count++ } END { if (count != 2) exit 1 }'
cp edimax_driver Install.command Uninstall.command README.md LICENSE VERSION "$package/"
cp tools/service.sh "$package/tools/"
cp services/com.edimax.ax88179.driver.plist "$package/services/"
chmod 755 "$package/edimax_driver" "$package/Install.command" "$package/Uninstall.command"
codesign --force --sign - --timestamp=none "$package/edimax_driver"
codesign --verify --strict --all-architectures "$package/edimax_driver"
commit=$(git rev-parse --verify HEAD 2>/dev/null || echo unknown)
if ! git diff --quiet --ignore-submodules HEAD 2>/dev/null; then commit="$commit-dirty"; fi
printf 'Version: %s\nSource commit: %s\nArchitectures: arm64 x86_64\nMinimum macOS: 14.0\nSigning: ad-hoc; no Developer ID or notarization\n' "$version" "$commit" > "$package/BUILD.txt"
(
    cd "$package"
    shasum -a 256 edimax_driver Install.command Uninstall.command tools/service.sh services/*.plist README.md LICENSE VERSION BUILD.txt > SHA256SUMS
)
ditto -c -k --keepParent --norsrc --noextattr "$package" "dist/$name.zip"
(cd dist && shasum -a 256 "$name.zip" > "$name.zip.sha256")
echo "Created dist/$name.zip and dist/$name.zip.sha256"
