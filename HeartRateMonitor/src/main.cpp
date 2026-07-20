#include <Arduino.h>
#include "ecg_sensor.h"
#include "web_server.h"

/* ================================================================
 *  Heart Rate Monitor — ESP32-S3 + AD8232
 *
 *  Dual-core architecture:
 *    Core 0: WiFi protocol stack + AsyncWebServer + AsyncWebSocket
 *    Core 1: Hardware timer ISR (250Hz ADC) + push task (50ms)
 *
 *  Dual-path BPM:
 *    Path A (ESP32): SimplePeakDetector → bpm_ref (lightweight)
 *    Path B (Browser): Pan-Tompkins DSP → bpm (high-precision)
 * ================================================================ */

void setup() {
    Serial.begin(115200);
    delay(500); // allow USB-CDC to enumerate
    Serial.println();
    Serial.println("========================================");
    Serial.println("  ECG Heart Rate Monitor");
    Serial.println("  ESP32-S3 + AD8232");
    Serial.println("========================================");

    // 1. Initialize ECG sensor (ADC + hardware timer + lead-off GPIOs)
    ecg_sensor_init();

    // 2. Start Wi-Fi AP + HTTP server + WebSocket push
    web_server_init();

    Serial.println("[Main] Initialization complete");
    Serial.println("[Main] Connect to Wi-Fi 'ECG-Monitor' → browse http://192.168.4.1");
}

void loop() {
    // All work is done by:
    //   - Hardware timer ISR (ADC sampling @ 250Hz)
    //   - Ticker callback (WebSocket push + peak detection @ 50ms)
    //   - AsyncWebServer callbacks

    // Periodically log status to serial
    static uint32_t last_log = 0;
    uint32_t now = millis();
    if (now - last_log >= 5000) {
        last_log = now;
        int bpm = peak_detector.getBPM();
        bool lead = ecg_lead_ok();
        Serial.printf("[Status] BPM_ref=%d  Lead=%s  Uptime=%us\n",
                      bpm, lead ? "OK" : "OFF", now / 1000);
    }

    delay(100);
}
