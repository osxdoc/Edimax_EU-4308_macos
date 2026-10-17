#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <libkern/OSByteOrder.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/bpf.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#ifndef EDIMAX_VERSION
#define EDIMAX_VERSION "development"
#endif

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
    #define GMII_PHY_PHYSR_LINK 0x0400

#define PHYSICAL_LINK_STATUS    0x02
#define AX_USB_SS               0x04
#define AX_USB_HS               0x02
#define MAX_FRAME_SIZE          1518

#define USB_RX_BUF_SIZE         65536
#define BPF_BUF_SIZE            65536

static volatile sig_atomic_t g_running = 1;
static atomic_bool g_session_active;
static atomic_bool g_link_ready;
static IOUSBDeviceInterface182 **g_dev;
static IOUSBInterfaceInterface550 **g_intf;
static UInt8 g_bulk_in, g_bulk_out;
static UInt16 g_max_packet;
static int g_bpf_fd = -1;
static u_int g_bpf_buf_size;
static atomic_uint_fast64_t g_rx_packets, g_rx_bytes, g_tx_packets, g_tx_bytes;
static atomic_uint_fast64_t g_rx_errors, g_tx_errors;
extern char **environ;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int run_ifconfig(const char *interface, const char *action, const char *value) {
    char *args[] = { "/sbin/ifconfig", (char *)interface, (char *)action, (char *)value, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, args[0], NULL, NULL, args, environ);
    if (rc != 0) {
        fprintf(stderr, "[-] ifconfig: %s\n", strerror(rc));
        return -1;
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[-] ifconfig %s %s failed\n", interface, action);
        return -1;
    }
    return 0;
}

static int ax_read_cmd(uint8_t cmd, uint16_t value, uint16_t index, uint16_t len, void *data) {
    IOUSBDevRequestTO req = {0};
    req.bmRequestType = 0xC0;
    req.bRequest = cmd;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = len;
    req.pData = data;
    req.noDataTimeout = 500;
    req.completionTimeout = 1000;
    IOReturn kr = (*g_dev)->DeviceRequestTO(g_dev, &req);
    if (kr != kIOReturnSuccess || req.wLenDone != len) {
        fprintf(stderr, "[-] USB read cmd=0x%02x reg=0x%04x: 0x%08x (%u/%u bytes)\n",
                cmd, value, kr, req.wLenDone, len);
        return -1;
    }
    return 0;
}

static int ax_write_cmd(uint8_t cmd, uint16_t value, uint16_t index, uint16_t len, const void *data) {
    IOUSBDevRequestTO req = {0};
    req.bmRequestType = 0x40;
    req.bRequest = cmd;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = len;
    req.pData = (void *)data;
    req.noDataTimeout = 500;
    req.completionTimeout = 1000;
    IOReturn kr = (*g_dev)->DeviceRequestTO(g_dev, &req);
    if (kr != kIOReturnSuccess || req.wLenDone != len) {
        fprintf(stderr, "[-] USB write cmd=0x%02x reg=0x%04x: 0x%08x (%u/%u bytes)\n",
                cmd, value, kr, req.wLenDone, len);
        return -1;
    }
    return 0;
}

static int ax_write_word(uint8_t cmd, uint16_t reg, uint16_t index, uint16_t value) {
    value = OSSwapHostToLittleInt16(value);
    return ax_write_cmd(cmd, reg, index, 2, &value);
}

static int ax_mdio_read(uint16_t reg, uint16_t *value) {
    uint16_t raw;
    if (ax_read_cmd(AX_ACCESS_PHY, AX88179_PHY_ID, reg, 2, &raw) != 0) return -1;
    *value = OSSwapLittleToHostInt16(raw);
    return 0;
}

static int open_bpf_on_interface(const char *ifname) {
    int fd = -1;
    for (int i = 0; i < 256; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/bpf%d", i);
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) break;
        if (errno != EBUSY) return -1;
    }
    if (fd < 0) return -1;
    g_bpf_buf_size = BPF_BUF_SIZE;
    struct ifreq ifr = {0};
    strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
    u_int on = 1, off = 0;
    if (ioctl(fd, BIOCSBLEN, &g_bpf_buf_size) < 0 ||
        ioctl(fd, BIOCSETIF, &ifr) < 0 || ioctl(fd, BIOCGBLEN, &g_bpf_buf_size) < 0 ||
        ioctl(fd, BIOCIMMEDIATE, &on) < 0 || ioctl(fd, BIOCSHDRCMPLT, &on) < 0 ||
        ioctl(fd, BIOCSSEESENT, &off) < 0) {
        perror("[-] Configure BPF");
        close(fd);
        return -1;
    }
    return fd;
}

