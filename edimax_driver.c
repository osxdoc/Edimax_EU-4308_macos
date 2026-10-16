#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/bpf.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

// ============================================================================
// ASIX AX88179 Hardware Constants
// ============================================================================
#define AX_ACCESS_MAC           0x01
#define AX_ACCESS_PHY           0x02
#define AX88179_PHY_ID          0x03

#define AX_RX_CTL               0x0b
    #define AX_RX_CTL_DROPCRCERR 0x0100
    #define AX_RX_CTL_IPE       0x0200
    #define AX_RX_CTL_START     0x0080
    #define AX_RX_CTL_AP        0x0020
    #define AX_RX_CTL_AM        0x0010
    #define AX_RX_CTL_AB        0x0008
    #define AX_RX_CTL_AMALL     0x0002

#define AX_NODE_ID              0x10

#define AX_MEDIUM_STATUS_MODE   0x22
    #define AX_MEDIUM_GIGAMODE  0x01
    #define AX_MEDIUM_FULL_DUPLEX 0x02
    #define AX_MEDIUM_EN_125MHZ 0x08
    #define AX_MEDIUM_RXFLOW_CTRLEN 0x10
    #define AX_MEDIUM_TXFLOW_CTRLEN 0x20
    #define AX_MEDIUM_RECEIVE_EN 0x100
    #define AX_MEDIUM_PS        0x200

#define AX_PHYPWR_RSTCTL        0x26
    #define AX_PHYPWR_RSTCTL_IPRL 0x0020

#define AX_RX_BULKIN_QCTRL      0x2e
#define AX_CLK_SELECT           0x33
    #define AX_CLK_SELECT_BCS   0x01
    #define AX_CLK_SELECT_ACS   0x02

#define AX_RXCOE_CTL            0x34
#define AX_TXCOE_CTL            0x35
    #define AX_COE_ALL          0x67

#define AX_PAUSE_WATERLVL_HIGH  0x54
#define AX_PAUSE_WATERLVL_LOW   0x55

#define MII_BMCR                0x00
    #define BMCR_ANENABLE       0x1000
    #define BMCR_ANRESTART      0x0200

#define MII_BMSR                0x01
    #define BMSR_LSTATUS        0x0004
    #define BMSR_ANEGCOMPLETE   0x0020

#define MII_ADVERTISE           0x04
#define MII_CTRL1000            0x09

#define GMII_PHY_PHYSR          0x11
    #define GMII_PHY_PHYSR_SMASK 0xc000
    #define GMII_PHY_PHYSR_GIGA 0x8000
    #define GMII_PHY_PHYSR_100  0x4000
    #define GMII_PHY_PHYSR_FULL 0x2000

#define USB_PIPE_BULK_IN        2
#define USB_PIPE_BULK_OUT       3

#define USB_RX_BUF_SIZE         65536
#define BPF_BUF_SIZE            65536

// ============================================================================
// Global State
// ============================================================================
static volatile int g_running = 1;
static volatile int g_session_active = 0;

static IOUSBDeviceInterface **g_dev = NULL;
static IOUSBInterfaceInterface **g_intf = NULL;
static int g_bpf_fd = -1;

static uint64_t g_rx_packets = 0;
static uint64_t g_rx_bytes = 0;
static uint64_t g_tx_packets = 0;
static uint64_t g_tx_bytes = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
    g_session_active = 0;
}

// ============================================================================
// USB Control Helpers
// ============================================================================
static int ax_read_cmd(IOUSBDeviceInterface **dev, uint8_t cmd, uint16_t value, uint16_t index, uint16_t len, void *data) {
    if (!dev) return -1;
    IOUSBDevRequest req;
    memset(&req, 0, sizeof(req));
    req.bmRequestType = 0xC0;
    req.bRequest = cmd;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = len;
    req.pData = data;
    kern_return_t kr = (*dev)->DeviceRequest(dev, &req);
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

static int ax_write_cmd(IOUSBDeviceInterface **dev, uint8_t cmd, uint16_t value, uint16_t index, uint16_t len, const void *data) {
    if (!dev) return -1;
    IOUSBDevRequest req;
    memset(&req, 0, sizeof(req));
    req.bmRequestType = 0x40;
    req.bRequest = cmd;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = len;
    req.pData = (void *)data;
    kern_return_t kr = (*dev)->DeviceRequest(dev, &req);
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

static uint16_t ax_mdio_read(IOUSBDeviceInterface **dev, uint16_t reg, int *err) {
    uint16_t val = 0;
    int rc = ax_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID, reg, 2, &val);
    if (err) *err = rc;
    return val;
}

static void ax_mdio_write(IOUSBDeviceInterface **dev, uint16_t reg, uint16_t val) {
    ax_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID, reg, 2, &val);
}

