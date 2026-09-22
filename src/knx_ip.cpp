#include "knx_ip.h"

#include <stdio.h>
#include <string.h>

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

#ifndef KNX_ENABLE
#define KNX_ENABLE 1
#endif
#ifndef KNX_DEBUG
#define KNX_DEBUG 0
#endif

#if KNX_ENABLE

// ---------------------------------------------------------------------
// Group address helper: (main,middle,sub) -> 15-bit group value,
// e.g. 2/1/1 -> 0x1101 (bytes 0x11 0x01), 2/0/1 -> 0x1001.
// The "group" flag is carried in cEMI control field 1 (bit 7 = 0x80 in
// 0xBC), NOT in the destination address - setting bit 15 here would turn
// 2/1/x into 18/1/x.
static uint16_t ga(uint8_t m, uint8_t mid, uint8_t s) {
    return (uint16_t)(((m & 0x1F) << 11) | ((mid & 0x07) << 8) | (s & 0xFF));
}

// Free client individual address (15.15.250, "not yet configured" range).
static const uint16_t SRC_ADDR = KNX_MY_ADDRESS;

// ---------------------------------------------------------------------
// Configured KNX devices (toggle + status group per device).
struct DevCfg {
    uint16_t toggleGA;
    uint16_t statusGA;
    uint8_t  tMid, tSub;   // for log messages
    uint8_t  sMid, sSub;
};
static const DevCfg kDevs[] = {
    { ga(KNX_DEVICE1_TOGGLE_MAIN, KNX_DEVICE1_TOGGLE_MIDDLE,
         KNX_DEVICE1_TOGGLE_SUB),
      ga(KNX_DEVICE1_STATUS_MAIN, KNX_DEVICE1_STATUS_MIDDLE,
         KNX_DEVICE1_STATUS_SUB),
      KNX_DEVICE1_TOGGLE_MIDDLE, KNX_DEVICE1_TOGGLE_SUB,
      KNX_DEVICE1_STATUS_MIDDLE, KNX_DEVICE1_STATUS_SUB },
    { ga(KNX_DEVICE2_TOGGLE_MAIN, KNX_DEVICE2_TOGGLE_MIDDLE,
         KNX_DEVICE2_TOGGLE_SUB),
      ga(KNX_DEVICE2_STATUS_MAIN, KNX_DEVICE2_STATUS_MIDDLE,
         KNX_DEVICE2_STATUS_SUB),
      KNX_DEVICE2_TOGGLE_MIDDLE, KNX_DEVICE2_TOGGLE_SUB,
      KNX_DEVICE2_STATUS_MIDDLE, KNX_DEVICE2_STATUS_SUB },
    { ga(KNX_DEVICE3_TOGGLE_MAIN, KNX_DEVICE3_TOGGLE_MIDDLE,
         KNX_DEVICE3_TOGGLE_SUB),
      ga(KNX_DEVICE3_STATUS_MAIN, KNX_DEVICE3_STATUS_MIDDLE,
         KNX_DEVICE3_STATUS_SUB),
      KNX_DEVICE3_TOGGLE_MIDDLE, KNX_DEVICE3_TOGGLE_SUB,
      KNX_DEVICE3_STATUS_MIDDLE, KNX_DEVICE3_STATUS_SUB },
    { ga(KNX_DEVICE4_TOGGLE_MAIN, KNX_DEVICE4_TOGGLE_MIDDLE,
         KNX_DEVICE4_TOGGLE_SUB),
      ga(KNX_DEVICE4_STATUS_MAIN, KNX_DEVICE4_STATUS_MIDDLE,
         KNX_DEVICE4_STATUS_SUB),
      KNX_DEVICE4_TOGGLE_MIDDLE, KNX_DEVICE4_TOGGLE_SUB,
      KNX_DEVICE4_STATUS_MIDDLE, KNX_DEVICE4_STATUS_SUB },
    { ga(KNX_DEVICE5_TOGGLE_MAIN, KNX_DEVICE5_TOGGLE_MIDDLE,
         KNX_DEVICE5_TOGGLE_SUB),
      ga(KNX_DEVICE5_STATUS_MAIN, KNX_DEVICE5_STATUS_MIDDLE,
         KNX_DEVICE5_STATUS_SUB),
      KNX_DEVICE5_TOGGLE_MIDDLE, KNX_DEVICE5_TOGGLE_SUB,
      KNX_DEVICE5_STATUS_MIDDLE, KNX_DEVICE5_STATUS_SUB },
};
static const int kDevCount = (int)(sizeof kDevs / sizeof kDevs[0]);

