/*
    FirmwareUpdate - a firmware image uploaded over HTTP into LittleFS,
                     verified, then handed to the arduino-pico OTA boot
                     stage at the reboot that follows.

    Routes, spec docs/superpowers/specs/2026-09-23-firmware-update-design.md:

      GET  /api/system/firmware   {"fsTotal":N,"fsFree":N,"maxSize":N,
                                   "minSize":N,"staged":bool}
      POST /api/system/firmware   body: the raw .bin (application/octet-stream)
                                  headers: X-Firmware-MD5 (32 hex digits over
                                  the body), X-Firmware-Build (the image's
                                  MINIWORLD_BUILD string)
        200 {"ok":true,"size":N,"md5":"..","build":".."}  staged; reboot follows
        400 missing X-Firmware-MD5 | missing X-Firmware-Build
        409 upload in progress
        413 payload too large (Content-Length outside the band; the server
            answers this from the headers, begin() repeats it as a guard)
        422 md5 mismatch | not this sketch | build stamp not in image | short body
        500 filesystem write failed | commit failed

    The GET is a plain (method, path, body) handler like every other web
    API. The POST is streamed by HttpServer through begin(), write() and
    end(), because a 248 kB image cannot live in a String; this class sees
    buffers only, never the client.

    How it works: the body is written to LittleFS as "firmware.bin" (that
    exact name, the one the core's Updater uses and the boot stage opens)
    while an MD5 accumulates. end() compares the MD5 with the header,
    rescans the file for the boot banner format string and the given build
    stamp, which is tools/checkimage.sh's identity test run against the
    bytes that will be flashed, and only then writes the OTA command page
    through PicoOTA and asks SystemWebApi for the reboot. The boot stage
    (lib/rp2040/ota.o, in every image) copies the file into application
    flash and erases the command. It does not erase the file, so
    cleanupAtBoot() does.

    Anything short of a 200 removes the file. There is no rollback: an
    image that passes every check but breaks the network needs USB.

    Dependencies: LittleFS, MD5Builder and PicoOTA from the core,
    ArduinoJson, SystemWebApi for the reboot flag.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>

class FirmwareUpdate {
public:
    static bool owns(const String &path) { return path == "/api/system/firmware"; }

    // The GET. Any other method is 405.
    static int handle(const String &method, const String &path,
                      const String &requestBody, String &body);

    // The band a Content-Length must fall in. maxSize() asks LittleFS for
    // its free space, one filesystem call.
    static size_t minSize();
    static size_t maxSize();

    // The streaming POST, driven by HttpServer. begin() returns 0 to go
    // on, or an HTTP code with body filled. write() returns false once a
    // write has failed; end() then answers 500. end() returns 200, 422 or
    // 500 with body filled. abort() drops a partial upload with no reply.
    static int begin(size_t length, const String &md5, const String &build, String &body);
    static bool write(const uint8_t *data, size_t len);
    static int end(String &body);
    static void abort();

    // Removes a leftover firmware.bin: the image just copied, or a half
    // upload that never got a command. Call once from setup().
    static void cleanupAtBoot();
};
