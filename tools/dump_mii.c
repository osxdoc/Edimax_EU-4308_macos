#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libkern/OSByteOrder.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define AX_ACCESS_MAC           0x01
#define AX_ACCESS_PHY           0x02
#define AX88179_PHY_ID          0x03

#define AX_PHYPWR_RSTCTL_IPRL 0x0020

#define GENERAL_STATUS          0x03

static int ax_read_cmd(IOUSBDeviceInterface182 **dev, uint8_t cmd, uint16_t value, uint16_t index, uint16_t len, void *data) {
    IOUSBDevRequest req;
    memset(&req, 0, sizeof(req));
    req.bmRequestType = 0xC0;
    req.bRequest = cmd;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = len;
    req.pData = data;
    kern_return_t kr = (*dev)->DeviceRequest(dev, &req);
    if (kr != KERN_SUCCESS || req.wLenDone != len) {
        fprintf(stderr, "USB request reg=0x%04x failed: 0x%08x (%u/%u bytes)\n", value, kr, req.wLenDone, len);
        return -1;
    }
    return 0;
}

static int ax_write_cmd(IOUSBDeviceInterface182 **dev, uint8_t cmd, uint16_t value, uint16_t index, uint16_t len, const void *data) {
    IOUSBDevRequest req;
    memset(&req, 0, sizeof(req));
    req.bmRequestType = 0x40;
    req.bRequest = cmd;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = len;
    req.pData = (void *)data;
    kern_return_t kr = (*dev)->DeviceRequest(dev, &req);
    if (kr != KERN_SUCCESS || req.wLenDone != len) {
        fprintf(stderr, "USB request reg=0x%04x failed: 0x%08x (%u/%u bytes)\n", value, kr, req.wLenDone, len);
        return -1;
    }
    return 0;
}

static int ax_mdio_read(IOUSBDeviceInterface182 **dev, uint16_t reg, uint16_t *value) {
    uint16_t raw;
    if (ax_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID, reg, 2, &raw) != 0) return -1;
    *value = OSSwapLittleToHostInt16(raw);
    return 0;
}

int main(void) {
    if (geteuid() != 0) { fprintf(stderr, "Run with sudo.\n"); return 1; }
    CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matchingDict) return 1;

    SInt32 idVendor = 0x0B95;
    SInt32 idProduct = 0x1790;
    CFNumberRef valVendor = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &idVendor);
    CFNumberRef valProduct = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &idProduct);
    if (!valVendor || !valProduct) {
        if (valVendor) CFRelease(valVendor);
        if (valProduct) CFRelease(valProduct);
        CFRelease(matchingDict);
        return 1;
    }
    CFDictionarySetValue(matchingDict, CFSTR(kUSBVendorID), valVendor);
    CFDictionarySetValue(matchingDict, CFSTR(kUSBProductID), valProduct);
    CFRelease(valVendor);
    CFRelease(valProduct);

    io_iterator_t iterator;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matchingDict, &iterator);
    if (kr != KERN_SUCCESS) return 1;

    io_service_t usbDeviceRef = IOIteratorNext(iterator);
    if (!usbDeviceRef) { IOObjectRelease(iterator); return 1; }

    IOCFPlugInInterface **plugInInterface = NULL;
    SInt32 score;
    IOCreatePlugInInterfaceForService(usbDeviceRef, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugInInterface, &score);
    IOObjectRelease(usbDeviceRef);
    IOObjectRelease(iterator);
    if (!plugInInterface) return 1;

    IOUSBDeviceInterface182 **dev = NULL;
    (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182), (LPVOID *)&dev);
    (*plugInInterface)->Release(plugInInterface);
    if (!dev) return 1;

    if ((*dev)->USBDeviceOpen(dev) != KERN_SUCCESS) { (*dev)->Release(dev); return 1; }
    int result = 1;

    // Power up PHY
    uint16_t pwr = OSSwapHostToLittleInt16(AX_PHYPWR_RSTCTL_IPRL);
    if (ax_write_cmd(dev, AX_ACCESS_MAC, 0x26, 2, 2, &pwr) != 0) goto done;
    usleep(100000);
    uint8_t clk = 0x03;
    if (ax_write_cmd(dev, AX_ACCESS_MAC, 0x33, 1, 1, &clk) != 0) goto done;
    usleep(50000);

    uint8_t gen_status = 0;
    if (ax_read_cmd(dev, AX_ACCESS_MAC, GENERAL_STATUS, 1, 1, &gen_status) != 0) goto done;
    printf("GENERAL_STATUS = 0x%02X\n", gen_status);

    // Read MII registers
    printf("\n=== MII PHY Registers (PHY ID %d) ===\n", AX88179_PHY_ID);
    for (int r = 0; r <= 18; r++) {
        uint16_t v;
        if (ax_mdio_read(dev, r, &v) != 0) goto done;
        printf("Reg 0x%02X (%2d): 0x%04X\n", r, r, v);
    }

    result = 0;
done:
    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    return result;
}
