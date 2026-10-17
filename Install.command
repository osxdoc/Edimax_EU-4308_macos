#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
trap 'echo "Installation failed. See the error above." >&2; if [[ -t 0 ]]; then read -r -p "Press Enter to close..."; fi' ERR

echo 'Building the experimental AX88179 userspace driver...'
make -B edimax_driver
sudo /bin/bash tools/service.sh install
if [[ -t 0 ]]; then read -r -p 'Press Enter to close...'; fi
