// Template for include/wifi_default.h, which is not in version control.
//
//     cp include/wifi_default.example.h include/wifi_default.h
//
// then fill in the network you want a freshly flashed board to join. Leave the
// file out entirely and the firmware behaves as it always has: no credentials,
// so the setup AP is the only way in.
//
// For an open network, define the SSID and leave the password empty.
#pragma once

#define EASYPILL_DEFAULT_WIFI_SSID "your-network"
#define EASYPILL_DEFAULT_WIFI_PASS "your-password"
