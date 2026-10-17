# v0.1.1 — Installer security and recovery fixes

Download **Edimax-EU-4308-macOS-0.1.1.zip**, extract the complete folder, and run `Install.command`. Keep the folder for `Uninstall.command`. Neither operation requires Xcode or a compiler. This version supersedes v0.1.0; existing users should install this update.

- Moves the root daemon from the user-writable `/usr/local/bin` directory into `/Library/PrivilegedHelperTools`, validates directory permissions, and migrates old installations.
- Serializes install/uninstall operations with macOS file locking. Failed updates restore the previous executable, service definition, and loaded job; failed recovery retains private backups.
- Recovers interfaces after a driver crash only when recorded interface indices and boot identity match. Foreign or unverified interfaces remain untouched.
- Selects the network service in the active network location, preserving its manual IP, DNS, and service order.
- Makes USB diagnostics reject failed and incomplete transfers, and removes unused hardware definitions.

Includes a universal arm64/x86_64 executable targeting macOS 14+, both Finder scripts, service helper/definition, license, source revision, and SHA-256 checksums. Verify the download with `shasum -a 256 -c Edimax-EU-4308-macOS-0.1.1.zip.sha256`.

Automated sanitizer, installer, package, locking, rollback, location, recovery, and diagnostic tests pass. On macOS 27.2 / Apple Silicon, the binary ZIP passed protected installation, forced-kill recovery, uninstall/reinstall, preserved static settings, checked PHY diagnostics, and LAN-bound IPv4/IPv6 HTTPS. One post-crash RX error recovered through an automatic USB-session restart; details are in the README. Intel, older macOS versions, first-download Gatekeeper approval, physical reconnection, sleep/wake, VPNs, and sustained throughput remain untested. A forced kill between interface creation and ownership recording requires manual inspection.

**Signing:** ad-hoc signed, without Developer ID or Apple notarization; [Gatekeeper may require explicit approval](https://support.apple.com/en-us/102445). The BPF/feth architecture remains experimental; this package contains no DriverKit extension.