// ============================================================================
// BPF (Berkeley Packet Filter) Setup
// ============================================================================
static int open_bpf_on_interface(const char *ifname) {
    char bpf_path[32];
    int fd = -1;
    for (int i = 0; i < 256; i++) {
        snprintf(bpf_path, sizeof(bpf_path), "/dev/bpf%d", i);
        fd = open(bpf_path, O_RDWR);
        if (fd != -1) break;
    }
    if (fd == -1) return -1;

    u_int buf_len = BPF_BUF_SIZE;
    if (ioctl(fd, BIOCSBLEN, &buf_len) < 0) {
        close(fd);
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, BIOCSETIF, &ifr) < 0) {
        close(fd);
        return -1;
    }

    u_int imm = 1;
    ioctl(fd, BIOCIMMEDIATE, &imm);

    u_int hdr_cmplt = 1;
    ioctl(fd, BIOCSHDRCMPLT, &hdr_cmplt);

    u_int seeseen = 0;
    ioctl(fd, BIOCSSEESENT, &seeseen);

    return fd;
}

// ============================================================================
// Packet Processing Worker Threads
// ============================================================================

// RX Worker: USB Bulk IN -> BPF
static void *rx_worker_thread(void *arg) {
    (void)arg;
    uint8_t *rx_buf = malloc(USB_RX_BUF_SIZE);
    if (!rx_buf) return NULL;

    while (g_session_active && g_intf) {
        UInt32 len = USB_RX_BUF_SIZE;
        kern_return_t kr = (*g_intf)->ReadPipeTO(g_intf, USB_PIPE_BULK_IN, rx_buf, &len, 50, 50);

        if (kr == KERN_SUCCESS && len >= 4) {
            uint32_t rx_hdr = *(uint32_t *)(rx_buf + len - 4);
            uint16_t pkt_cnt = (uint16_t)rx_hdr;
            uint16_t hdr_off = (uint16_t)(rx_hdr >> 16);

            if (pkt_cnt > 0 && (hdr_off + pkt_cnt * 4) <= (len - 4)) {
                uint32_t *pkt_meta = (uint32_t *)(rx_buf + hdr_off);
                uint16_t pkt_offset = 0;

                for (int i = 0; i < pkt_cnt; i++) {
                    uint32_t meta = pkt_meta[i];
                    uint16_t pkt_len = (meta >> 16) & 0x1fff;
                    uint16_t pkt_len_aligned = (pkt_len + 7) & 0xfff8;

                    if (pkt_len == 0) continue;
                    if (pkt_offset + pkt_len > hdr_off) break;

                    if (!(meta & (0x20000000 | 0x80000000)) && pkt_len >= 16) {
                        uint8_t *frame = rx_buf + pkt_offset + 2;
                        int frame_len = pkt_len - 2;

                        if (g_bpf_fd >= 0) {
                            write(g_bpf_fd, frame, frame_len);
                            g_rx_packets++;
                            g_rx_bytes += frame_len;
                        }
                    }
                    pkt_offset += pkt_len_aligned;
                }
            }
        } else if (kr == kIOReturnNoDevice || kr == kIOReturnNotResponding) {
            // USB disconnected
            g_session_active = 0;
            break;
        }
    }

    free(rx_buf);
    return NULL;
}

