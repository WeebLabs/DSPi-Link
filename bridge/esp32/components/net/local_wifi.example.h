/*
 * local_wifi.example.h - TEMPORARY, phase 3 only.
 *
 * Until the setup page arrives in phase 4, the bridge joins the network
 * named here. Copy this file to local_wifi.h in the same folder and fill it
 * in. local_wifi.h is git-ignored, so your password never reaches the
 * repository. If the firmware was built before the file existed, touch
 * net.c so the build notices it.
 */

#define LOCAL_WIFI_SSID "Your network"
#define LOCAL_WIFI_PASSWORD "your password"
