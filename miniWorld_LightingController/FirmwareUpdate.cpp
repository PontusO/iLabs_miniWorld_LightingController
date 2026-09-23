/*
    FirmwareUpdate - see FirmwareUpdate.h

    Invector Embedded Systems AB
*/

#include "FirmwareUpdate.h"
#include "SystemWebApi.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <MD5Builder.h>
#include <PicoOTA.h>

// The name the core's Updater stages under and the boot stage opens. No
// leading slash, deliberately: PicoOTA::addFile() opens it as given and
// puts the same string in the command page.
static const char IMAGE_FILE[] = "firmware.bin";

// The band, spec section 3.3. A build of this sketch is about 248 kB.
static const size_t MIN_SIZE  = 100000;
static const size_t MAX_SIZE  = 1048576;
static const size_t FS_MARGIN = 65536;

// What checkimage.sh greps for on the host. The literal below is also in
// the image once more, which is harmless.
static const char BANNER_FORMAT[] = "miniWorld lighting controller %s (%s)";

// The scan reads the file in pieces and carries the tail of one into the
// next, so a needle straddling a boundary is still found. OVERLAP must be
// at least one less than the longest needle: the banner is 37 bytes and a
// build string is capped at 63.
static const size_t SCAN_CHUNK   = 4096;
static const size_t SCAN_OVERLAP = 63;
static const size_t BUILD_MAX    = 64;

static File       s_file;
static MD5Builder s_md5;
static size_t     s_expected    = 0;
static size_t     s_received    = 0;
static bool       s_open        = false;
static bool       s_writeFailed = false;
static char       s_expectMd5[33];
static char       s_expectBuild[BUILD_MAX];
static uint8_t    s_scan[SCAN_CHUNK + SCAN_OVERLAP];

static void mountFs() {
    static bool begun = false;
    if (!begun) {
        LittleFS.begin();
        begun = true;
    }
}

static int reply(int code, const char *msg, String &body) {
    JsonDocument doc;
    doc["error"] = msg;
    body = "";
    serializeJson(doc, body);
    return code;
}

