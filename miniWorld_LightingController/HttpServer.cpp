/*
    HttpServer - see HttpServer.h

    Invector Embedded Systems AB
*/

#include "HttpServer.h"

#include "LampWebApi.h"
#include "SceneWebApi.h"
#include "NetWebApi.h"
#include "SystemWebApi.h"
#include "NetManager.h"
#include "WebUI.gen.h"

HttpServer Http;

// Limits from the design. The body limit is what a whole scene or lamp
// configuration needs with room to spare.
static const size_t   REQUEST_LINE_MAX = 1024;
static const size_t   HEADER_LINE_MAX  = 512;
static const uint8_t  HEADER_LINES_MAX = 32;
static const size_t   BODY_MAX         = 8192;
static const uint32_t READ_TIMEOUT_MS  = 2000;   // silence at one stage
static const uint32_t REQUEST_TIMEOUT_MS = 5000; // the whole request
static const uint32_t CONN_POLL_MS     = 50;     // connected() is an AT round trip
static const size_t   UI_CHUNK         = 1024;

// Three client slots, and the AT firmware drops a connection that goes
// quiet for this many seconds.
static const uint8_t  MAX_CLIENTS      = 3;
static const uint16_t SERVER_TIMEOUT_S = 10;

// A refused CIPSERVER is retried on this interval rather than on every pass
// through loop(), because each attempt is an AT round trip over the link the
// GUI is served on.
static const uint32_t LISTEN_RETRY_MS  = 10000;

// The paths phones and desktops fetch to decide whether they are behind a
// captive portal. Answering them with a redirect is what opens the sign-in
// sheet.
static const char *const CAPTIVE_PROBES[] = {
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/connecttest.txt",
    "/ncsi.txt",
    "/canonical.html",
    "/success.txt",
    "/redirect",
};

static const char JSON_TYPE[] = "application/json";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Wrap-safe: true once millis() has reached the deadline.
static inline bool expired(uint32_t deadline) {
    return (int32_t)(millis() - deadline) >= 0;
}

static const char *reasonFor(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        default:  return "Error";
    }
}

// True when line starts with "<name>:", case-insensitively. The value is
// everything after the colon, trimmed.
static bool headerValue(const String &line, const char *name, String &value) {
    size_t n = strlen(name);
    if (line.length() <= n || line[n] != ':') {
        return false;
    }
    if (strncasecmp(line.c_str(), name, n) != 0) {
        return false;
    }
    value = line.substring(n + 1);
    value.trim();
    return true;
}

