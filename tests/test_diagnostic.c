#define main diagnostic_main
#include "../tools/dump_mii.c"
#undef main
#include <assert.h>

static IOReturn reply_result;
static UInt32 reply_length;
static IOReturn reply(void *self, IOUSBDevRequest *request) {
    (void)self;
    request->wLenDone = reply_length;
    if ((request->bmRequestType & 0x80) && reply_length == 2 && reply_result == kIOReturnSuccess) {
        uint16_t raw = OSSwapHostToLittleInt16(0xabcd);
        memcpy(request->pData, &raw, 2);
    }
    return reply_result;
}

int main(void) {
    IOUSBDeviceInterface182 table = { .DeviceRequest = reply };
    IOUSBDeviceInterface182 *device = &table;
    uint16_t value = 0x1234;
    reply_result = kIOReturnNoDevice;
    assert(ax_mdio_read(&device, 1, &value) == -1 && value == 0x1234);
    reply_result = kIOReturnSuccess;
    reply_length = 0;
    assert(ax_mdio_read(&device, 1, &value) == -1 && value == 0x1234);
    assert(ax_write_cmd(&device, AX_ACCESS_PHY, 3, 1, 2, &value) == -1);
    reply_length = 2;
    assert(ax_mdio_read(&device, 1, &value) == 0 && value == 0xabcd);
    assert(ax_write_cmd(&device, AX_ACCESS_PHY, 3, 1, 2, &value) == 0);
    puts("Diagnostic USB errors, short transfers, and byte order passed.");
}
