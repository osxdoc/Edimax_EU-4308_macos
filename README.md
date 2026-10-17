# Edimax EU-4308 (ASIX AX88179) macOS Userspace Driver

Experimental userspace Ethernet driver for the Edimax EU-4308 v1.0 (`VID 0x0b95`, `PID 0x1790`). It uses native macOS frameworks with no third-party libraries. Binary releases target macOS 14 or later on Apple Silicon and Intel; runtime validation currently covers macOS 27.2 / Apple Silicon. Administrator access is required for installation and use. Building from source requires Xcode Command Line Tools.

## Download and install without Xcode

1. Download `Edimax-EU-4308-macOS-0.1.0.zip` from [GitHub Releases](https://github.com/osxdoc/Edimax_EU-4308_macos/releases). The automatically generated **Source code** downloads contain no compiled executable.
2. Extract the complete ZIP and keep its files together. It includes the universal `edimax_driver`, `Install.command`, `Uninstall.command`, the service definition, and the shared service helper.
3. Double-click `Install.command` and authenticate as an administrator. The binary package installs without `make`, a compiler, or Xcode. Keep the extracted folder for updates and uninstallation.
4. To remove the driver, double-click that folder's `Uninstall.command`; no build tools are needed. It retains the disabled network service's saved settings and existing logs.

Download the accompanying `.zip.sha256` file to verify the archive before extracting it:

```sh
shasum -a 256 -c Edimax-EU-4308-macOS-0.1.0.zip.sha256
```

The package also contains `SHA256SUMS`, checked by the service helper before installation or removal, and `BUILD.txt` with the source commit and build target. Checksums detect corruption; they are not a developer identity signature.

These experimental releases are ad-hoc signed, without a Developer ID certificate or Apple notarization. Gatekeeper may block downloaded scripts or the executable. If you trust the release, follow [Apple's instructions for opening software from an unidentified developer](https://support.apple.com/en-us/102445). Do not treat an ad-hoc signature as Apple approval. A notarized release requires Developer ID signing credentials.

## Architecture and limitations

```
AX88179 USB bulk endpoints <-> edimax_driver <-> BPF on feth1 <-> feth0 <-> macOS
```

`feth` is a Darwin testing interface, not Apple's supported USB Ethernet driver interface. This project remains an experimental workaround. Apple's supported userspace architecture is [USBDriverKit with NetworkingDriverKit](https://developer.apple.com/documentation/networkingdriverkit), packaged as a system extension with the required entitlements and signing. This repository does not implement or ship a DriverKit extension. Kernel extensions are deprecated; it is inaccurate to say they are universally blocked.

The daemon discovers the bulk endpoints, configures the controller for the negotiated speed and duplex, and pairs the virtual interfaces only while the physical cable has carrier. On link loss it disconnects the pair so macOS can select another network service. A process lock prevents two instances from taking over the same interfaces. Existing `feth0` or `feth1` interfaces are never overwritten.

## Build and test

```sh
make                 # Universal arm64/x86_64 driver
make tools           # Standalone MII/PHY diagnostic
make test            # Sanitizer-backed C tests and isolated installer tests
make package         # Universal ZIP and SHA-256 checksum in dist/
make clean           # Remove generated executables
```

Tests cover carrier detection, 10/100/1000-Mbit modes, USB framing and padding, malformed BPF/USB records, and successful versus failed RX injection. They require no root access or USB device. Hardware checks must also cover DHCP/static IPv4, IPv6, LAN-bound traffic, cable removal, USB reconnection, sleep/wake, and Wi-Fi/VPN coexistence. Link speed is not a throughput measurement.

Installer tests run against temporary files and mocked system commands. They check stop failures, updates, uninstalling legacy versions, repeated uninstallation, executable preflight, and corrupt packages without changing the Mac's network. Package tests extract the ZIP, verify signatures and checksums, and exercise both entry points with a failing compiler stub and mocked administrator operations.

`VERSION` defines the release version. `make package` checks both architectures and the macOS 14 deployment target, adds an ad-hoc signature to the packaged copy, and includes the source revision in `BUILD.txt`. ZIP files stay in ignored `dist/` and are distributed as release assets, rather than committed binaries. For publication, package a clean, committed source tree and attach both the ZIP and `.zip.sha256` file to the matching GitHub release.

### Hardware validation (2026-10-17)

Tested on macOS 27.2 / Apple Silicon with the EU-4308 negotiating 1000 Mbit/s full duplex. All 20 LAN-bound pings across payload sizes 56, 462, 974, and 1472 bytes succeeded; the 974-byte case exercises the USB 1024-byte transfer boundary. IPv4 and IPv6 HTTPS returned 200. Native DHCP reached `BOUND` with a router ACK; the previous static configuration was restored and retested. launchd stop/restart, interface cleanup, duplicate-instance rejection, and Wi-Fi connectivity after stopping passed. The final installed driver reported zero RX/TX errors during validation. Build, sanitizer tests, and Clang static analysis passed.

Physical unplug/replug, sleep/wake, 10/100-Mbit hardware, Intel runtime, VPN traffic, sustained throughput, and service creation on a clean Mac remain untested. The earlier boundary test exposed a TX timeout; the fix sends a terminating short USB packet in addition to setting the controller's padding flag.

The complete uninstall/reinstall cycle was also tested on this Mac: installed files and virtual interfaces were removed, Wi-Fi became the default route, and reinstallation restored the existing static LAN address, autostart, and HTTPS connectivity. Isolated installer tests cover failure cases separately.

The v0.1.0 binary ZIP was separately extracted to a path containing spaces and used for installation, uninstallation, and reinstallation without source files or a Makefile. The packaged executable then passed LAN-bound IPv4 and IPv6 HTTPS checks. Download quarantine and first-run Gatekeeper approval on another Mac remain untested.

## Network configuration

Run the one-time setup before starting the daemon:

```sh
sudo ./edimax_driver --configure-network
sudo ./edimax_driver
```

Stop with Ctrl+C. Do not run an interactive instance alongside the installed service.

Setup finds the network service by its `feth0` interface, preserving existing manual IP, DNS, and service-order settings. If no service exists, it creates one with default protocols through the public SystemConfiguration preferences schema; `feth` is not enumerated as IOKit hardware. The daemon itself does not change saved network settings. A disabled service must be enabled explicitly in System Settings or by the setup command.

Configure **AX88179** in System Settings → Network. Use DHCP unless your network requires an assigned static address. Do not copy an IP address from somebody else's setup or use manual configuration merely to force a green indicator. macOS manages DHCP, IPv6, routes, DNS, and service priority through [SystemConfiguration](https://developer.apple.com/library/archive/documentation/Networking/Conceptual/SystemConfigFrameworks/SC_Components/SC_Components.html). The daemon does not publish fabricated status or periodically replace the default route. To prefer Wi-Fi during testing, place it before AX88179 in the service order.

## Install and uninstall

The binary ZIP's scripts use the included executable. In a source checkout, double-click `Install.command`, or run `make install`: both build before requesting administrator access and use the same service-management script. The Finder script forces a rebuild when `edimax_driver.c` exists; Make uses normal source timestamps. Installation validates the plist, checks that the current helper can run, stages the executable, stops the previous daemon, atomically replaces `/usr/local/bin/edimax_driver`, configures the network service, and registers `services/com.edimax.ax88179.driver.plist` with launchd. Installation enables autostart, including after a previous recovery disabled the job.

Double-click `Uninstall.command`, or run `make uninstall` in a source checkout. Source uninstallation builds the current helper and requires Xcode Command Line Tools; the binary ZIP's uninstaller requires neither. Both stop the daemon, use the current helper to disable the saved `feth0` service, and remove the installed executable and launchd plist. Network settings and existing logs are retained. The installed legacy executable is never invoked with new options; an already missing installed binary does not prevent cleanup.

Both operations request `SIGTERM` and wait up to 15 seconds before unloading the launchd job. If a driver process remains, they abort without replacing or deleting installed files. They do not issue `SIGKILL`. Later setup or registration failures can leave a partial installation; there is no automatic rollback. Fix the reported error and rerun installation. Successful registration alone does not prove adapter connectivity; check the status, log, and LAN-bound traffic below.

```sh
sudo launchctl print system/com.edimax.ax88179.driver
tail -f /var/log/edimax_driver.log
./edimax_driver --version
```

## Diagnostics and recovery

A carrier indication does not prove LAN or Internet connectivity. Compare traffic explicitly bound to LAN and Wi-Fi, for example `ping -b feth0 -c 3 <your-router>` and `ping -b en0 -c 3 <your-router>`; verify the Wi-Fi interface name on your Mac. Inspect `ifconfig feth0`, `scutil --nwi`, and `route -n get default`.

Stop the daemon before using `sudo ./tools/dump_mii`: the tool opens the USB device and changes PHY power/clock registers. It must not compete with the running driver.

For recovery from the old driver, stop/unload its launchd service first and disable its AX88179 network service. The old release overwrote global network state and started temporary `ipconfig` services; do not run it alongside this version. Preserve your existing Wi-Fi/VPN configuration. Do not delete all SystemConfiguration preferences or disable SIP. A stale virtual interface after a crash should be inspected before removal, since another application may own it.

## License

MIT; see [LICENSE](LICENSE).
