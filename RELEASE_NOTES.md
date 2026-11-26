# v0.1.2 — USB stability, event-driven hotplug, and installer staging

Download **Edimax-EU-4308-macOS-0.1.2.zip**, extract the complete folder, and run `Install.command`. Keep the folder for `Uninstall.command`. Neither operation requires Xcode or a compiler. This version supersedes v0.1.1; existing users should install this update.

- Accepts USB Bulk-IN zero-length packets without resetting the active Ethernet session, increases BPF TX buffer headroom to 256 KB, and marks the LaunchDaemon `Interactive` to avoid background I/O throttling.
- Stops MAC RX before PHY reset, clears USB bulk endpoint stalls and data toggles on interface claim, and resets MAC RX/medium registers on clean shutdown.
- Detects sub-second link drops and sleep/wake subnet changes via the latched `MII_BMSR` link bit, and restricts IEEE 802.3x PAUSE flow control to full-duplex links.
- Replaces 1 Hz idle polling when unplugged with IOKit hotplug notifications, handles `SIGHUP`/`SIGPIPE` cleanly, and logs `[STATS]` only when counters change.
- Retries `SCPreferencesLock` on `kSCStatusStale`, avoids `launchd` crash loops when the `AX88179` service is disabled or outside the active location, and stages installer inputs into root-owned storage before checksum verification and execution.

Includes a universal arm64/x86_64 executable targeting macOS 14+, both Finder scripts, service helper/definition, license, source revision, and SHA-256 checksums. Verify the download with `shasum -a 256 -c Edimax-EU-4308-macOS-0.1.2.zip.sha256`.

Automated sanitizer, installer, staging, locking, rollback, location, recovery, package, and diagnostic tests pass. On macOS 27.3 / Apple Silicon, live hardware validation passed event-driven USB hotplug, 1000 Mbps full-duplex negotiation, DHCP IPv4, SLAAC/DHCPv6 IPv6, and multi-megabyte IPv4/IPv6 HTTPS transfers over `feth0` (>460k packets, 0 RX/TX errors, 0 idle log lines). Intel, older macOS versions, first-download Gatekeeper approval, sleep/wake, VPNs, and multi-hour sustained load remain untested. A forced kill between interface creation and ownership recording requires manual inspection.

**Signing:** ad-hoc signed, without Developer ID or Apple notarization; [Gatekeeper may require explicit approval](https://support.apple.com/en-us/102445). The BPF/feth architecture remains experimental; this package contains no DriverKit extension.