static uint32_t read_le32(const uint8_t *data) {
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return OSSwapLittleToHostInt32(value);
}

static int forward_rx_packets(const uint8_t *data, size_t length) {
    if (length < 4) return -1;
    uint32_t trailer = read_le32(data + length - 4);
    size_t count = trailer & 0xffff, headers = trailer >> 16, offset = 0;
    if (!count || headers + count * 4 > length - 4) return -1;
    for (size_t i = 0; i < count; i++) {
        uint32_t meta = read_le32(data + headers + i * 4);
        size_t packet_length = (meta >> 16) & 0x1fff;
        if (!packet_length) continue; // Alignment-only descriptor.
        size_t padded = (packet_length + 7) & ~(size_t)7;
        if (offset + padded > headers) return -1;
        if ((meta & 0xa0000000) || packet_length < 16 || packet_length > MAX_FRAME_SIZE + 2) {
            g_rx_errors++;
        } else {
            ssize_t sent;
            do {
                sent = write(g_bpf_fd, data + offset + 2, packet_length - 2);
            } while (sent < 0 && errno == EINTR);
            if (sent == (ssize_t)(packet_length - 2)) {
                g_rx_packets++;
                g_rx_bytes += sent;
            } else {
                g_rx_errors++;
                if (sent < 0 && errno != EAGAIN && errno != ENOBUFS) {
                    perror("[-] Inject Ethernet frame");
                    return -1;
                }
            }
        }
        offset += padded;
    }
    return 0;
}

static size_t build_tx_frame(uint8_t *output, const uint8_t *frame, size_t length, uint16_t max_packet) {
    if (length < 14 || length > MAX_FRAME_SIZE || !max_packet) return 0;
    size_t padded = length < 60 ? 60 : length;
    bool usb_padding = (padded + 8) % max_packet == 0;
    uint32_t header1 = OSSwapHostToLittleInt32((uint32_t)padded);
    uint32_t header2 = OSSwapHostToLittleInt32(usb_padding ? 0x80008000 : 0);
    memcpy(output, &header1, 4);
    memcpy(output + 4, &header2, 4);
    memcpy(output + 8, frame, length);
    memset(output + 8 + length, 0, padded - length);
    // A full USB packet needs a terminating short packet, excluded from Ethernet length.
    if (usb_padding) output[padded + 8] = 0;
    return padded + 8 + usb_padding;
}

static int next_bpf_frame(const uint8_t *data, size_t length, const uint8_t **frame,
                          size_t *frame_length, size_t *consumed) {
    size_t minimum_header = offsetof(struct bpf_hdr, bh_hdrlen) + sizeof(((struct bpf_hdr *)0)->bh_hdrlen);
    if (length < minimum_header) return -1;
    struct bpf_hdr header = {0};
    memcpy(&header, data, minimum_header);
    if (header.bh_hdrlen < minimum_header || header.bh_hdrlen > length ||
        header.bh_caplen > length - header.bh_hdrlen || header.bh_caplen != header.bh_datalen) return -1;
    size_t record_length = header.bh_hdrlen + header.bh_caplen;
    *consumed = BPF_WORDALIGN(record_length);
    if (*consumed > length) *consumed = record_length;
    *frame = data + header.bh_hdrlen;
    *frame_length = header.bh_caplen;
    return 0;
}

