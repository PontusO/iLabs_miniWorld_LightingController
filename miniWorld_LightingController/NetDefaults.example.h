/*
    NetDefaults - the WiFi network a fresh board joins before anyone has
    configured it through the GUI.

    Template. Copy this file to NetDefaults.h and fill in the real network;
    NetDefaults.h is ignored by git. The values are the compile-time
    defaults of NetConfig and apply only while no /net.json exists in
    LittleFS. The first save from the GUI replaces them. Leave both empty
    ("") to make a fresh board start in portal mode instead.

    Invector Embedded Systems AB
*/

#pragma once

#define NET_DEFAULT_SSID "YourNetwork"
#define NET_DEFAULT_PASS "YourPassword"
