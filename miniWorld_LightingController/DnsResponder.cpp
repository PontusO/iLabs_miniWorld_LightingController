/*
    DnsResponder - see DnsResponder.h

    Invector Embedded Systems AB
*/

#include "DnsResponder.h"

bool DnsResponder::begin(IPAddress answer, uint16_t port) {
    if (_udp.begin(port) != 1) {
        return false;
    }
    _ip = answer;
    _active = true;
    return true;
}

void DnsResponder::end() {
    _udp.stop();
    _active = false;
}

void DnsResponder::tick() {
    int len = _udp.parsePacket();
    if (len <= 0) return;

    // Design section 3.4: a packet over 512 bytes is dropped, not truncated.
    // Leaving it unread is enough, the next parsePacket() discards it.
    if (len > (int)sizeof(_buf)) return;

    // Everything below bounds itself with what read() actually returned, not
    // with the wire length.
    int n = _udp.read(_buf, (size_t)len);

    // Validate packet
    if (n < 17) return;
    if (_buf[2] & 0x80) return;  // QR bit must be clear

    // Check QDCOUNT is 1
    if (_buf[4] != 0 || _buf[5] != 1) return;

    // Walk the question name from offset 12
    uint16_t idx = 12;
    while (idx < n) {
        uint8_t labelLen = _buf[idx];
        if (labelLen == 0) {
            idx++;
            break;
        }
        // Check that label doesn't exceed bounds (idx + labelLen + 1 must be < n - 4)
        if (idx + labelLen + 1 >= n - 4) return;
        idx += labelLen + 1;
    }

    // idx now points after the name's zero byte
    // QTYPE and QCLASS follow (4 bytes total)
    uint16_t qend = idx + 4;

    // Check if qend + 16 > 512, drop if so
    if (qend + 16 > 512) return;

    // Build the reply
    _buf[2] = 0x81;
    _buf[3] = 0x80;
    _buf[6] = 0;
    _buf[7] = 1;
    _buf[8] = _buf[9] = _buf[10] = _buf[11] = 0;

    // Build answer record
    uint8_t ans[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 60, 0, 4,
                      _ip[0], _ip[1], _ip[2], _ip[3] };

    memcpy(_buf + qend, ans, 16);

    // Send the reply
    _udp.beginPacket(_udp.remoteIP(), _udp.remotePort());
    _udp.write(_buf, qend + 16);
    _udp.endPacket();
    _answered++;
}
