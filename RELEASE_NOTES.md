# v0.1.0 — Experimental binary package

Download **Edimax-EU-4308-macOS-0.1.0.zip** and extract the complete folder. Run `Install.command` to install or `Uninstall.command` to remove the driver. Keep the folder for uninstallation. Neither script needs Xcode or a compiler in this binary package.

- Universal executable for Apple Silicon and Intel, targeting macOS 14 or later.
- Includes both scripts, shared service helper, launchd definition, license, build information, and SHA-256 checksums.
- macOS manages DHCP, IPv6, routing, DNS, and network service priority.
- Fixes USB framing, full-packet transfer termination, negotiated link settings, and installer cleanup.

The accompanying `.zip.sha256` file verifies the download with `shasum -a 256 -c Edimax-EU-4308-macOS-0.1.0.zip.sha256`.

Runtime and hardware validation covers macOS 27.2 / Apple Silicon with an EU-4308: IPv4/IPv6 HTTPS, DHCP, static addressing, packet boundaries, service lifecycle, and Wi-Fi connectivity after stopping. Build, sanitizer, installer, and package tests pass. Intel, older macOS versions, physical reconnection, sleep/wake, VPN traffic, sustained throughput, and network service creation on a clean Mac remain untested.

Installation, uninstallation, and reinstallation from the binary ZIP were tested separately, with IPv4/IPv6 HTTPS afterward. First-run Gatekeeper approval on another Mac remains untested.

**Signing:** ad-hoc signed, without Developer ID or Apple notarization. Gatekeeper may require explicit user approval; see [Apple's instructions](https://support.apple.com/en-us/102445). The BPF/feth architecture remains experimental; this release contains no DriverKit extension.