static IPAddress s_host;          // KNX IP interface (from KNX_HOST)
static WiFiUDP   s_udp;
static bool      s_sock = false;  // local UDP socket open

enum KnxState : uint8_t { KS_DOWN, KS_CONNECTING, KS_READY };
static KnxState s_state = KS_DOWN;
static uint8_t  s_channel = 0;
static uint8_t  s_seq     = 0;
static uint32_t s_t0      = 0;    // connect attempt / heartbeat timestamp
static uint32_t s_lastRx  = 0;    // last incoming packet (watchdog)
static uint32_t s_retryAt = 0;    // don't reconnect before this time
static bool     s_statusAsk[kDevCount];   // initial status read sent
static bool     s_toggleReq[kDevCount];   // a toggle was requested by the UI
// last value written per device - fallback used while the status GA has not
// reported yet, so successive clicks still alternate on/off
static bool     s_lastCmd[kDevCount];
static bool     s_cmdKnown[kDevCount];

// per-device state (UI reads under s_mux)
static SemaphoreHandle_t s_mux = nullptr;
static bool s_ampOn[kDevCount];
static bool s_ampValid[kDevCount];

static void setAmp(int dev, bool on) {
    if (!s_mux || dev < 0 || dev >= kDevCount) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_ampOn[dev]    = on;
    s_ampValid[dev] = true;
    xSemaphoreGive(s_mux);
}
static void clearAmp(int dev) {
    if (!s_mux || dev < 0 || dev >= kDevCount) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_ampValid[dev] = false;
    xSemaphoreGive(s_mux);
}
// Remember a state we commanded ourselves, so the play menu reacts to
// knobs even when the status group never reports back.
static void commandAmp(int dev, bool on) {
    s_lastCmd[dev]  = on;
    s_cmdKnown[dev] = true;
    setAmp(dev, on);
}
bool knxAmpIsOn(int dev) {
    if (!s_mux || dev < 0 || dev >= kDevCount) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool v = s_ampOn[dev];
    xSemaphoreGive(s_mux);
    return v;
}
bool knxAmpIsValid(int dev) {
    if (!s_mux || dev < 0 || dev >= kDevCount) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool v = s_ampValid[dev];
    xSemaphoreGive(s_mux);
    return v;
}
int knxDeviceCount() { return kDevCount; }

const char* knxDeviceName(int dev) {
    switch (dev) {
        case 0: return KNX_DEVICE1_NAME;
        case 1: return KNX_DEVICE2_NAME;
        case 2: return KNX_DEVICE3_NAME;
        case 3: return KNX_DEVICE4_NAME;
        case 4: return KNX_DEVICE5_NAME;
        default: return "";
    }
}

// Devices 0..knxPlayDevCount()-1 are pinned to the play menu; the rest are
// listed in the KNX submenu.
static int knxPlayDevCount() {
    return (KNX_PLAYMENU_DEVICES < kDevCount) ? KNX_PLAYMENU_DEVICES
                                              : kDevCount;
}
int knxMenuDeviceBase() { return knxPlayDevCount(); }
int knxMenuDeviceCount() { return kDevCount - knxPlayDevCount(); }

// ---------------------------------------------------------------------
static void sendPkt(const uint8_t* p, size_t n) {
    if (!s_sock) return;
    s_udp.beginPacket(s_host, KNX_PORT);
    s_udp.write(p, n);
    s_udp.endPacket();
}

static void dbgFrame(const char* tag, const uint8_t* p, size_t n) {
#if KNX_DEBUG
    Serial.printf("[knx] %s:", tag);
    for (size_t i = 0; i < n; ++i) Serial.printf(" %02X", p[i]);
    Serial.println();
#else
    (void)tag; (void)p; (void)n;
#endif
}

