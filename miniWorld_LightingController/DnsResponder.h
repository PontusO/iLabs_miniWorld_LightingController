/*
    DnsResponder - a catch-all DNS server for captive portals. Responds to
                   every DNS query with the same IP address, redirecting
                   clients to the portal regardless of the hostname they query.

    Invector Embedded Systems AB
*/

#pragma once
#include <Arduino.h>
#include <WiFiEspAT.h>

class DnsResponder {
public:
    // Answer every query with this address. Returns false if the UDP
    // socket could not be opened.
    bool begin(IPAddress answer, uint16_t port = 53);
    void end();
    // Handles at most one packet per call.
    void tick();
    bool active() const { return _active; }
    uint32_t answered() const { return _answered; }
private:
    WiFiUDP _udp;
    IPAddress _ip;
    bool _active = false;
    uint32_t _answered = 0;
    uint8_t _buf[512];
};