// TX Worker: BPF -> USB Bulk OUT
static void *tx_worker_thread(void *arg) {
    (void)arg;
    uint8_t *bpf_buf = malloc(BPF_BUF_SIZE);
    uint8_t *tx_buf = malloc(16384);
    if (!bpf_buf || !tx_buf) return NULL;

    while (g_session_active && g_bpf_fd >= 0 && g_intf) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_bpf_fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };

        int sel = select(g_bpf_fd + 1, &fds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(g_bpf_fd, &fds)) {
            ssize_t bytes_read = read(g_bpf_fd, bpf_buf, BPF_BUF_SIZE);
            if (bytes_read <= 0) continue;

            uint8_t *ptr = bpf_buf;
            while (ptr < bpf_buf + bytes_read && g_session_active) {
                struct bpf_hdr *hdr = (struct bpf_hdr *)ptr;
                uint8_t *frame = ptr + hdr->bh_hdrlen;
                uint32_t frame_len = hdr->bh_caplen;

                if (frame_len > 0 && frame_len <= 1514) {
                    uint32_t actual_len = (frame_len < 60) ? 60 : frame_len;

                    uint32_t tx_hdr1 = (actual_len & 0x0001FFFF) | 0x80000000;
                    uint32_t tx_hdr2 = 0;
                    if (((actual_len + 8) % 1024) == 0 || ((actual_len + 8) % 512) == 0) {
                        tx_hdr2 |= 0x80008000;
                    }

                    memcpy(tx_buf, &tx_hdr1, 4);
                    memcpy(tx_buf + 4, &tx_hdr2, 4);
                    memcpy(tx_buf + 8, frame, frame_len);
                    if (actual_len > frame_len) {
                        memset(tx_buf + 8 + frame_len, 0, actual_len - frame_len);
                    }

                    UInt32 tx_total = 8 + actual_len;
                    kern_return_t kr = (*g_intf)->WritePipeTO(g_intf, USB_PIPE_BULK_OUT, tx_buf, tx_total, 100, 100);
                    if (kr == KERN_SUCCESS) {
                        g_tx_packets++;
                        g_tx_bytes += actual_len;
                    } else if (kr == kIOReturnNoDevice || kr == kIOReturnNotResponding) {
                        g_session_active = 0;
                        break;
                    }
                }
                ptr += BPF_WORDALIGN(hdr->bh_hdrlen + hdr->bh_caplen);
            }
        }
    }

    free(bpf_buf);
    free(tx_buf);
    return NULL;
}

// ============================================================================
// USB Device Discovery
// ============================================================================
static IOUSBDeviceInterface **find_and_open_device(void) {
    CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matchingDict) return NULL;

    SInt32 idVendor = 0x0B95;
    SInt32 idProduct = 0x1790;
    CFNumberRef valVendor = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &idVendor);
    CFNumberRef valProduct = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &idProduct);
    CFDictionarySetValue(matchingDict, CFSTR(kUSBVendorID), valVendor);
    CFDictionarySetValue(matchingDict, CFSTR(kUSBProductID), valProduct);
    CFRelease(valVendor);
    CFRelease(valProduct);

    io_iterator_t iterator;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matchingDict, &iterator);
    if (kr != KERN_SUCCESS) return NULL;

    io_service_t usbDeviceRef = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!usbDeviceRef) return NULL;

    IOCFPlugInInterface **plugInInterface = NULL;
    SInt32 score;
    kr = IOCreatePlugInInterfaceForService(usbDeviceRef, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugInInterface, &score);
    IOObjectRelease(usbDeviceRef);
    if (kr != KERN_SUCCESS || !plugInInterface) return NULL;

    IOUSBDeviceInterface **dev = NULL;
    (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
    (*plugInInterface)->Release(plugInInterface);
    if (!dev) return NULL;

    if ((*dev)->USBDeviceOpen(dev) != KERN_SUCCESS) {
        (*dev)->Release(dev);
        return NULL;
    }

    return dev;
}

static IOUSBInterfaceInterface **claim_interface(IOUSBDeviceInterface **dev) {
    IOUSBFindInterfaceRequest intfReq = {
        .bInterfaceClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
        .bAlternateSetting = kIOUSBFindInterfaceDontCare
    };
    io_iterator_t intfIterator;
    kern_return_t kr = (*dev)->CreateInterfaceIterator(dev, &intfReq, &intfIterator);
    if (kr != KERN_SUCCESS) return NULL;

    io_service_t usbInterfaceRef = IOIteratorNext(intfIterator);
    IOObjectRelease(intfIterator);
    if (!usbInterfaceRef) return NULL;

    IOCFPlugInInterface **intfPlugIn = NULL;
    SInt32 score;
    kr = IOCreatePlugInInterfaceForService(usbInterfaceRef, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &intfPlugIn, &score);
    IOObjectRelease(usbInterfaceRef);
    if (kr != KERN_SUCCESS || !intfPlugIn) return NULL;

    IOUSBInterfaceInterface **intf = NULL;
    (*intfPlugIn)->QueryInterface(intfPlugIn, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID *)&intf);
    (*intfPlugIn)->Release(intfPlugIn);
    if (!intf) return NULL;

    if ((*intf)->USBInterfaceOpen(intf) != KERN_SUCCESS) {
        (*intf)->Release(intf);
        return NULL;
    }

    return intf;
}