static void sendTunnelling(uint16_t ga_, const uint8_t tpdu[2]) {
    uint8_t p[28];
    size_t  o = 0;
    p[o++] = 0x06; p[o++] = 0x10;        // KNXnet/IP header
    p[o++] = 0x04; p[o++] = 0x20;        // TUNNELING_REQUEST
    p[o++] = 0;     p[o++] = 0;          // total length (filled below)
    p[o++] = 0x04;                       // connection header structure len
    p[o++] = s_channel;
    p[o++] = s_seq++;                    // sequence counter (wraps)
    p[o++] = 0x00;                       // reserved
    p[o++] = 0x11;                       // L_Data.req
    p[o++] = 0x00;                       // additional info length
    p[o++] = 0xBC;                       // control field 1 (group flag)
    p[o++] = 0xE0;                       // control field 2
    p[o++] = (uint8_t)(SRC_ADDR >> 8);
    p[o++] = (uint8_t)(SRC_ADDR & 0xFF);
    p[o++] = (uint8_t)(ga_ >> 8);
    p[o++] = (uint8_t)(ga_ & 0xFF);
    p[o++] = 0x01;                       // NPDU length (1 data octet)
    p[o++] = tpdu[0];                    // TPCI/APCI octet
    p[o++] = tpdu[1];                    // value octet
    p[4] = (uint8_t)(o >> 8);
    p[5] = (uint8_t)(o & 0xFF);
    dbgFrame("tx", p, o);
    sendPkt(p, o);
}

// 1-bit group writes: GroupValueWrite 0x0080 | value, TPDU [0x00, 0x80|v]
static void sendGroupWrite(uint16_t ga_, bool on) {
    uint8_t tpdu[2] = {0x00, (uint8_t)(0x80 | (on ? 1 : 0))};
    sendTunnelling(ga_, tpdu);
}

static void sendGroupRead(uint16_t ga_) {
    uint8_t tpdu[2] = {0x00, 0x00};      // GroupValueRead (0x0000)
    sendTunnelling(ga_, tpdu);
}

static void sendConnectRequest() {
    uint8_t p[26];
    size_t  o = 0;
    p[o++] = 0x06; p[o++] = 0x10;
    p[o++] = 0x02; p[o++] = 0x05;        // CONNECT_REQUEST
    p[o++] = 0x00; p[o++] = 0x1A;        // total length 26
    // control endpoint HPAI - route back to the request's source
    p[o++] = 0x08; p[o++] = 0x01;
    p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00;
    p[o++] = 0x00; p[o++] = 0x00;
    // data endpoint HPAI - same
    p[o++] = 0x08; p[o++] = 0x01;
    p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00;
    p[o++] = 0x00; p[o++] = 0x00;
    // CRI: length, TUNNEL_CONNECTION, DATA_LINK_LAYER, reserved
    p[o++] = 0x04; p[o++] = 0x04; p[o++] = 0x02; p[o++] = 0x00;
    sendPkt(p, o);
}

static void sendDisconnectRequest() {
    uint8_t p[16];
    size_t  o = 0;
    p[o++] = 0x06; p[o++] = 0x10;
    p[o++] = 0x02; p[o++] = 0x09;        // DISCONNECT_REQUEST
    p[o++] = 0x00; p[o++] = 0x10;        // total length 16
    p[o++] = s_channel;
    p[o++] = 0x00;                       // reserved
    p[o++] = 0x08; p[o++] = 0x01;        // control endpoint HPAI (route-back)
    p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00;
    p[o++] = 0x00; p[o++] = 0x00;
    sendPkt(p, o);
}

static void sendDisconnectResponse() {
    uint8_t p[10];
    p[0] = 0x06; p[1] = 0x10;
    p[2] = 0x02; p[3] = 0x0A;            // DISCONNECT_RESPONSE
    p[4] = 0x00; p[5] = 0x0A;
    p[6] = s_channel; p[7] = 0x00;       // status ok
    p[8] = 0x00; p[9] = 0x00;            // reserved
    sendPkt(p, 10);
}

static void sendConnectionstateResponse() {
    uint8_t p[10];
    p[0] = 0x06; p[1] = 0x10;
    p[2] = 0x02; p[3] = 0x08;            // CONNECTIONSTATE_RESPONSE
    p[4] = 0x00; p[5] = 0x0A;
    p[6] = s_channel; p[7] = 0x00;       // status ok
    p[8] = 0x00; p[9] = 0x00;            // reserved
    sendPkt(p, 10);
}

static void sendConnectionstateRequest() {
    uint8_t p[16];
    size_t  o = 0;
    p[o++] = 0x06; p[o++] = 0x10;
    p[o++] = 0x02; p[o++] = 0x07;        // CONNECTIONSTATE_REQUEST
    p[o++] = 0x00; p[o++] = 0x10;        // total length 16
    p[o++] = s_channel;
    p[o++] = 0x00;                       // reserved
    p[o++] = 0x08; p[o++] = 0x01;        // control endpoint HPAI (route-back)
    p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00;
    p[o++] = 0x00; p[o++] = 0x00;
    sendPkt(p, o);
}

