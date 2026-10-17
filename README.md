# Edimax EU-4308 macOS Driver

Experimental userspace Ethernet driver for **Edimax EU-4308 v1.0 / ASIX AX88179** (`0b95:1790`). Universal builds target macOS 14+ on Apple Silicon and Intel. Hardware-tested on macOS 27.2 / Apple Silicon.

## Install

1. Download **Edimax-EU-4308-macOS-0.1.1.zip** from [Releases](https://github.com/osxdoc/Edimax_EU-4308_macos/releases/tag/v0.1.1).
2. Extract the complete ZIP. Choose the binary package, not GitHub's **Source code** download.
3. Double-click **Install.command** and enter your administrator password. No Xcode or compiler is needed; the driver starts automatically.
4. Open **System Settings → Network → AX88179**. Use DHCP unless your LAN requires a static address. Existing network settings are preserved.

Keep the extracted folder. To uninstall, double-click **Uninstall.command**. This removes the driver and disables its network service while retaining saved settings.

Optional download verification, with the ZIP and accompanying `.sha256` file in the same folder:

```sh
shasum -a 256 -c Edimax-EU-4308-macOS-0.1.1.zip.sha256
```

## Limitations and troubleshooting

- Experimental BPF/feth implementation; no DriverKit extension.
- Ad-hoc signed, without Apple notarization. If macOS blocks it, follow [Apple's approval instructions](https://support.apple.com/en-us/102445) only if you trust the download.
- DHCP, static IPv4, IPv6, install/uninstall, and crash recovery were tested. Intel, older macOS versions, physical reconnection, sleep/wake, VPNs, and sustained throughput remain untested.

After startup, allow macOS time to configure the connection. For errors, check:

```sh
tail -n 50 /var/log/edimax_driver.log
```

See [release notes](https://github.com/osxdoc/Edimax_EU-4308_macos/releases/tag/v0.1.1) for recovery limits. Do not delete system network preferences or disable SIP.

## Build from source

Requires macOS and Xcode Command Line Tools. If needed, run `xcode-select --install` and complete the installation first.

```sh
git clone https://github.com/osxdoc/Edimax_EU-4308_macos.git
cd Edimax_EU-4308_macos
make                 # Creates ./edimax_driver for arm64 and x86_64
```

From the same directory:

```sh
make test            # Regression and installer tests
make install         # Build and install; requests administrator access
make uninstall       # Build the cleanup helper and remove the driver
make package         # Binary ZIP and SHA-256 checksum in dist/
```

## License

[MIT](LICENSE).