// ============================================================================
// Network & DNS Helper Routines
// ============================================================================
static void sync_system_routing_and_dns(void) {
    char router[32] = {0};
    char dns[64] = {0};
    char ip[32] = {0};

    FILE *fp_r = popen("ipconfig getoption feth0 router 2>/dev/null", "r");
    if (fp_r) {
        if (fgets(router, sizeof(router), fp_r)) router[strcspn(router, "\r\n")] = 0;
        pclose(fp_r);
    }
    if (strlen(router) == 0) {
        FILE *fp = popen("printf 'show State:/Network/Service/DHCP-feth0/IPv4\\n' | scutil | awk -F': ' '/Router/ {print $2}'", "r");
        if (fp) {
            if (fgets(router, sizeof(router), fp)) router[strcspn(router, "\r\n")] = 0;
            pclose(fp);
        }
    }

    FILE *fp_d = popen("ipconfig getoption feth0 domain_name_server 2>/dev/null", "r");
    if (fp_d) {
        if (fgets(dns, sizeof(dns), fp_d)) dns[strcspn(dns, "\r\n")] = 0;
        pclose(fp_d);
    }
    if (strlen(dns) == 0 && strlen(router) > 0) {
        strncpy(dns, router, sizeof(dns) - 1);
    }

    FILE *fp_ip = popen("ipconfig getifaddr feth0 2>/dev/null", "r");
    if (fp_ip) {
        if (fgets(ip, sizeof(ip), fp_ip)) ip[strcspn(ip, "\r\n")] = 0;
        pclose(fp_ip);
    }

    char cmd[512];

    // Mark link active in dynamic store
    system("printf 'd.init\\nd.add Link TRUE\\nset State:/Network/Interface/feth0/Link\\n' | scutil >/dev/null 2>&1");

    // Publish to System Settings service (AX88179) so it displays GREEN in macOS Settings
    if (strlen(ip) > 0 && strlen(router) > 0) {
        snprintf(cmd, sizeof(cmd),
                 "printf 'd.init\\nd.add Addresses * %s\\nd.add SubnetMasks * 255.255.255.0\\nd.add Router %s\\nd.add InterfaceName feth0\\nset State:/Network/Service/94D0AB8D-4EEC-4A7B-8DF8-C28FBE913B74/IPv4\\n' | scutil >/dev/null 2>&1",
                 ip, router);
        system(cmd);

        snprintf(cmd, sizeof(cmd),
                 "printf 'd.init\\nd.add ServerAddresses * %s %s\\nd.add DomainName fritz.box\\nset State:/Network/Service/94D0AB8D-4EEC-4A7B-8DF8-C28FBE913B74/DNS\\n' | scutil >/dev/null 2>&1",
                 dns, router);
        system(cmd);
    }

    if (strlen(router) > 0) {
        // 1. Resolve router MAC address from DHCP lease and populate ARP cache for feth0
        char router_mac[32] = {0};
        FILE *fp_m = popen("printf 'show State:/Network/Service/DHCP-feth0/IPv4\\n' | scutil | awk -F': ' '/ARPResolvedHardwareAddress/ {print $2}'", "r");
        if (fp_m) {
            if (fgets(router_mac, sizeof(router_mac), fp_m)) router_mac[strcspn(router_mac, "\r\n")] = 0;
            pclose(fp_m);
        }
        if (strlen(router_mac) > 0) {
            snprintf(cmd, sizeof(cmd), "arp -s %s %s ifscope feth0 >/dev/null 2>&1", router, router_mac);
            system(cmd);
        }

        // 2. Ensure global default route points to feth0 if en0 (Wi-Fi) is absent or down
        snprintf(cmd, sizeof(cmd),
                 "if ! route -n get default 2>/dev/null | grep -q 'interface: en0'; then "
                 "  route delete default >/dev/null 2>&1; "
                 "  route add default %s >/dev/null 2>&1; "
                 "fi", router);
        system(cmd);

        // 3. Declare feth0 as primary internet connection in SystemConfiguration
        snprintf(cmd, sizeof(cmd),
                 "printf 'd.init\\nd.add PrimaryInterface feth0\\nd.add Router %s\\nset State:/Network/Global/IPv4\\n' | scutil >/dev/null 2>&1",
                 router);
        system(cmd);
    }

    if (strlen(dns) > 0) {
        snprintf(cmd, sizeof(cmd),
                 "printf 'd.init\\nd.add ServerAddresses * %s\\nd.add DomainName fritz.box\\nd.add __FLAGS__ 6\\nset State:/Network/Global/DNS\\n' | scutil >/dev/null 2>&1",
                 dns);
        system(cmd);
    }
}