static void *rx_worker_thread(void *arg) {
    (void)arg;
    uint8_t *buffer = malloc(USB_RX_BUF_SIZE);
    if (!buffer) {
        perror("[-] Allocate RX buffer");
        g_session_active = false;
        return NULL;
    }
    while (g_session_active) {
        if (!g_link_ready) { usleep(50000); continue; }
        UInt32 length = USB_RX_BUF_SIZE;
        IOReturn kr = (*g_intf)->ReadPipeTO(g_intf, g_bulk_in, buffer, &length, 100, 500);
        if (kr == kIOReturnTimeout || kr == kIOUSBTransactionTimeout) continue;
        if (!g_link_ready) continue;
        if (kr != kIOReturnSuccess || forward_rx_packets(buffer, length) != 0) {
            fprintf(stderr, "[-] RX failed: USB=0x%08x length=%u; restarting session\n", kr, length);
            g_rx_errors++;
            g_session_active = false;
        }
    }
    free(buffer);
    return NULL;
}

static void *tx_worker_thread(void *arg) {
    (void)arg;
    uint8_t *buffer = malloc(g_bpf_buf_size);
    uint8_t output[MAX_FRAME_SIZE + 9];
    if (!buffer) {
        perror("[-] Allocate TX buffer");
        g_session_active = false;
        return NULL;
    }
    while (g_session_active) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_bpf_fd, &fds);
        struct timeval timeout = { .tv_usec = 100000 };
        int ready = select(g_bpf_fd + 1, &fds, NULL, NULL, &timeout);
        if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
        if (ready < 0) { perror("[-] BPF select"); g_session_active = false; break; }
        ssize_t length = read(g_bpf_fd, buffer, g_bpf_buf_size);
        if (length < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (length <= 0) { perror("[-] BPF read"); g_session_active = false; break; }
        size_t offset = 0;
        while (offset < (size_t)length && g_session_active) {
            const uint8_t *frame;
            size_t frame_length, consumed;
            if (next_bpf_frame(buffer + offset, length - offset, &frame, &frame_length, &consumed) != 0) {
                fprintf(stderr, "[-] Invalid or truncated BPF record\n");
                g_tx_errors++;
                break;
            }
            offset += consumed;
            if (!g_link_ready) continue;
            size_t total = build_tx_frame(output, frame, frame_length, g_max_packet);
            if (!total) { g_tx_errors++; continue; }
            IOReturn kr = (*g_intf)->WritePipeTO(g_intf, g_bulk_out, output, (UInt32)total, 100, 500);
            if (kr == kIOReturnSuccess) {
                g_tx_packets++;
                g_tx_bytes += frame_length < 60 ? 60 : frame_length;
            } else {
                fprintf(stderr, "[-] USB TX failed: 0x%08x; restarting session\n", kr);
                g_tx_errors++;
                g_session_active = false;
            }
        }
    }
    free(buffer);
    return NULL;
}

static IOUSBDeviceInterface182 **find_and_open_device(void) {
    CFMutableDictionaryRef matching = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matching) return NULL;
    SInt32 vendor = 0x0b95, product = 0x1790;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &vendor);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberSInt32Type, &product);
    if (!v || !p) {
        if (v) CFRelease(v);
        if (p) CFRelease(p);
        CFRelease(matching);
        return NULL;
    }
    CFDictionarySetValue(matching, CFSTR(kUSBVendorID), v);
    CFDictionarySetValue(matching, CFSTR(kUSBProductID), p);
    CFRelease(v);
    CFRelease(p);
    io_iterator_t iterator;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) return NULL;
    IOUSBDeviceInterface182 **dev = NULL;
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        IOCFPlugInInterface **plugin = NULL;
        SInt32 score;
        IOReturn kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                                       kIOCFPlugInInterfaceID, &plugin, &score);
        IOObjectRelease(service);
        if (kr != KERN_SUCCESS || !plugin) continue;
        HRESULT result = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182), (LPVOID *)&dev);
        (*plugin)->Release(plugin);
        if (result != S_OK || !dev) continue;
        if ((*dev)->USBDeviceOpen(dev) == KERN_SUCCESS) break;
        (*dev)->Release(dev);
        dev = NULL;
    }
    IOObjectRelease(iterator);
    return dev;
}

