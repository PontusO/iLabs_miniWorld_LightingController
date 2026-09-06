/*
    NetManager - the WiFi state machine: owns the ESP8285, WiFiEspAT, SNTP,
                 mDNS and the captive portal DNS responder.

    The sketch calls begin() once in setup() and tick() in loop(). Everything
    else is driven from the web API. There are four states:

        NoModule    the ESP8285 did not answer, retried every 10 s
        Connecting  WiFi.begin() issued, waiting up to 20 s for WL_CONNECTED
        Online      joined a network, time and mDNS running
        Portal      own open AP miniWorld-XXXX on 192.168.4.1 with a
                    catch-all DNS responder, so a phone lands on the GUI

    Transitions:

        Boot        -> Connecting  stored credentials, or the network the AT
                                   firmware itself remembers (no-argument
                                   WiFi.begin(), used once at boot)
                    -> Portal      that attempt timed out
        Connecting  -> Online      WL_CONNECTED within 20 s
                    -> Portal      failure or timeout
        Portal      -> Connecting  every 5 min while credentials are stored,
                                   or at once from /api/net/connect, which
                                   keeps the AP up so the phone stays put
        Online      -> Portal      WL_CONNECTED lost for 2 min, or forget()

    Onboarding from the portal: connectResult() goes Connecting, then Ok or
    Failed. On Ok the AP is kept for 60 s more so the portal page can show
    the new address, then it is taken down.

    The ESP8285 hangs off Serial2. Only runReset() is taken from the board
    variant helper (ChallengerWiFi.h), because the helper's own waits report
    success when they time out; NetManager does the waiting against real
    deadlines and sends the AT+UART_CUR raise to 921600 itself. Serial1 is
    never touched: GPIO16/17 are an I2C bus on this product.

    Dependencies: WiFiEspAT, NetConfig, DnsResponder, the board variant.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include <WiFiEspAT.h>
#include "NetConfig.h"
#include "DnsResponder.h"

enum class NetMode : uint8_t { NoModule, Connecting, Online, Portal };
enum class ConnectResult : uint8_t { Idle, Connecting, Ok, Failed };

class NetManager {
public:
    bool begin();                  // brings up the ESP, loads /net.json, starts the state machine
    void tick();

    NetMode mode() const { return _mode; }
    ConnectResult connectResult() const { return _result; }
    const NetConfig &config() const { return _cfg; }

    // Save and apply hostname, ntp, tz, password. ssid/pass in cfg are
    // ignored here; use connectTo() for those.
    bool applyConfig(const NetConfig &cfg);
    // Store credentials and start an attempt now, keeping the AP up if
    // it is up.
    void connectTo(const char *ssid, const char *pass);
    // Clear credentials, go to Portal.
    void forget();

    bool apActive() const { return _apUp; }
    IPAddress apIP() const { return IPAddress(192, 168, 4, 1); }
    const char *apSsid() const { return _apSsid; }
    IPAddress ip() const;          // station IP when Online, else 0.0.0.0
    int rssi() const;
    String ssid() const;
    bool timeValid() const { return _timeValid; }
    bool moduleOk() const { return _mode != NetMode::NoModule; }

    // Blocking scan, strongest first. Returns count written.
    int scan(WiFiApData *out, int max);

    static const char *modeName(NetMode m);
    static const char *resultName(ConnectResult r);

private:
    bool bringUpModule();
    void enterNoModule();
    void enterConnecting(bool keepAp);
    void enterOnline();
    void enterPortal();
    void startAp();
    void stopAp();
    void applyOnlineServices();    // hostname, mDNS, SNTP, TZ
    void pollTime();

    NetConfig _cfg;
    NetMode _mode = NetMode::NoModule;
    ConnectResult _result = ConnectResult::Idle;
    DnsResponder _dns;
    char _apSsid[20] = "miniWorld";
    // The network the AT firmware remembers, read back for display only.
    // It is never written into _cfg: _cfg.ssid without _cfg.pass would be
    // saved to /net.json and retried with an empty passphrase forever.
    char _moduleSsid[NET_SSID_LEN] = "";
    bool _apUp = false;
    bool _moduleUp = false;
    bool _timeValid = false;
    // True while the network in use is the one the AT firmware itself
    // remembers: joined with a no-argument WiFi.begin() and retried the
    // same way, because there is no passphrase on this side. Cleared as
    // soon as the GUI supplies credentials of its own, and cleared again
    // if the one attempt at boot never produced a connection.
    bool _triedPersisted = false;
    uint32_t _stateSince = 0;      // millis() at last transition
    uint32_t _lostSince = 0;       // Online: when WL_CONNECTED was last seen
    uint32_t _apDropAt = 0;        // Online after onboarding: when to end the AP
    uint32_t _lastRetry = 0;
    uint32_t _lastTimePoll = 0;
    uint32_t _lastModuleTry = 0;
    uint32_t _lastApTry = 0;       // Portal: last startAp() attempt
    uint32_t _lastStatus = 0;      // last WiFi.status() poll, an AT round trip
};

extern NetManager Net;