static void sendTunnellingAck(uint8_t seq) {
    uint8_t p[10];
    p[0] = 0x06; p[1] = 0x10;
    p[2] = 0x04; p[3] = 0x21;            // TUNNELING_ACK
    p[4] = 0x00; p[5] = 0x0A;
    p[6] = 0x04; p[7] = s_channel; p[8] = seq; p[9] = 0x00;
    sendPkt(p, 10);
}

// ---------------------------------------------------------------------
static void closeDown();

// ---------------------------------------------------------------------
// Parse a single cEMI L_Data frame (indication or confirmation) that the
// interface forwarded to us and track the status group addresses.
static void parseLData(const uint8_t* p, int len) {
    if (len < 10) return;
    if (p[0] != 0x29 && p[0] != 0x2E) return;   // L_Data.ind / L_Data.con
    int o = 2 + p[1];                            // skip msg code + addinfo
    if (o + 8 > len) return;

    uint16_t dst   = (uint16_t)((p[o + 4] << 8) | p[o + 5]);
    uint8_t  nplen = p[o + 6];
    (void)nplen;                                 // see note below
    int o2 = o + 7;
    if (o2 >= len) return;
    // The NPDU length octet is unreliable for the octet count: ETS/XKNX
    // emit two TPDU octets (TPCI/APCI + value) but count only the value
    // octet (length=1).  Decode from the bytes actually present instead.
    int nData = len - o2;
    if (nData < 1) return;

    int dev = -1;
    for (int i = 0; i < kDevCount; ++i)
        if (dst == kDevs[i].statusGA) { dev = i; break; }
    if (dev < 0) return;

    // Decode the 10-bit APCI + value.  Accept both TPDU shapes used on the
    // wire: the "short" single-octet form (0x80|v) and the two-octet form
    // ([0x00, 0x80|v]) used by XKNX / ETS.
    uint8_t v0 = p[o2];
    uint8_t svc;
    uint8_t val;
    if (nData >= 2) {
        svc = (uint8_t)(((v0 & 0x03) << 8) | (p[o2 + 1] & 0xC0));
        val = p[o2 + 1] & 0x3F;
    } else {
        svc = v0 & 0xC0;                   // high 2 bits carry the APCI
        val = v0 & 0x3F;
    }
    if (svc != 0x80 && svc != 0x40) return;     // GroupValueWrite / Response

    bool on = (val & 1) != 0;
    setAmp(dev, on);
#if KNX_DEBUG
    Serial.printf("[knx] dev%d status 2/%u/%u = %s (src %02X%02X)\n",
                  dev + 1, kDevs[dev].sMid, kDevs[dev].sSub, on ? "on" : "off",
                  p[o + 2], p[o + 3]);
#endif
}

static void processIncoming() {
    while (s_udp.parsePacket() > 0) {
        uint8_t b[160];
        int     n = s_udp.read(b, sizeof b);
        if (n < 6) continue;
        s_lastRx = millis();
        uint16_t svc = (uint16_t)((b[2] << 8) | b[3]);
        switch (svc) {
        case 0x0206: {                     // CONNECT_RESPONSE
            if (n < 8) break;
            uint8_t status = b[7];
            if (status == 0) {
                s_channel = b[6];
                s_seq     = 0;
                for (int i = 0; i < kDevCount; ++i) s_statusAsk[i] = false;
                s_t0      = millis();       // heartbeat timer starts now
                s_state   = KS_READY;
                s_lastRx  = millis();
                Serial.printf("[knx] tunneling connected (channel %u)\n",
                              s_channel);
            } else {
                Serial.printf("[knx] connect refused (status 0x%02X, %s); "
                              "retrying in 5 s\n", status,
                              status == 0x24 ? "no free connection slots" :
                              status == 0x2E ? "source address in use" :
                              status == 0x29 ? "tunnelling layer unavailable" :
                              "see KNXnet/IP spec");
                closeDown();
            }
            break;
        }
        case 0x0420: {                     // TUNNELING_REQUEST (bus->us)
            if (n < 14) break;
            uint8_t ch = b[7];
            uint8_t sq = b[8];
            if (ch == s_channel) {
                sendTunnellingAck(sq);
                parseLData(b + 10, n - 10);
            }
            break;
        }
        case 0x0207:                       // CONNECTIONSTATE_REQUEST
            sendConnectionstateResponse();
            break;
        case 0x0209:                       // DISCONNECT_REQUEST
            sendDisconnectResponse();
            closeDown();
            break;
        default:                           // 0x0421 ack, 0x0208 resp, ...: ignore
            break;
        }
    }
}

