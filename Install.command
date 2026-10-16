#!/usr/bin/env bash
# ============================================================================
#  Edimax EU-4308 One-Click Installer for macOS
# ============================================================================
cd "$(dirname "$0")"

trap 'echo ""; echo "[-] Installation aborted or failed. See error above."; echo ""; read -p "Press [Enter] to exit..." dummy; exit 1' ERR

echo "===================================================================="
echo "   Edimax EU-4308 (ASIX AX88179) Driver Installation                "
echo "===================================================================="
echo ""
echo "This installer will:"
echo "  1. Prepare the Universal Binary (Apple Silicon & Intel)"
echo "  2. Install the driver to /usr/local/bin/edimax_driver"
echo "  3. Configure a background service so it starts automatically on boot"
echo ""

# Prompt for sudo password
echo "Please authenticate with your administrator password or Touch ID:"
sudo -v

echo ""
echo "[1/3] Preparing driver binary..."
if which clang >/dev/null 2>&1; then
    echo "      Compiling latest Universal Binary (Apple Silicon & Intel)..."
    clang -O2 -arch arm64 -arch x86_64 \
        -framework IOKit -framework CoreFoundation -lpthread \
        edimax_driver.c -o edimax_driver
elif [ -f edimax_driver ]; then
    echo "      Using existing pre-compiled Universal Binary."
else
    echo "[-] Error: edimax_driver binary not found and clang compiler is not installed."
    echo "    Please install Xcode Command Line Tools: xcode-select --install"
    exit 1
fi

echo "[2/3] Installing binary to /usr/local/bin..."
sudo launchctl bootout system "/Library/LaunchDaemons/${PLIST}" 2>/dev/null || true
sudo killall -9 edimax_driver 2>/dev/null || true
sudo mkdir -p /usr/local/bin
sudo cp -f edimax_driver /usr/local/bin/edimax_driver
sudo chmod 755 /usr/local/bin/edimax_driver

echo "[3/3] Setting up background service (launchd)..."
PLIST="com.edimax.ax88179.driver.plist"
sudo cp "services/${PLIST}" "/Library/LaunchDaemons/${PLIST}"
sudo chown root:wheel "/Library/LaunchDaemons/${PLIST}"
sudo chmod 644 "/Library/LaunchDaemons/${PLIST}"

# Bootout old if existing, then bootstrap
sudo launchctl bootout system "/Library/LaunchDaemons/${PLIST}" 2>/dev/null || true
sudo launchctl bootstrap system "/Library/LaunchDaemons/${PLIST}" 2>/dev/null || sudo launchctl load "/Library/LaunchDaemons/${PLIST}"

echo ""
echo "===================================================================="
echo "   [SUCCESS] Edimax EU-4308 Driver installed successfully!           "
echo "===================================================================="
echo ""
echo "The driver is now running in the background."
echo "NO REBOOT is needed! The network interface is active immediately."
echo ""
echo "To check the live driver logs, run:"
echo "  tail -f /var/log/edimax_driver.log"
echo ""
echo "To uninstall at any time, run: ./Uninstall.command (or make uninstall)"
echo ""
read -p "Press [Enter] to finish..." dummy
