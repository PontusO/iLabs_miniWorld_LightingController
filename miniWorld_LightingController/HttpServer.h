/*
    HttpServer - the product's HTTP server: serves the embedded single page
                 GUI and routes the JSON APIs.

    Replaces the throwaway adaptor that used to live in the sketch. A
    WiFiEspAT WiFiServer on port 80 with three client slots. tick() accepts
    at most one client per call and serves that request to completion, so
    Scene.tick() between calls keeps the town moving.

    begin() only records the port. tick() starts the listener the first time
    NetManager reports the ESP8285 is up, and starts it again on every
    further false-to-true edge, because a module reset takes the AT
    firmware's CIPSERVER with it while the local state still says LISTEN.

    Request handling:

      - Request line: method, path, the ?query stripped and ignored. Over
        1024 bytes gives 431, a line that is not a request line gives 400.
      - Headers until the blank line. Content-Length, Authorization, Host
        and If-None-Match are kept, everything else is skipped. A header
        line over 512 bytes, or more than 32 of them, gives 431.
      - Body up to 8192 bytes. More gives 413 and the body is not read.
      - Two seconds without a byte at any stage drops the connection, and so
        does a whole request that takes more than five seconds, so a client
        dribbling a byte at a time cannot stall the scene.

    Routing, in order:

      1. 400 for a malformed request line.
      2. 431 for an over-long or too-numerous request and header lines.
      3. 413 for an over-long body.
      4. In Portal mode, any request carrying a Host header that is not the
         AP address is answered 302 to the portal, which is what makes a
         phone open the sign-in sheet.
      5. The captive-portal probe paths: 302 to the portal. Before the
         password check, so a probe still opens the sign-in sheet on a
         device that has a password set.
      6. Basic auth when a device password is set. The user part is
         ignored; no password means no check. Failure is 401 with
         WWW-Authenticate: Basic realm="miniWorld".
      7. /api/lamps/*, /api/scene/*, /api/net/*, /api/system/* to the
         handler whose owns() matches, answered as application/json.
      8. / and /index.html: the gzipped SPA from WebUI.gen.h, with an ETag
         and 304 on a match.
      9. Anything else: 404 JSON.

    The page is always sent gzipped and Accept-Encoding is deliberately not
    parsed: the only asset is the generated header, which exists in gzip
    form only, and every browser that can run the GUI accepts gzip.

    Every response carries Connection: close and Content-Length. The web
    API handlers know nothing about this class; the call only goes one way.

    Dependencies: WiFiEspAT, NetManager, the four web API classes and
    WebUI.gen.h, which the Makefile generates from web/ before every build.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include <WiFiEspAT.h>

class HttpServer {
public:
    void begin(uint16_t port = 80);
    // Serves at most one request per call.
    void tick();

private:
    struct Request {
        String method, path, host, auth, ifNoneMatch, body;
        size_t contentLength = 0;
        bool tooLarge = false, headerTooLong = false, badRequest = false;
    };
    bool readRequest(WiFiClient &c, Request &r);
    bool readLine(WiFiClient &c, String &line, size_t maxLen);
    // Blocks until c has a byte, the client is gone or a deadline passes.
    bool waitReadable(WiFiClient &c, uint32_t idleDeadline, uint32_t &nextConnPoll);
    bool authorised(const Request &r);
    bool isCaptiveProbe(const String &path);
    void sendStatus(WiFiClient &c, int code, const char *type, const String &body, const char *extra = nullptr);
    void sendRedirect(WiFiClient &c, const char *location);
    void sendUi(WiFiClient &c, const Request &r);
    WiFiServer _server{80};

    // Set by readLine when the line it was reading passed maxLen. The
    // signature has no room for it and readRequest needs to know, so it
    // travels here: readRequest clears it before every call.
    bool _lineTooLong = false;

    // Whole-request deadline, set by readRequest and read by every wait
    // loop below it. One slow client must not hold up the scene.
    uint32_t _requestDeadline = 0;

    uint16_t _port = 80;
    bool _begun = false;            // the listener is up and accepting
    uint32_t _listenRetryAt = 0;    // next _server.begin() after a failure
    bool _lastModuleOk = false;     // for the false-to-true edge

    // Filled in by begin() from NetManager::apIP(), so the portal address
    // is named in one place only.
    String _apHost;                 // "192.168.4.1"
    String _portalUrl;              // "http://192.168.4.1/"
};

extern HttpServer Http;