static IOUSBInterfaceInterface550 **claim_interface(void) {
    IOUSBFindInterfaceRequest request = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                        kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t iterator;
    if ((*g_dev)->CreateInterfaceIterator(g_dev, &request, &iterator) != KERN_SUCCESS) return NULL;
    IOUSBInterfaceInterface550 **interface = NULL;
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        IOCFPlugInInterface **plugin = NULL;
        SInt32 score;
        IOReturn kr = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                                       kIOCFPlugInInterfaceID, &plugin, &score);
        IOObjectRelease(service);
        if (kr != KERN_SUCCESS || !plugin) continue;
        HRESULT result = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID550), (LPVOID *)&interface);
        (*plugin)->Release(plugin);
        if (result != S_OK || !interface) continue;
        if ((*interface)->USBInterfaceOpen(interface) == KERN_SUCCESS) {
            UInt8 endpoints = 0;
            g_bulk_in = g_bulk_out = 0;
            g_max_packet = 0;
            if ((*interface)->GetNumEndpoints(interface, &endpoints) == KERN_SUCCESS) {
                for (UInt8 pipe = 1; pipe <= endpoints; pipe++) {
                    UInt8 direction, number, type, interval;
                    UInt16 max_packet;
                    if ((*interface)->GetPipeProperties(interface, pipe, &direction, &number, &type,
                                                       &max_packet, &interval) != KERN_SUCCESS || type != kUSBBulk) continue;
                    if (direction == kUSBIn) g_bulk_in = pipe;
                    if (direction == kUSBOut) {
                        UInt8 alternate;
                        IOUSBEndpointProperties properties = { .bVersion = kUSBEndpointPropertiesVersion3,
                            .bDirection = direction, .bEndpointNumber = number };
                        if ((*interface)->GetAlternateSetting(interface, &alternate) != KERN_SUCCESS) continue;
                        properties.bAlternateSetting = alternate;
                        // Pipe properties include USB 3 bursts; AX framing needs the base packet size.
                        if ((*interface)->GetEndpointPropertiesV3(interface, &properties) != KERN_SUCCESS) continue;
                        g_bulk_out = pipe;
                        g_max_packet = properties.wMaxPacketSize;
                    }
                }
            }
            if (g_bulk_in && g_bulk_out && g_max_packet) break;
            (*interface)->USBInterfaceClose(interface);
        }
        (*interface)->Release(interface);
        interface = NULL;
    }
    IOObjectRelease(iterator);
    return interface;
}

