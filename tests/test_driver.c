#define main driver_main
#include "../edimax_driver.c"
#undef main
#include <assert.h>

static void put_le32(uint8_t *data, uint32_t value) {
    value = OSSwapHostToLittleInt32(value);
    memcpy(data, &value, 4);
}

static void test_link(void) {
    assert(!link_is_up(0, GMII_PHY_PHYSR_GIGA));
    assert(!link_is_up(BMSR_LSTATUS, GMII_PHY_PHYSR_100));
    assert(!link_is_up(0, GMII_PHY_PHYSR_LINK | GMII_PHY_PHYSR_GIGA));
    assert(link_is_up(BMSR_LSTATUS, GMII_PHY_PHYSR_LINK | GMII_PHY_PHYSR_100));
    uint16_t base = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN | AX_MEDIUM_RXFLOW_CTRLEN;
    assert(medium_for_link(0) == base);
    assert(medium_for_link(GMII_PHY_PHYSR_100) == (base | AX_MEDIUM_PS));
    assert(medium_for_link(GMII_PHY_PHYSR_100 | GMII_PHY_PHYSR_FULL) ==
           (base | AX_MEDIUM_PS | AX_MEDIUM_FULL_DUPLEX));
    assert(medium_for_link(GMII_PHY_PHYSR_GIGA | GMII_PHY_PHYSR_FULL) ==
           (base | AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ | AX_MEDIUM_FULL_DUPLEX));
}

static void test_tx(void) {
    uint8_t frame[MAX_FRAME_SIZE + 1], output[MAX_FRAME_SIZE + 9];
    memset(frame, 0x5a, sizeof(frame));
    assert(build_tx_frame(output, frame, 42, 1024) == 68);
    assert(read_le32(output) == 60); // Length only: no undocumented bit 31.
    assert(read_le32(output + 4) == 0);
    assert(!memcmp(output + 8, frame, 42));
    for (size_t i = 50; i < 68; i++) assert(output[i] == 0);
    assert(build_tx_frame(output, frame, 504, 512) == 513);
    assert(read_le32(output + 4) == 0x80008000);
    assert(read_le32(output) == 504 && output[512] == 0);
    assert(build_tx_frame(output, frame, 504, 1024) == 512);
    assert(read_le32(output + 4) == 0);
    assert(build_tx_frame(output, frame, 1016, 1024) == 1025);
    assert(read_le32(output + 4) == 0x80008000);
    assert(read_le32(output) == 1016 && output[1024] == 0);
    assert(build_tx_frame(output, frame, MAX_FRAME_SIZE, 1024) == MAX_FRAME_SIZE + 8);
    assert(!build_tx_frame(output, frame, MAX_FRAME_SIZE + 1, 1024));
    assert(!build_tx_frame(output, frame, 13, 1024));
    assert(!build_tx_frame(output, frame, 60, 0));
}

static void test_rx(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
    assert(fcntl(sockets[1], F_SETFL, O_NONBLOCK) == 0);
    g_bpf_fd = sockets[0];
    uint8_t data[68] = {0}, frame[64];
    memset(data + 2, 0x11, 16);
    memset(data + 26, 0x22, 20);
    put_le32(data + 48, 18 << 16);
    put_le32(data + 52, 0x80000000); // Firmware alignment-only descriptor.
    put_le32(data + 56, 22 << 16);
    put_le32(data + 60, 0x80000000);
    put_le32(data + 64, (48 << 16) | 4);
    assert(forward_rx_packets(data, sizeof(data)) == 0);
    assert(read(sockets[1], frame, sizeof(frame)) == 16 && frame[0] == 0x11);
    assert(read(sockets[1], frame, sizeof(frame)) == 20 && frame[0] == 0x22);
    assert(g_rx_packets == 2 && g_rx_bytes == 36);
    put_le32(data + 48, (18 << 16) | 0x20000000);
    put_le32(data + 56, (22 << 16) | 0x80000000);
    assert(forward_rx_packets(data, sizeof(data)) == 0);
    assert(read(sockets[1], frame, sizeof(frame)) < 0 && errno == EAGAIN);
    assert(g_rx_packets == 2 && g_rx_errors == 2);
    put_le32(data + 48, 60 << 16); // Packet overlaps its metadata table.
    assert(forward_rx_packets(data, sizeof(data)) == -1);
    assert(forward_rx_packets(data, 3) == -1);
    put_le32(data + 64, (64 << 16) | 4); // Metadata exceeds input.
    assert(forward_rx_packets(data, sizeof(data)) == -1);
    put_le32(data + 48, 18 << 16);
    put_le32(data + 64, (48 << 16) | 4);
    int saved_stderr = dup(STDERR_FILENO);
    int null = open("/dev/null", O_WRONLY);
    assert(saved_stderr >= 0 && null >= 0);
    assert(dup2(null, STDERR_FILENO) >= 0);
    close(sockets[0]);
    g_bpf_fd = -1;
    assert(forward_rx_packets(data, sizeof(data)) == -1);
    assert(g_rx_packets == 2); // Failed injection must not count as delivered.
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    close(null);
    close(sockets[1]);
}

static void test_bpf(void) {
    uint8_t data[128] = {0};
    struct bpf_hdr header = {0};
    size_t minimum = offsetof(struct bpf_hdr, bh_hdrlen) + sizeof(header.bh_hdrlen);
    header.bh_hdrlen = (u_short)minimum;
    header.bh_caplen = header.bh_datalen = 42;
    memcpy(data, &header, minimum);
    memset(data + minimum, 0xab, 42);
    const uint8_t *frame;
    size_t length, consumed;
    assert(next_bpf_frame(data, minimum + 42, &frame, &length, &consumed) == 0);
    assert(frame == data + minimum && length == 42 && frame[0] == 0xab);
    assert(consumed == minimum + 42); // Final record need not have tail padding.
    assert(next_bpf_frame(data, minimum - 1, &frame, &length, &consumed) == -1);
    assert(next_bpf_frame(data, minimum + 41, &frame, &length, &consumed) == -1);
    header.bh_caplen = 41;
    memcpy(data, &header, minimum);
    assert(next_bpf_frame(data, sizeof(data), &frame, &length, &consumed) == -1);
    header.bh_caplen = header.bh_datalen = UINT32_MAX;
    memcpy(data, &header, minimum);
    assert(next_bpf_frame(data, sizeof(data), &frame, &length, &consumed) == -1);
    header.bh_hdrlen = 0;
    memcpy(data, &header, minimum);
    assert(next_bpf_frame(data, sizeof(data), &frame, &length, &consumed) == -1);
}

int main(void) {
    test_link();
    test_tx();
    test_rx();
    test_bpf();
    puts("Link modes, USB framing, RX delivery accounting, and BPF bounds passed.");
    return 0;
}
