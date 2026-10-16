# Edimax EU-4308 (ASIX AX88179) Native macOS Userspace Driver

A zero-dependency, open-source userspace network driver for the **Edimax EU-4308 (v1.0)** USB-C Multiport Adapter (and other ASIX AX88179 Gigabit Ethernet devices) on modern macOS (macOS 14 Sonoma, macOS 15 Sequoia, macOS 26/27+ on Apple Silicon & Intel).

---

## The Problem

The **Edimax EU-4308 (v1.0)** USB-C adapter combines:
* A **Realtek USB 3.0 Hub** (3x external USB-A ports)
* An **ASIX AX88179** USB 3.0 to Gigabit Ethernet controller (`VID: 0x0B95`, `PID: 0x1790`)

On modern macOS releases:
1. **No Apple Inbox Driver:** Unlike standard USB CDC-NCM adapters, the AX88179 uses a proprietary vendor class (`bDeviceClass = 255`). Apple provides no built-in driver for it.
2. **KEXT Deprecation:** Legacy Kernel Extensions (`.kext`) are completely blocked.
3. **Broken Official Driver:** ASIX's official DriverKit extension (`.dext`) is notoriously unstable, fails Gatekeeper checks, and requires container apps that easily get orphaned or fail to bind behind USB hubs.
4. **Developer Entitlement Lock:** Writing a custom DriverKit `.dext` requires restricted Apple Developer entitlements (`com.apple.developer.driverkit.family.networking`) that Apple rarely grants to individual developers.

---

## The Solution: Native Userspace Architecture

This driver runs **100% in user space** using Apple's native APIs. No kernel extensions, no disabled SIP, and no paid developer accounts required.

```
[Switch / Router Ethernet Cable]
               │
               ▼
      [ASIX AX88179 Chip]
               │  (USB Bulk IN/OUT via native IOKit)
               ▼
      [edimax_driver daemon]
               │  (Raw Layer 2 Ethernet Frames via BPF)
               ▼
   [feth1] <=======> [feth0] (macOS Virtual Ethernet Pair)
                        │
                        ▼
               macOS Network Stack (DHCP / TCP / IP)
```

### Key Highlights
* **Zero External Dependencies:** Built entirely with macOS native frameworks (`IOKit`, `CoreFoundation`, Darwin `feth` and `bpf`).
* **SIP Compatible:** Fully functional with 100% active System Integrity Protection (SIP).
* **Gigabit Performance:** Auto-negotiates 1000 Mbit/s Full-Duplex or 100 Mbit/s with your switch.
* **Auto DHCP:** Automatically provisions `feth0` with the adapter's genuine factory MAC address (`08:BE:AC:...`) and pulls an IP address via DHCP.
* **Clean Cleanup:** Automatically tears down virtual interfaces and releases USB hardware upon termination (`Ctrl+C` or `SIGTERM`).

---

## Quick Start

### 1. Build
Requirements: macOS Command Line Tools / Xcode (`clang`, `make`).
```bash
make
```

### 2. Run Directly (Interactive)
```bash
sudo ./edimax_driver
# or: make run
```

You will see live link negotiation and real-time traffic statistics:
```text
====================================================================
   Edimax EU-4308 (ASIX AX88179) Native macOS Userspace Driver       
====================================================================

[+] Hardware MAC: 08:be:ac:21:b8:aa
[*] Initializing AX88179 controller & Gigabit PHY...
[+] USB Interface claimed successfully.
[*] Setting up virtual Ethernet interface (feth0)...
[+] Network bridge established: macOS [feth0] <---> [feth1] <---> Edimax AX88179

[LINK STATUS] ACTIVE -> 1000 Mbps Full-Duplex
[STATS] RX: 124 pkts (18 KB) | TX: 98 pkts (12 KB) | Link: 1000M
```

Press `Ctrl+C` to stop the driver.

---

## Easy One-Click Installation (For End Users)

You don't need to use the terminal. In the Finder:
1. Double-click **`Install.command`**
2. Enter your macOS administrator password (or Touch ID) when prompted.
3. Done! The driver runs as an automatic background service on every boot. **No reboot required.**

To uninstall:
* Double-click **`Uninstall.command`** (or run `make uninstall`).

---

## Permanent Installation (CLI / Terminal)

```bash
make install
```

This installs the binary to `/usr/local/bin/edimax_driver` and registers a `launchd` daemon in `/Library/LaunchDaemons/com.edimax.ax88179.driver.plist`.

### Check Service Status & Logs
```bash
tail -f /var/log/edimax_driver.log
```

### macOS System Settings Integration
Under **System Settings → Network**, the adapter appears as **AX88179**. 
* Set IPv4 to **Manually** (e.g. IP `10.10.10.158`, Subnet `255.255.255.0`, Router `10.10.10.1`, DNS `10.10.10.1` or `10.10.10.32`) to have macOS show a permanent **green status indicator** and automatically route internet traffic through the adapter when Wi-Fi is disabled.

### Uninstall
```bash
make uninstall
```

---

## Diagnostic Tools

A standalone tool to inspect the hardware MII/PHY transceiver registers directly over USB is included in `tools/`:

```bash
make tools
./tools/dump_mii
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