static bool isHex32(const String &s) {
    if (s.length() != 32) {
        return false;
    }
    for (unsigned i = 0; i < 32; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

// Closes and removes the staged file and forgets the upload.
static void discard() {
    if (s_file) {
        s_file.close();
    }
    LittleFS.remove(IMAGE_FILE);
    s_open = false;
    s_expected = s_received = 0;
}

static bool contains(const uint8_t *hay, size_t hayLen, const char *needle, size_t n) {
    if (n == 0 || hayLen < n) {
        return false;
    }
    for (size_t i = 0; i + n <= hayLen; i++) {
        if (hay[i] == (uint8_t)needle[0] && memcmp(hay + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool fileContains(File &f, const char *needle) {
    size_t n = strlen(needle);
    size_t carry = 0;
    f.seek(0);
    for (;;) {
        int got = f.read(s_scan + carry, SCAN_CHUNK);
        if (got <= 0) {
            return false;
        }
        size_t total = carry + (size_t)got;
        if (contains(s_scan, total, needle, n)) {
            return true;
        }
        carry = (total < SCAN_OVERLAP) ? total : SCAN_OVERLAP;
        memmove(s_scan, s_scan + total - carry, carry);
    }
}

size_t FirmwareUpdate::minSize() {
    return MIN_SIZE;
}

size_t FirmwareUpdate::maxSize() {
    mountFs();
    FSInfo info;
    if (!LittleFS.info(info) || info.totalBytes <= info.usedBytes) {
        return 0;
    }
    uint64_t free = info.totalBytes - info.usedBytes;
    if (free <= FS_MARGIN) {
        return 0;
    }
    free -= FS_MARGIN;
    return (free < MAX_SIZE) ? (size_t)free : MAX_SIZE;
}

int FirmwareUpdate::handle(const String &method, const String &path,
                           const String &requestBody, String &body) {
    (void)path;
    (void)requestBody;
    if (method != "GET") {
        return reply(405, "method not allowed", body);
    }
    mountFs();
    FSInfo info;
    JsonDocument doc;
    if (LittleFS.info(info)) {
        doc["fsTotal"] = (uint32_t)info.totalBytes;
        doc["fsFree"]  = (uint32_t)(info.totalBytes > info.usedBytes
                                    ? info.totalBytes - info.usedBytes : 0);
    } else {
        doc["fsTotal"] = 0;
        doc["fsFree"]  = 0;
    }
    doc["maxSize"] = (uint32_t)maxSize();
    doc["minSize"] = (uint32_t)MIN_SIZE;
    doc["staged"]  = LittleFS.exists(IMAGE_FILE);
    body = "";
    serializeJson(doc, body);
    return 200;
}

int FirmwareUpdate::begin(size_t length, const String &md5, const String &build, String &body) {
    if (s_open) {
        return reply(409, "upload in progress", body);
    }
    if (!isHex32(md5)) {
        return reply(400, "missing X-Firmware-MD5", body);
    }
    if (build.length() == 0) {
        return reply(400, "missing X-Firmware-Build", body);
    }
    if (build.length() >= BUILD_MAX) {
        return reply(400, "X-Firmware-Build longer than 63 characters", body);
    }
    size_t max = maxSize();
    if (length < MIN_SIZE || length > max) {
        JsonDocument doc;
        doc["error"] = "payload too large";
        doc["max"] = (uint32_t)max;
        body = "";
        serializeJson(doc, body);
        return 413;
    }
    mountFs();
    LittleFS.remove(IMAGE_FILE);
    s_file = LittleFS.open(IMAGE_FILE, "w");
    if (!s_file) {
        return reply(500, "filesystem write failed", body);
    }
    s_md5.begin();
    strncpy(s_expectMd5, md5.c_str(), 32);
    s_expectMd5[32] = 0;
    strncpy(s_expectBuild, build.c_str(), BUILD_MAX - 1);
    s_expectBuild[BUILD_MAX - 1] = 0;
    s_expected = length;
    s_received = 0;
    s_writeFailed = false;
    s_open = true;
    Serial.printf("firmware: upload of %u bytes, build %s\n", (unsigned)length, s_expectBuild);
    return 0;
}

bool FirmwareUpdate::write(const uint8_t *data, size_t len) {
    if (!s_open || s_writeFailed) {
        return false;
    }
    if (s_file.write(data, len) != len) {
        s_writeFailed = true;
        return false;
    }
    s_received += len;
    // MD5Builder::add() takes a uint16_t length. The pieces are 2 kB
    // today, but the interface takes size_t, so feed it in steps that
    // always fit instead of trusting the caller.
    while (len > 0) {
        size_t step = len > 0xFFFF ? 0xFFFF : len;
        s_md5.add(data, (uint16_t)step);
        data += step;
        len  -= step;
    }
    return true;
}

int FirmwareUpdate::end(String &body) {
    if (!s_open) {
        return reply(409, "no upload in progress", body);
    }
    if (s_writeFailed) {
        discard();
        Serial.println("firmware: rejected, filesystem write failed");
        return reply(500, "filesystem write failed", body);
    }
    s_file.close();
    if (s_received != s_expected) {
        discard();
        Serial.println("firmware: rejected, short body");
        return reply(422, "short body", body);
    }
    s_md5.calculate();
    String got = s_md5.toString();      // lower-case hex
    if (!got.equalsIgnoreCase(s_expectMd5)) {
        discard();
        Serial.println("firmware: rejected, md5 mismatch");
        return reply(422, "md5 mismatch", body);
    }
    File f = LittleFS.open(IMAGE_FILE, "r");
    if (!f) {
        discard();
        return reply(500, "filesystem write failed", body);
    }
    bool banner = fileContains(f, BANNER_FORMAT);
    bool stamp  = banner && fileContains(f, s_expectBuild);
    f.close();
    if (!banner) {
        discard();
        Serial.println("firmware: rejected, not this sketch");
        return reply(422, "not this sketch", body);
    }
    if (!stamp) {
        discard();
        Serial.println("firmware: rejected, build stamp not in image");
        return reply(422, "build stamp not in image", body);
    }
    picoOTA.begin();
    if (!picoOTA.addFile(IMAGE_FILE) || !picoOTA.commit()) {
        discard();
        // commit() opens the command file for writing before it fails,
        // so a partial page may be left; the boot stage's CRC would
        // refuse it, but it has no business staying in the filesystem.
        LittleFS.remove(_OTA_COMMAND_FILE);
        Serial.println("firmware: rejected, commit failed");
        return reply(500, "commit failed", body);
    }
    Serial.printf("firmware: staged %u bytes, build %s, rebooting into it\n",
                  (unsigned)s_received, s_expectBuild);
    JsonDocument doc;
    doc["ok"]    = true;
    doc["size"]  = (uint32_t)s_received;
    doc["md5"]   = got;
    doc["build"] = s_expectBuild;
    body = "";
    serializeJson(doc, body);
    s_open = false;
    SystemWebApi::requestReboot();
    return 200;
}

void FirmwareUpdate::abort() {
    if (s_open) {
        Serial.println("firmware: upload dropped");
    }
    discard();
}

void FirmwareUpdate::cleanupAtBoot() {
    mountFs();
    if (LittleFS.exists(IMAGE_FILE)) {
        LittleFS.remove(IMAGE_FILE);
        Serial.println("firmware: removed staged image");
    }
}