// ---------------------------------------------------------------------
static void closeDown() {
    for (int i = 0; i < kDevCount; ++i) {
        s_cmdKnown[i] = false;
        s_lastCmd[i]  = true;              // first toggle after re-connect = on
        s_toggleReq[i] = false;
        clearAmp(i);
    }
    if (s_sock && s_state != KS_DOWN) sendDisconnectRequest();
    s_udp.stop();
    s_sock   = false;
    s_state  = KS_DOWN;
    s_channel = 0;
    s_seq     = 0;
    s_retryAt = millis() + 5000;   // don't hammer the interface on refusal
}

// ---------------------------------------------------------------------
void knxSetup() {
    if (s_mux == nullptr) s_mux = xSemaphoreCreateMutex();
    unsigned int ip[4] = {0, 0, 0, 0};
    if (sscanf(KNX_HOST, "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]) == 4 &&
        ip[0] <= 255 && ip[1] <= 255 && ip[2] <= 255 && ip[3] <= 255)
        s_host = IPAddress((uint8_t)ip[0], (uint8_t)ip[1], (uint8_t)ip[2],
                           (uint8_t)ip[3]);
    for (int i = 0; i < kDevCount; ++i)
        Serial.printf("[knx] dev%d: toggle 2/%u/%u <- status 2/%u/%u\n",
                      i + 1, kDevs[i].tMid, kDevs[i].tSub,
                      kDevs[i].sMid, kDevs[i].sSub);
    Serial.printf("[knx] iface %s:%u, %d device(s)\n", KNX_HOST, KNX_PORT,
                  kDevCount);
}

void knxLoop() {
    switch (s_state) {
    case KS_DOWN:
        if ((int32_t)(millis() - s_retryAt) < 0) break;  // wait out the delay
        if (!s_sock) {
            if (!s_udp.begin(KNX_LOCAL_PORT)) break;
            s_sock = true;
        }
        sendConnectRequest();
        s_t0   = millis();
        s_state = KS_CONNECTING;
        break;
    case KS_CONNECTING:
        processIncoming();
        if (millis() - s_t0 > 3000) {
            Serial.println("[knx] connect timeout - retrying");
            closeDown();
        }
        break;
    case KS_READY:
        processIncoming();
        // Heartbeat: ask the interface every 30 s whether the tunnel is
        // alive (it answers with a CONNECTIONSTATE_RESPONSE, which also
        // feeds the dead-tunnel watchdog below).
        if (millis() - s_t0 >= 30000) {
            sendConnectionstateRequest();
            s_t0 = millis();
        }
        if (millis() - s_lastRx >= 90000) {
            Serial.println("[knx] tunnel silent - reconnecting");
            closeDown();
            break;
        }
        // pick up the current state of every device once per connection
        for (int i = 0; i < kDevCount; ++i)
            if (!s_statusAsk[i]) {
                s_statusAsk[i] = true;
                sendGroupRead(kDevs[i].statusGA);
            }
        // handle toggle requests
        for (int i = 0; i < kDevCount; ++i) {
            if (!s_toggleReq[i]) continue;
            s_toggleReq[i] = false;
            bool on;
            if (knxAmpIsValid(i)) {
                on = !knxAmpIsOn(i);   // flip the known bus state
            } else {
                // No status report (yet): alternate from the last value we
                // commanded, defaulting to ON for the very first toggle.
                // Ask the bus for the real state in parallel for the UI.
                on = s_cmdKnown[i] ? !s_lastCmd[i] : true;
                sendGroupRead(kDevs[i].statusGA);
                s_statusAsk[i] = true;
            }
            commandAmp(i, on);         // optimistic, echoed/updated later
            sendGroupWrite(kDevs[i].toggleGA, on);
#if KNX_DEBUG
            Serial.printf("[knx] dev%d toggle 2/%u/%u -> %s\n",
                          i + 1, kDevs[i].tMid, kDevs[i].tSub,
                          on ? "on" : "off");
#endif
        }
        break;
    }
}

void knxReset() { closeDown(); }

void knxToggle(int dev) {
    if (dev >= 0 && dev < kDevCount) s_toggleReq[dev] = true;
}

#else   // !KNX_ENABLE - stubs

void knxSetup() {}
void knxLoop() {}
void knxReset() {}
void knxToggle(int) {}
bool knxAmpIsOn(int) { return false; }
bool knxAmpIsValid(int) { return false; }
int knxDeviceCount() { return 0; }
const char* knxDeviceName(int) { return ""; }
int knxMenuDeviceBase() { return 0; }
int knxMenuDeviceCount() { return 0; }

#endif