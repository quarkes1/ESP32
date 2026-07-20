#include "web_server.h"
#include "ecg_sensor.h"
#include "index_html.h"

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Ticker.h>

/* ================================================================
 *  WiFi AP Configuration
 * ================================================================ */

static const char *AP_SSID = "ECG-Monitor";
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GW(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

/* ================================================================
 *  AsyncWebServer & WebSocket
 * ================================================================ */

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

/* ================================================================
 *  WebSocket Event Handler
 * ================================================================ */

static void on_ws_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
    case WS_EVT_CONNECT:
        Serial.printf("[WS] Client #%u connected\n", client->id());
        break;
    case WS_EVT_DISCONNECT:
        Serial.printf("[WS] Client #%u disconnected\n", client->id());
        break;
    case WS_EVT_DATA:
        // We don't expect data from clients, but handle gracefully
        break;
    case WS_EVT_ERROR:
        Serial.printf("[WS] Client #%u error\n", client->id());
        break;
    default:
        break;
    }
}

/* ================================================================
 *  Periodic Push Task — fires every 50ms
 * ================================================================ */

static Ticker ws_ticker;

static void push_ecg_data() {
    // Skip if no clients connected
    if (ws.count() == 0) return;

    uint32_t now = sample_timestamp_ms;
    int avail = ring_available();

    // Read up to WS_BATCH_SIZE samples from ring buffer
    // Peak detection (Path A) already done in the 250Hz sampling task
    uint16_t samples[WS_BATCH_SIZE];
    int count = 0;
    for (int i = 0; i < WS_BATCH_SIZE && avail > 0; i++, avail--) {
        samples[i] = ring_buf[ring_read_idx];
        ring_read_idx = (ring_read_idx + 1) % RING_BUF_SIZE;
        count++;
    }

    if (count == 0) return; // nothing new

    // Build JSON
    // {"t":12345,"samples":[2048,...],"bpm_ref":72,"lead":true}
    static char json[512];
    int pos = snprintf(json, sizeof(json),
                       R"({"t":%u,"samples":[)", now);
    for (int i = 0; i < count; i++) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "%s%u", (i > 0 ? "," : ""), samples[i]);
    }
    pos += snprintf(json + pos, sizeof(json) - pos,
                    R"(],"bpm_ref":%d,"lead":%s})",
                    peak_detector.getBPM(),
                    ecg_lead_ok() ? "true" : "false");

    ws.textAll(json);
}

/* ================================================================
 *  Initialization
 * ================================================================ */

void web_server_init() {
    // --- WiFi AP ---
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
    bool ok = WiFi.softAP(AP_SSID);
    // ^ no password = open network

    if (ok) {
        Serial.printf("[WiFi] AP '%s' started, IP: %s\n",
                      AP_SSID, WiFi.softAPIP().toString().c_str());
    } else {
        Serial.println("[WiFi] AP start FAILED!");
    }

    // --- WebSocket ---
    ws.onEvent(on_ws_event);
    server.addHandler(&ws);

    // --- HTTP Routes ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", index_html);
    });

    // --- Start Server ---
    server.begin();
    Serial.println("[HTTP] Server started on port 80");

    // --- Start Periodic Push (50ms interval) ---
    ws_ticker.attach_ms(50, push_ecg_data);
    Serial.println("[WS] Push ticker started @ 50ms");
}
