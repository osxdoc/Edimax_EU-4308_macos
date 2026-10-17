#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
trap 'echo "Uninstallation failed. See the error above." >&2; if [[ -t 0 ]]; then read -r -p "Press Enter to close..."; fi' ERR

if [[ -f edimax_driver.c ]]; then make -B edimax_driver; fi
sudo /bin/bash tools/service.sh uninstall
if [[ -t 0 ]]; then read -r -p 'Press Enter to close...'; fi