// ============================================================================
// Main Lifecycle
// ============================================================================
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("====================================================================\n");
    printf("   Edimax EU-4308 (ASIX AX88179) Native macOS Userspace Driver       \n");
    printf("====================================================================\n\n");

    if (getuid() != 0) {
        fprintf(stderr, "[-] Root privileges required. Please run: sudo ./edimax_driver\n\n");
        return 1;
    }

    // Outer Loop: Auto-Reconnect on USB Hotplug
    while (g_running) {
        printf("[*] Looking for Edimax EU-4308 (AX88179) USB adapter...\n");
        while (g_running) {
            g_dev = find_and_open_device();
            if (g_dev) break;
            sleep(1);
        }
        if (!g_running) break;

        printf("[+] Found and opened Edimax adapter!\n");

        IOUSBConfigurationDescriptorPtr configDesc;
        if ((*g_dev)->GetConfigurationDescriptorPtr(g_dev, 0, &configDesc) == KERN_SUCCESS && configDesc) {
            (*g_dev)->SetConfiguration(g_dev, configDesc->bConfigurationValue);
        }

        uint8_t mac[6] = {0};
        ax_read_cmd(g_dev, AX_ACCESS_MAC, AX_NODE_ID, 6, 6, mac);
        char mac_str[32];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        printf("[+] Hardware MAC: %s\n", mac_str);

        // Power up PHY
        uint16_t pwr = 0;
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, &pwr);
        usleep(10000);
        pwr = AX_PHYPWR_RSTCTL_IPRL;
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, &pwr);
        usleep(200000);

        uint8_t clk = AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS;
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, &clk);
        usleep(100000);

        uint8_t qctrl[5] = {7, 0x4f, 0, 0x12, 0xff};
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, qctrl);
        uint8_t wlow = 0x34, whigh = 0x52;
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_LOW, 1, 1, &wlow);
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_HIGH, 1, 1, &whigh);

        uint8_t rx_coe = AX_COE_ALL;
        uint8_t tx_coe = 0; // Disable TX checksum engine on chip
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_RXCOE_CTL, 1, 1, &rx_coe);
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, &tx_coe);

        uint16_t rxctl = AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE | AX_RX_CTL_START |
                         AX_RX_CTL_AP | AX_RX_CTL_AMALL | AX_RX_CTL_AB;
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &rxctl);

        uint16_t medium = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN |
                          AX_MEDIUM_RXFLOW_CTRLEN | AX_MEDIUM_FULL_DUPLEX |
                          AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ;
        ax_write_cmd(g_dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 2, &medium);

        ax_mdio_write(g_dev, MII_ADVERTISE, 0x05E1);
        ax_mdio_write(g_dev, MII_CTRL1000, 0x0200);

        uint16_t bmcr = ax_mdio_read(g_dev, MII_BMCR, NULL);
        bmcr |= (BMCR_ANENABLE | BMCR_ANRESTART);
        ax_mdio_write(g_dev, MII_BMCR, bmcr);

        g_intf = claim_interface(g_dev);
        if (!g_intf) {
            printf("[-] Failed to claim USB Interface. Retrying...\n");
            (*g_dev)->USBDeviceClose(g_dev);
            (*g_dev)->Release(g_dev);
            g_dev = NULL;
            sleep(1);
            continue;
        }

        // Setup Virtual Ethernet Pair (feth0 <-> feth1)
        system("ifconfig feth0 destroy 2>/dev/null");
        system("ifconfig feth1 destroy 2>/dev/null");
        system("ifconfig feth0 create");
        system("ifconfig feth1 create");
        system("ifconfig feth0 peer feth1");

        char cmd[128];
        snprintf(cmd, sizeof(cmd), "ifconfig feth0 ether %s", mac_str);
        system(cmd);
        system("ifconfig feth1 up");
        system("ifconfig feth0 up");

        g_bpf_fd = open_bpf_on_interface("feth1");
        if (g_bpf_fd < 0) {
            fprintf(stderr, "[-] Failed to attach BPF tap to feth1.\n");
            system("ifconfig feth0 destroy 2>/dev/null");
            system("ifconfig feth1 destroy 2>/dev/null");
            (*g_intf)->USBInterfaceClose(g_intf);
            (*g_intf)->Release(g_intf);
            g_intf = NULL;
            (*g_dev)->USBDeviceClose(g_dev);
            (*g_dev)->Release(g_dev);
            g_dev = NULL;
            sleep(1);
            continue;
        }

        // Start Workers
        g_session_active = 1;
        pthread_t rx_thread, tx_thread;
        pthread_create(&rx_thread, NULL, rx_worker_thread, NULL);
        pthread_create(&tx_thread, NULL, tx_worker_thread, NULL);

        printf("[+] Network bridge active! Requesting DHCP & IPv6 lease...\n");
        system("ipconfig set feth0 DHCP");
        system("ipconfig set feth0 AUTOMATIC-V6");

        // Supervision & Link Monitor Loop
        int prev_link = -1;
        int usb_fail_count = 0;
        int tick = 0;

        while (g_running && g_session_active) {
            int err = 0;
            ax_mdio_read(g_dev, MII_BMSR, &err);
            uint16_t bmsr = ax_mdio_read(g_dev, MII_BMSR, &err);
            uint16_t physr = ax_mdio_read(g_dev, GMII_PHY_PHYSR, &err);

            if (err != 0) {
                usb_fail_count++;
                if (usb_fail_count >= 2) {
                    printf("\n[*] USB device disconnected! Resetting session...\n");
                    break;
                }
            } else {
                usb_fail_count = 0;
            }

            int is_giga = (physr & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_GIGA;
            int is_100  = (physr & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_100;
            int link = (bmsr & BMSR_LSTATUS) || is_giga || is_100;

            if (link != prev_link) {
                prev_link = link;
                if (link) {
                    printf("\n[LINK STATUS] CONNECTED -> %s Full-Duplex\n", is_giga ? "1000 Mbps" : is_100 ? "100 Mbps" : "10 Mbps");
                    sync_system_routing_and_dns();
                } else {
                    printf("\n[LINK STATUS] NO CARRIER / Cable unplugged\n");
                }
            }

            // Keep default routing & DNS synchronized periodically
            if (++tick % 3 == 0 && link) {
                sync_system_routing_and_dns();
            }

            printf("\r[STATS] RX: %llu pkts (%llu KB) | TX: %llu pkts (%llu KB) | Link: %s",
                   g_rx_packets, g_rx_bytes / 1024,
                   g_tx_packets, g_tx_bytes / 1024,
                   link ? (is_giga ? "1000M" : "100M") : "DOWN");
            fflush(stdout);

            sleep(1);
        }

        // Teardown current session
        g_session_active = 0;
        pthread_join(rx_thread, NULL);
        pthread_join(tx_thread, NULL);

        if (g_bpf_fd >= 0) {
            close(g_bpf_fd);
            g_bpf_fd = -1;
        }

        system("ifconfig feth0 destroy 2>/dev/null");
        system("ifconfig feth1 destroy 2>/dev/null");

        if (g_intf) {
            (*g_intf)->USBInterfaceClose(g_intf);
            (*g_intf)->Release(g_intf);
            g_intf = NULL;
        }
        if (g_dev) {
            (*g_dev)->USBDeviceClose(g_dev);
            (*g_dev)->Release(g_dev);
            g_dev = NULL;
        }

        printf("\n[*] Session ended cleanly. Waiting for USB reconnection...\n\n");
    }

    printf("[+] Driver stopped.\n");
    return 0;
}
