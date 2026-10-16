#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define AX_ACCESS_MAC           0x01
#define AX_ACCESS_PHY           0x02
#define AX88179_PHY_ID          0x03

#define AX_PHYPWR_RSTCTL_IPRL 0x0020
#define AX_GPIO_CTRL            0x25
    #define AX_GPIO_CTRL_GPIO3EN 0x80
    #define AX_GPIO_CTRL_GPIO2EN 0x40
    #define AX_GPIO_CTRL_GPIO1EN 0x20

#define GENERAL_STATUS          0x03
    #define AX_SECLD            0x04

static int ax_read_cmd(IOUSBDeviceInterface **dev, uint8_t cmd, uint16_t value, uint16_t index, uint16_t len, void *data) {
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

static uint16_t ax_mdio_read(IOUSBDeviceInterface **dev, uint16_t reg) {
    uint16_t val = 0;
    ax_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID, reg, 2, &val);
    return val;
}

__attribute__((unused))
static void ax_mdio_write(IOUSBDeviceInterface **dev, uint16_t reg, uint16_t val) {
    ax_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID, reg, 2, &val);
}

int main(void) {
    CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matchingDict) return 1;

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
    if (kr != KERN_SUCCESS) return 1;

    io_service_t usbDeviceRef = IOIteratorNext(iterator);
    if (!usbDeviceRef) return 1;

    IOCFPlugInInterface **plugInInterface = NULL;
    SInt32 score;
    IOCreatePlugInInterfaceForService(usbDeviceRef, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugInInterface, &score);
    IOObjectRelease(usbDeviceRef);
    IOObjectRelease(iterator);
    if (!plugInInterface) return 1;

    IOUSBDeviceInterface **dev = NULL;
    (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
    (*plugInInterface)->Release(plugInInterface);
    if (!dev) return 1;

    if ((*dev)->USBDeviceOpen(dev) != KERN_SUCCESS) return 1;

    // Power up PHY
    uint16_t pwr = AX_PHYPWR_RSTCTL_IPRL;
    ax_write_cmd(dev, AX_ACCESS_MAC, 0x26, 2, 2, &pwr);
    usleep(100000);
    uint8_t clk = 0x03;
    ax_write_cmd(dev, AX_ACCESS_MAC, 0x33, 1, 1, &clk);
    usleep(50000);

    uint8_t gen_status = 0;
    ax_read_cmd(dev, AX_ACCESS_MAC, GENERAL_STATUS, 1, 1, &gen_status);
    printf("GENERAL_STATUS = 0x%02X\n", gen_status);

    // Read MII registers
    printf("\n=== MII PHY Registers (PHY ID %d) ===\n", AX88179_PHY_ID);
    for (int r = 0; r <= 18; r++) {
        uint16_t v = ax_mdio_read(dev, r);
        printf("Reg 0x%02X (%2d): 0x%04X\n", r, r, v);
    }

    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    return 0;
}
