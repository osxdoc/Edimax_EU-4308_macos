#!/usr/bin/env bash
# ============================================================================
#  Edimax EU-4308 One-Click Uninstaller for macOS
# ============================================================================
set -e

cd "$(dirname "$0")"

echo "===================================================================="
echo "   Edimax EU-4308 Driver Uninstallation                             "
echo "===================================================================="
echo ""
echo "Please enter your administrator password if prompted:"
sudo -v

PLIST="com.edimax.ax88179.driver.plist"

echo ""
echo "[1/3] Stopping background service..."
sudo launchctl bootout system "/Library/LaunchDaemons/${PLIST}" 2>/dev/null || \
sudo launchctl unload "/Library/LaunchDaemons/${PLIST}" 2>/dev/null || true

echo "[2/3] Removing service file..."
sudo rm -f "/Library/LaunchDaemons/${PLIST}"

echo "[3/3] Removing driver binary..."
sudo rm -f "/usr/local/bin/edimax_driver"

# Clean up interfaces if left up
sudo ifconfig feth0 destroy 2>/dev/null || true
sudo ifconfig feth1 destroy 2>/dev/null || true

echo ""
echo "===================================================================="
echo "   [SUCCESS] Edimax EU-4308 Driver uninstalled completely.           "
echo "===================================================================="
echo ""
read -p "Press [Enter] to exit..."