static uint16_t medium_for_link(uint16_t physr) {
    uint16_t mode = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN | AX_MEDIUM_RXFLOW_CTRLEN;
    if ((physr & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_GIGA) mode |= AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ;
    else if ((physr & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_100) mode |= AX_MEDIUM_PS;
    if (physr & GMII_PHY_PHYSR_FULL) mode |= AX_MEDIUM_FULL_DUPLEX;
    return mode;
}

static bool link_is_up(uint16_t bmsr, uint16_t physr) {
    return (bmsr & BMSR_LSTATUS) && (physr & GMII_PHY_PHYSR_LINK);
}

static int program_link(uint16_t physr) {
    static const uint8_t queues[][5] = {
        {7, 0x4f, 0, 0x12, 0xff}, {7, 0x20, 3, 0x16, 0xff},
        {7, 0xae, 7, 0x18, 0xff}, {7, 0xcc, 0x4c, 0x18, 8}
    };
    uint8_t usb_status;
    if (ax_read_cmd(AX_ACCESS_MAC, PHYSICAL_LINK_STATUS, 1, 1, &usb_status) != 0) return -1;
    unsigned queue = 3;
    if ((physr & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_GIGA) {
        if (usb_status & AX_USB_SS) queue = 0;
        else if (usb_status & AX_USB_HS) queue = 1;
    } else if ((physr & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_100 && (usb_status & (AX_USB_SS | AX_USB_HS))) queue = 2;
    if (ax_write_cmd(AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, queues[queue]) != 0) return -1;
    return ax_write_word(AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, medium_for_link(physr));
}

static int initialize_device(uint8_t mac[6]) {
    UInt8 configuration;
    IOUSBConfigurationDescriptorPtr descriptor;
    if ((*g_dev)->GetConfigurationDescriptorPtr(g_dev, 0, &descriptor) != KERN_SUCCESS || !descriptor ||
        (*g_dev)->GetConfiguration(g_dev, &configuration) != KERN_SUCCESS) return -1;
    if (configuration != descriptor->bConfigurationValue &&
        (*g_dev)->SetConfiguration(g_dev, descriptor->bConfigurationValue) != KERN_SUCCESS) return -1;
    if (ax_read_cmd(AX_ACCESS_MAC, AX_NODE_ID, 6, 6, mac) != 0) return -1;
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
        fprintf(stderr, "[-] Invalid factory MAC address\n");
        return -1;
    }
    if (ax_write_word(AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 0) != 0 ||
        ax_write_word(AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, AX_PHYPWR_RSTCTL_IPRL) != 0) return -1;
    usleep(500000);
    uint8_t clock = AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS;
    if (ax_write_cmd(AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, &clock) != 0) return -1;
    usleep(200000);
    uint8_t low = 0x34, high = 0x52, checksum = 0;
    if (ax_write_cmd(AX_ACCESS_MAC, AX_PAUSE_WATERLVL_LOW, 1, 1, &low) != 0 ||
        ax_write_cmd(AX_ACCESS_MAC, AX_PAUSE_WATERLVL_HIGH, 1, 1, &high) != 0 ||
        ax_write_cmd(AX_ACCESS_MAC, AX_RXCOE_CTL, 1, 1, &checksum) != 0 ||
        ax_write_cmd(AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, &checksum) != 0 ||
        ax_write_word(AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 0) != 0 ||
        ax_write_word(AX_ACCESS_MAC, AX_RX_CTL, 2, AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE | AX_RX_CTL_START |
                       AX_RX_CTL_AP | AX_RX_CTL_AMALL | AX_RX_CTL_AB) != 0 ||
        ax_write_word(AX_ACCESS_PHY, AX88179_PHY_ID, MII_ADVERTISE, 0x05e1) != 0 ||
        ax_write_word(AX_ACCESS_PHY, AX88179_PHY_ID, MII_CTRL1000, 0x0200) != 0) return -1;
    uint16_t bmcr;
    if (ax_mdio_read(MII_BMCR, &bmcr) != 0) return -1;
    return ax_write_word(AX_ACCESS_PHY, AX88179_PHY_ID, MII_BMCR, bmcr | BMCR_ANENABLE | BMCR_ANRESTART);
}

static SCNetworkServiceRef copy_feth_service(SCPreferencesRef prefs) {
    CFArrayRef services = SCNetworkServiceCopyAll(prefs);
    SCNetworkServiceRef found = NULL;
    if (services) {
        for (CFIndex i = 0; i < CFArrayGetCount(services); i++) {
            SCNetworkServiceRef service = (SCNetworkServiceRef)CFArrayGetValueAtIndex(services, i);
            SCNetworkInterfaceRef interface = SCNetworkServiceGetInterface(service);
            CFStringRef name = interface ? SCNetworkInterfaceGetBSDName(interface) : NULL;
            if (name && CFEqual(name, CFSTR("feth0"))) { found = (SCNetworkServiceRef)CFRetain(service); break; }
        }
        CFRelease(services);
    }
    return found;
}

static int configure_network_service(int mode) {
    bool install = mode > 0;
    SCPreferencesRef prefs = SCPreferencesCreate(NULL, CFSTR("Edimax AX88179"), NULL);
    if (!prefs) return -1;
    if (!SCPreferencesLock(prefs, true)) { CFRelease(prefs); return -1; }
    SCNetworkServiceRef service = copy_feth_service(prefs);
    SCNetworkSetRef set = SCNetworkSetCopyCurrent(prefs);
    int result = -1;
    bool changed = false;
    if (!set) goto done;
    if (mode < 0) {
        if (!service || !SCNetworkServiceGetEnabled(service)) { result = 0; goto done; }
        if (SCNetworkServiceSetEnabled(service, false) && SCPreferencesCommitChanges(prefs) &&
            SCPreferencesApplyChanges(prefs)) result = 0;
        goto done;
    }
    if (!service && install) {
        // shortcut: feth has no IOKit interface object; use the public preferences schema until DriverKit migration.
        CFUUIDRef uuid = CFUUIDCreate(NULL);
        CFStringRef service_id = CFUUIDCreateString(NULL, uuid);
        CFRelease(uuid);
        CFStringRef path = CFStringCreateWithFormat(NULL, NULL, CFSTR("/NetworkServices/%@/Interface"), service_id);
        const void *keys[] = { kSCPropNetInterfaceDeviceName, kSCPropNetInterfaceHardware, kSCPropNetInterfaceType };
        const void *values[] = { CFSTR("feth0"), CFSTR("Ethernet"), kSCNetworkInterfaceTypeEthernet };
        CFDictionaryRef interface = CFDictionaryCreate(NULL, keys, values, 3,
                                                       &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        Boolean saved = SCPreferencesPathSetValue(prefs, path, interface);
        CFRelease(interface);
        CFRelease(path);
        if (saved) service = SCNetworkServiceCopy(prefs, service_id);
        CFRelease(service_id);
        if (!service || !SCNetworkServiceSetName(service, CFSTR("AX88179")) ||
            !SCNetworkServiceEstablishDefaultConfiguration(service)) goto done;
        changed = true;
    }
    if (!service) {
        fprintf(stderr, "[-] No feth0 network service. Run sudo ./edimax_driver --configure-network first.\n");
        goto done;
    }
    CFArrayRef current = SCNetworkSetCopyServices(set);
    bool present = current && CFArrayContainsValue(current, CFRangeMake(0, CFArrayGetCount(current)), service);
    if (current) CFRelease(current);
    if (!present) {
        if (!install || !SCNetworkSetAddService(set, service)) goto done;
        changed = true;
    }
    if (!SCNetworkServiceGetEnabled(service)) {
        if (!install) {
            fprintf(stderr, "[-] The feth0 network service is disabled. Enable it in Network settings to use LAN.\n");
            goto done;
        }
        if (!SCNetworkServiceSetEnabled(service, true)) goto done;
        changed = true;
    }
    if (changed && (!SCPreferencesCommitChanges(prefs) || !SCPreferencesApplyChanges(prefs))) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "[-] Network service setup failed: %s\n", SCErrorString(SCError()));
    if (set) CFRelease(set);
    if (service) CFRelease(service);
    SCPreferencesUnlock(prefs);
    CFRelease(prefs);
    return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("edimax_driver %s (experimental)\n", EDIMAX_VERSION);
        return 0;
    }
    int configure = 0;
    if (argc == 2 && strcmp(argv[1], "--configure-network") == 0) configure = 1;
    if (argc == 2 && strcmp(argv[1], "--disable-network") == 0) configure = -1;
    if (argc > 1 && !configure) {
        fprintf(stderr, "Usage: %s [--version | --configure-network | --disable-network]\n", argv[0]);
        return 1;
    }
    if (geteuid() != 0) { fprintf(stderr, "Run with sudo.\n"); return 1; }
    if (configure) return configure_network_service(configure) == 0 ? 0 : 1;
    int lock = open("/var/run/edimax_driver.pid", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "[-] Cannot acquire driver lock; another instance may be running.\n");
        if (lock >= 0) close(lock);
        return 1;
    }
    if (ftruncate(lock, 0) < 0 || dprintf(lock, "%d\n", getpid()) < 0) { close(lock); return 1; }
    struct sigaction action = { .sa_handler = sig_handler };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("[-] Install signal handlers");
        close(lock);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    bool own_feth0 = false, own_feth1 = false;
    int result = 1;
    if (if_nametoindex("feth0") || if_nametoindex("feth1")) {
        fprintf(stderr, "[-] feth0/feth1 already exist; refusing to replace another interface.\n");
        goto cleanup;
    }
    if (configure_network_service(0) != 0) goto cleanup;
    if (run_ifconfig("feth0", "create", NULL) != 0) goto cleanup;
    own_feth0 = true;
    if (run_ifconfig("feth1", "create", NULL) != 0) goto cleanup;
    own_feth1 = true;
    if (run_ifconfig("feth0", "up", NULL) != 0 || run_ifconfig("feth1", "up", NULL) != 0) goto cleanup;
    g_bpf_fd = open_bpf_on_interface("feth1");
    if (g_bpf_fd < 0) { perror("[-] Open BPF"); goto cleanup; }
    printf("[*] Experimental AX88179 userspace driver; routing and DNS are managed by macOS.\n");
    while (g_running) {
        printf("[*] Waiting for AX88179 USB adapter...\n");
        while (g_running && !(g_dev = find_and_open_device())) sleep(1);
        if (!g_running) break;
        uint8_t mac[6];
        pthread_t rx_thread, tx_thread;
        bool rx_started = false, tx_started = false;
        if (initialize_device(mac) != 0 || !(g_intf = claim_interface())) {
            fprintf(stderr, "[-] USB initialization failed; retrying.\n");
            goto end_session;
        }
        char address[18];
        snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if (run_ifconfig("feth0", "ether", address) != 0) goto end_session;
        printf("[+] MAC %s; bulk IN=%u OUT=%u max-packet=%u\n", address, g_bulk_in, g_bulk_out, g_max_packet);
        g_session_active = true;
        int rc = pthread_create(&rx_thread, NULL, rx_worker_thread, NULL);
        if (rc) { fprintf(stderr, "[-] RX thread: %s\n", strerror(rc)); goto end_session; }
        rx_started = true;
        rc = pthread_create(&tx_thread, NULL, tx_worker_thread, NULL);
        if (rc) { fprintf(stderr, "[-] TX thread: %s\n", strerror(rc)); goto end_session; }
        tx_started = true;
        int previous_mode = -1;
        unsigned tick = 0;
        while (g_running && g_session_active) {
            uint16_t bmsr, physr;
            if (ax_mdio_read(MII_BMSR, &bmsr) != 0 || ax_mdio_read(MII_BMSR, &bmsr) != 0 ||
                ax_mdio_read(GMII_PHY_PHYSR, &physr) != 0) break;
            bool link = link_is_up(bmsr, physr);
            int mode = link ? medium_for_link(physr) : 0;
            if (mode != previous_mode) {
                g_link_ready = false;
                if (previous_mode > 0 && run_ifconfig("feth0", "-peer", NULL) != 0) break;
                if (link) {
                    if (program_link(physr) != 0 || ioctl(g_bpf_fd, BIOCFLUSH, NULL) < 0) break;
                    g_link_ready = true;
                    if (run_ifconfig("feth0", "peer", "feth1") != 0) { g_link_ready = false; break; }
                    printf("[LINK] %s Mbps %s-duplex\n",
                           (mode & AX_MEDIUM_GIGAMODE) ? "1000" : (mode & AX_MEDIUM_PS) ? "100" : "10",
                           (mode & AX_MEDIUM_FULL_DUPLEX) ? "full" : "half");
                } else {
                    if (ax_write_word(AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 0) != 0) break;
                    printf("[LINK] No carrier; virtual Ethernet disconnected.\n");
                }
                previous_mode = mode;
            }
            if (++tick % 10 == 0) printf("[STATS] RX=%llu TX=%llu RX-errors=%llu TX-errors=%llu\n",
                (unsigned long long)g_rx_packets, (unsigned long long)g_tx_packets,
                (unsigned long long)g_rx_errors, (unsigned long long)g_tx_errors);
            sleep(1);
        }
end_session:
        g_link_ready = false;
        g_session_active = false;
        int disconnected = run_ifconfig("feth0", "-peer", NULL);
        if (g_intf) {
            (*g_intf)->AbortPipe(g_intf, g_bulk_in);
            (*g_intf)->AbortPipe(g_intf, g_bulk_out);
        }
        if (rx_started) pthread_join(rx_thread, NULL);
        if (tx_started) pthread_join(tx_thread, NULL);
        if (g_intf) {
            (*g_intf)->USBInterfaceClose(g_intf);
            (*g_intf)->Release(g_intf);
            g_intf = NULL;
        }
        (*g_dev)->USBDeviceClose(g_dev);
        (*g_dev)->Release(g_dev);
        g_dev = NULL;
        if (disconnected != 0) goto cleanup;
        if (g_running) sleep(1);
    }
    result = 0;
cleanup:
    if (g_dev) {
        (*g_dev)->USBDeviceClose(g_dev);
        (*g_dev)->Release(g_dev);
    }
    if (g_bpf_fd >= 0) close(g_bpf_fd);
    if (own_feth0 && run_ifconfig("feth0", "destroy", NULL) != 0) result = 1;
    if (own_feth1 && run_ifconfig("feth1", "destroy", NULL) != 0) result = 1;
    close(lock);
    if (result == 0) printf("[*] Driver stopped; virtual interfaces removed.\n");
    return result;
}