// Standard base64 alphabet. Missing padding is accepted, '=' and anything
// after it is ignored, and any character outside the alphabet is a reject.
static bool base64Decode(const String &in, String &out) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out = "";
    uint8_t quad[4];
    int n = 0;
    for (unsigned int i = 0; i < in.length(); i++) {
        char ch = in[i];
        if (ch == '=') {
            break;                          // padding, and the tail with it
        }
        const char *p = (ch == 0) ? nullptr : strchr(alphabet, ch);
        if (p == nullptr) {
            return false;                   // outside the alphabet
        }
        quad[n++] = (uint8_t)(p - alphabet);
        if (n == 4) {
            out += (char)((quad[0] << 2) | (quad[1] >> 4));
            out += (char)(((quad[1] & 0x0f) << 4) | (quad[2] >> 2));
            out += (char)(((quad[2] & 0x03) << 6) | quad[3]);
            n = 0;
        }
    }
    if (n == 1) {
        return false;                       // six bits is not a byte
    }
    if (n >= 2) {
        out += (char)((quad[0] << 2) | (quad[1] >> 4));
    }
    if (n >= 3) {
        out += (char)(((quad[1] & 0x0f) << 4) | (quad[2] >> 2));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Only records the port and the portal address. The listener is started
// from tick(), because it needs the ESP8285 to be up and has to be started
// again after every module reset.
void HttpServer::begin(uint16_t port) {
    _port = port;
    _apHost = Net.apIP().toString();
    _portalUrl = "http://" + _apHost + "/";
}

// ---------------------------------------------------------------------------
// Reading the request
// ---------------------------------------------------------------------------

// connected() costs an AT round trip, so it is polled at most every
// CONN_POLL_MS while waiting rather than on every turn of the loop.
bool HttpServer::waitReadable(WiFiClient &c, uint32_t idleDeadline, uint32_t &nextConnPoll) {
    while (c.available() <= 0) {
        if (expired(_requestDeadline) || expired(idleDeadline)) {
            return false;
        }
        if (expired(nextConnPoll)) {
            nextConnPoll = millis() + CONN_POLL_MS;
            if (!c.connected() && c.available() <= 0) {
                return false;               // peer gone with nothing buffered
            }
        }
        yield();
    }
    return true;
}

// Reads to the next '\n', strips '\r', and gives up after two seconds
// without a byte or when the whole-request deadline passes. A line longer
// than maxLen is truncated, _lineTooLong is set and the rest of it is
// discarded under the same deadlines, so the caller still finds the
// following headers where it expects them.
bool HttpServer::readLine(WiFiClient &c, String &line, size_t maxLen) {
    line = "";
    uint32_t idle = millis() + READ_TIMEOUT_MS;
    uint32_t nextPoll = millis() + CONN_POLL_MS;
    for (;;) {
        if (!waitReadable(c, idle, nextPoll)) {
            return false;
        }
        int ch = c.read();
        if (ch < 0) {
            continue;                       // deadlines rechecked above
        }
        idle = millis() + READ_TIMEOUT_MS;
        if (ch == '\n') {
            return true;
        }
        if (ch == '\r') {
            continue;
        }
        if (line.length() < maxLen) {
            line += (char)ch;
        } else {
            _lineTooLong = true;
        }
    }
}

bool HttpServer::readRequest(WiFiClient &c, Request &r) {
    String line;

    _requestDeadline = millis() + REQUEST_TIMEOUT_MS;

    _lineTooLong = false;
    if (!readLine(c, line, REQUEST_LINE_MAX)) {
        return false;
    }
    if (_lineTooLong) {
        // The line is truncated, so there is nothing to parse. Read the
        // headers anyway so the answer can be 431 rather than a hang-up.
        r.headerTooLong = true;
    } else {
        int sp1 = line.indexOf(' ');
        int sp2 = (sp1 < 0) ? -1 : line.indexOf(' ', sp1 + 1);
        if (sp1 < 1 || sp2 < 0) {
            r.badRequest = true;            // not a request line, answer 400
        } else {
            r.method = line.substring(0, sp1);
            r.path = line.substring(sp1 + 1, sp2);
            int q = r.path.indexOf('?');
            if (q >= 0) {
                r.path.remove(q);
            }
        }
    }

    uint8_t headerLines = 0;
    for (;;) {
        _lineTooLong = false;
        if (!readLine(c, line, HEADER_LINE_MAX)) {
            return false;
        }
        if (line.length() == 0 && !_lineTooLong) {
            break;                          // end of the headers
        }
        if (++headerLines > HEADER_LINES_MAX) {
            r.headerTooLong = true;
            return true;                    // 431, nothing more is read
        }
        if (_lineTooLong) {
            r.headerTooLong = true;
            continue;
        }
        String value;
        if (headerValue(line, "Content-Length", value)) {
            long n = value.toInt();
            if (n < 0 || (size_t)n > BODY_MAX) {
                r.tooLarge = true;
            } else {
                r.contentLength = (size_t)n;
            }
        } else if (headerValue(line, "Authorization", value)) {
            r.auth = value;
        } else if (headerValue(line, "Host", value)) {
            int colon = value.indexOf(':');  // Host may carry a port
            if (colon >= 0) {
                value.remove(colon);
            }
            r.host = value;
        } else if (headerValue(line, "If-None-Match", value)) {
            r.ifNoneMatch = value;
        }
    }

    if (r.badRequest || r.headerTooLong || r.tooLarge) {
        return true;                        // the body is never read
    }

    if (r.contentLength > 0) {
        r.body.reserve(r.contentLength + 1);
        uint32_t idle = millis() + READ_TIMEOUT_MS;
        uint32_t nextPoll = millis() + CONN_POLL_MS;
        char buf[64];
        while (r.body.length() < r.contentLength) {
            if (!waitReadable(c, idle, nextPoll)) {
                return false;
            }
            size_t want = r.contentLength - r.body.length();
            if (want > sizeof(buf)) {
                want = sizeof(buf);
            }
            int avail = c.available();
            if (avail > 0 && (size_t)avail < want) {
                want = (size_t)avail;
            }
            int got = c.read((uint8_t *)buf, want);
            if (got <= 0) {
                if (expired(_requestDeadline)) {
                    return false;
                }
                yield();
                continue;
            }
            r.body.concat(buf, (unsigned int)got);
            idle = millis() + READ_TIMEOUT_MS;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Authorisation
// ---------------------------------------------------------------------------

bool HttpServer::authorised(const Request &r) {
    const NetConfig &cfg = Net.config();
    if (!cfg.hasPassword()) {
        return true;                        // no password, no check
    }
    String scheme = r.auth.substring(0, 6);
    scheme.toLowerCase();
    if (scheme != "basic ") {
        return false;
    }
    String token = r.auth.substring(6);
    token.trim();
    String creds;
    if (!base64Decode(token, creds)) {
        return false;
    }
    int colon = creds.indexOf(':');
    if (colon < 0) {
        return false;
    }
    // The user part is ignored on purpose: the device has one password and
    // no accounts, so any user name the browser offers is accepted.
    return strcmp(creds.c_str() + colon + 1, cfg.password) == 0;
}

bool HttpServer::isCaptiveProbe(const String &path) {
    for (unsigned i = 0; i < sizeof(CAPTIVE_PROBES) / sizeof(CAPTIVE_PROBES[0]); i++) {
        if (path == CAPTIVE_PROBES[i]) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

// extra, when given, is complete header lines each ending in CRLF.
void HttpServer::sendStatus(WiFiClient &c, int code, const char *type,
                            const String &body, const char *extra) {
    String head;
    head.reserve(128 + (extra ? strlen(extra) : 0));
    head += "HTTP/1.1 ";
    head += code;
    head += ' ';
    head += reasonFor(code);
    head += "\r\nContent-Type: ";
    head += type;
    head += "\r\nContent-Length: ";
    head += (unsigned)body.length();
    head += "\r\nConnection: close\r\n";
    if (extra) {
        head += extra;
    }
    head += "\r\n";
    c.write((const uint8_t *)head.c_str(), head.length());
    if (body.length()) {
        c.write((const uint8_t *)body.c_str(), body.length());
    }
}

void HttpServer::sendRedirect(WiFiClient &c, const char *location) {
    String extra = "Location: ";
    extra += location;
    extra += "\r\n";
    sendStatus(c, 302, "text/plain", "", extra.c_str());
}

void HttpServer::sendUi(WiFiClient &c, const Request &r) {
    if (r.ifNoneMatch == WEBUI_ETAG) {
        sendStatus(c, 304, "text/html; charset=utf-8", "",
                   "ETag: " WEBUI_ETAG "\r\nCache-Control: no-cache\r\n");
        return;
    }

    String head;
    head.reserve(192);
    head += "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Encoding: gzip\r\n"
            "ETag: " WEBUI_ETAG "\r\n"
            "Cache-Control: no-cache\r\n"
            "Content-Length: ";
    head += (unsigned)WEBUI_GZ_LEN;
    head += "\r\nConnection: close\r\n\r\n";
    c.write((const uint8_t *)head.c_str(), head.length());

    // Flash is memory mapped on the RP2040, so the page goes out straight
    // from the image, a kilobyte at a time.
    size_t sent = 0;
    while (sent < WEBUI_GZ_LEN) {
        size_t n = WEBUI_GZ_LEN - sent;
        if (n > UI_CHUNK) {
            n = UI_CHUNK;
        }
        size_t written = c.write(WEBUI_GZ + sent, n);
        if (written == 0) {
            return;                         // client gone, stop writing
        }
        sent += written;
        yield();
    }
}

// ---------------------------------------------------------------------------
// The request loop
// ---------------------------------------------------------------------------

void HttpServer::tick() {
    // The AT firmware forgets its CIPSERVER across a module reset, and
    // NetManager resets the module to recover it, so the listener is
    // started on every false-to-true edge of moduleOk() and not just once.
    bool moduleOk = Net.moduleOk();
    if (moduleOk && (!_lastModuleOk || (!_begun && expired(_listenRetryAt)))) {
        _listenRetryAt = millis() + LISTEN_RETRY_MS;
        _server.begin(_port, MAX_CLIENTS, SERVER_TIMEOUT_S);
        // WiFiServer::operator bool() is false when the listener ended up
        // CLOSED, so a refused CIPSERVER leaves _begun false and the test
        // above tries again.
        _begun = (bool)_server;
        if (_begun) {
            Serial.printf("http: listening on port %u\n", _port);
        } else {
            Serial.println("http: listen failed");
        }
    }
    _lastModuleOk = moduleOk;
    if (!_begun) {
        return;
    }

    WiFiClient c = _server.accept();
    if (!c) {
        return;
    }

    Request r;
    if (!readRequest(c, r)) {
        c.stop();
        return;
    }

    String out;
    if (r.badRequest) {
        sendStatus(c, 400, JSON_TYPE, "{\"error\":\"bad request\"}");
    } else if (r.headerTooLong) {
        sendStatus(c, 431, JSON_TYPE, "{\"error\":\"header too large\"}");
    } else if (r.tooLarge) {
        sendStatus(c, 413, JSON_TYPE, "{\"error\":\"payload too large\"}");
    } else if (Net.mode() == NetMode::Portal && r.host != _apHost && r.host.length()) {
        sendRedirect(c, _portalUrl.c_str());
    } else if (isCaptiveProbe(r.path)) {
        // Before the password check: a probe has no credentials to offer
        // and its whole job is to open the sign-in sheet.
        sendRedirect(c, _portalUrl.c_str());
    } else if (Net.config().hasPassword() && !authorised(r)) {
        sendStatus(c, 401, JSON_TYPE, "{\"error\":\"unauthorised\"}",
                   "WWW-Authenticate: Basic realm=\"miniWorld\"\r\n");
    } else if (LampWebApi::owns(r.path)) {
        sendStatus(c, LampWebApi::handle(r.method, r.path, r.body, out), JSON_TYPE, out);
    } else if (SceneWebApi::owns(r.path)) {
        sendStatus(c, SceneWebApi::handle(r.method, r.path, r.body, out), JSON_TYPE, out);
    } else if (NetWebApi::owns(r.path)) {
        sendStatus(c, NetWebApi::handle(r.method, r.path, r.body, out), JSON_TYPE, out);
    } else if (SystemWebApi::owns(r.path)) {
        sendStatus(c, SystemWebApi::handle(r.method, r.path, r.body, out), JSON_TYPE, out);
    } else if (r.path == "/" || r.path == "/index.html") {
        sendUi(c, r);
    } else {
        sendStatus(c, 404, JSON_TYPE, "{\"error\":\"not found\"}");
    }

    c.stop();

    // The reboot waits until the response has gone out and the connection
    // is closed, so the GUI sees its answer.
    if (SystemWebApi::rebootPending()) {
        delay(200);
        rp2040.reboot();
    }
}
