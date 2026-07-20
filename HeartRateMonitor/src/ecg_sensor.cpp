#include "ecg_sensor.h"

/* ================================================================
 *  Ring Buffer State
 * ================================================================ */

volatile uint16_t ring_buf[RING_BUF_SIZE] = {};
volatile int      ring_write_idx  = 0;
volatile int      ring_read_idx   = 0;
volatile uint32_t sample_timestamp_ms = 0;

SimplePeakDetector peak_detector;

/* ================================================================
 *  FreeRTOS Sampling Task — 250 Hz
 *
 *  Runs on Core 1. Uses vTaskDelayUntil() for precise 4ms period.
 *  analogRead() is SAFE here because we're in task context, not ISR.
 * ================================================================ */

static TaskHandle_t sample_task_handle = nullptr;

static void ecg_sample_task(void *param) {
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(SAMPLE_INTERVAL_MS);

    uint32_t sample_count = 0;
    uint32_t dbg_last = 0;

    while (true) {
        vTaskDelayUntil(&last_wake, period);

        // Read ADC — safe in task context
        uint16_t val = (uint16_t)analogRead(ADC_PIN);

        // Write ring buffer
        ring_buf[ring_write_idx] = val;
        ring_write_idx = (ring_write_idx + 1) % RING_BUF_SIZE;

        uint32_t now = millis();
        sample_timestamp_ms = now;

        // Feed lightweight peak detector (Path A)
        peak_detector.feed(val, now);

        sample_count++;

#if ECG_DEBUG
        // Print debug info every 2 seconds
        if (now - dbg_last >= 2000) {
            dbg_last = now;
            bool lo_p = (digitalRead(LO_PLUS_PIN) == LOW);
            bool lo_n = (digitalRead(LO_MINUS_PIN) == LOW);
            Serial.printf("[ECG] ADC=%4u  BPM_ref=%d  LO+=%d LO-=%d  "
                          "samples=%u ring_avail=%d\n",
                          val, peak_detector.getBPM(),
                          lo_p, lo_n,
                          sample_count, ring_available());
        }
#endif
    }
}

/* ================================================================
 *  Initialization
 * ================================================================ */

void ecg_sensor_init() {
    // --- ADC ---
    analogReadResolution(12);           // 12-bit = 0..4095
    analogSetAttenuation(ADC_11db);     // ~3.3V full-scale range

    // Sanity check: read ADC a few times and print
    Serial.println("[ECG] ADC self-test (should read ~1500-2500 with "
                   "AD8232 powered):");
    for (int i = 0; i < 5; i++) {
        int v = analogRead(ADC_PIN);
        Serial.printf("  read #%d: %d\n", i + 1, v);
        delay(2);
    }

    // --- Lead-off GPIOs ---
    // AD8232 LO+/LO-: LOW = leads connected, HIGH = disconnected
    // Some modules have open-drain outputs → use INPUT_PULLUP
    pinMode(LO_PLUS_PIN, INPUT_PULLUP);
    pinMode(LO_MINUS_PIN, INPUT_PULLUP);

    bool lo_p = (digitalRead(LO_PLUS_PIN) == LOW);
    bool lo_n = (digitalRead(LO_MINUS_PIN) == LOW);
    Serial.printf("[ECG] Lead-off pins initial state: LO+=%d LO-=%d "
                  "(1=connected, 0=disconnected)\n", lo_p, lo_n);
    Serial.printf("[ECG] Leads: %s\n",
                  (lo_p && lo_n) ? "ALL CONNECTED" : "DETACHED!");

    // --- Start Sampling Task ---
    // High priority on Core 1 to minimize sampling jitter
    BaseType_t ret = xTaskCreatePinnedToCore(
        ecg_sample_task,       // function
        "ecg_sample",          // name
        4096,                  // stack size (bytes)
        nullptr,               // parameters
        configMAX_PRIORITIES - 2, // priority (high)
        &sample_task_handle,   // task handle
        1                      // core ID (1 = app core)
    );

    if (ret != pdPASS) {
        Serial.println("[ECG] FATAL: Failed to create sampling task!");
    } else {
        Serial.printf("[ECG] Sampling task started @ %d Hz on Core 1\n",
                      SAMPLE_RATE_HZ);
    }
}

/* ================================================================
 *  SimplePeakDetector Implementation
 * ================================================================ */

bool SimplePeakDetector::feed(uint16_t adc_value, uint32_t now_ms) {
    if (now_ms - last_peak_ms_ < REFRACTORY_MS) {
        noise_peak_ = (1.0f - EMA_ALPHA) * noise_peak_ + EMA_ALPHA * adc_value;
        return false;
    }

    float threshold = SIGNAL_WEIGHT * signal_peak_ + NOISE_WEIGHT * noise_peak_;
    if (threshold < 50.0f) threshold = 50.0f;

    if (adc_value > threshold) {
        signal_peak_ = (1.0f - EMA_ALPHA) * signal_peak_ + EMA_ALPHA * adc_value;

        if (prev_peak_ms_ != 0) {
            uint16_t rr = now_ms - prev_peak_ms_;
            rr_buf_[rr_idx_ % 8] = rr;
            rr_idx_++;
            if (rr_count_ < 8) rr_count_++;

            if (rr_count_ >= 2) {
                float sum = 0;
                int n = rr_count_ < 8 ? rr_count_ : 8;
                for (int i = 0; i < n; i++) sum += rr_buf_[i];
                bpm_ = (int)(60000.0f / (sum / n));
            }
        }

        prev_peak_ms_ = now_ms;
        last_peak_ms_ = now_ms;
        return true;
    } else {
        noise_peak_ = (1.0f - EMA_ALPHA) * noise_peak_ + EMA_ALPHA * adc_value;
        return false;
    }
}
