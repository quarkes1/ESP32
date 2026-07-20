#ifndef ECG_SENSOR_H
#define ECG_SENSOR_H

#include <Arduino.h>

/* ================================================================
 *  Constants
 * ================================================================ */

constexpr int SAMPLE_RATE_HZ      = 250;       // ADC sampling rate
constexpr int SAMPLE_INTERVAL_MS  = 4;         // 1 / 250 Hz
constexpr int RING_BUF_SIZE       = 256;       // ring buffer holds ~1 second
constexpr int WS_BATCH_SIZE       = 13;        // samples per 50ms WebSocket push

// ADC pin — MUST be ADC1 (GPIO1-10) to avoid WiFi interference
constexpr int ADC_PIN             = 4;         // GPIO4 = ADC1_CH3
constexpr int LO_PLUS_PIN         = 15;        // lead-off detection +
constexpr int LO_MINUS_PIN        = 16;        // lead-off detection -

// Debug: set to 1 to print raw ADC + lead-off state to serial
#define ECG_DEBUG 1

/* ================================================================
 *  Ring Buffer — written by sampling task, read by push task
 * ================================================================ */

extern volatile uint16_t ring_buf[RING_BUF_SIZE];
extern volatile int      ring_write_idx;
extern volatile int      ring_read_idx;
extern volatile uint32_t sample_timestamp_ms;

inline int ring_available() {
    int avail = ring_write_idx - ring_read_idx;
    if (avail < 0) avail += RING_BUF_SIZE;
    return avail;
}

/* ================================================================
 *  Simple Peak Detector — lightweight R-peak detection (Path A)
 * ================================================================ */

class SimplePeakDetector {
public:
    bool feed(uint16_t adc_value, uint32_t now_ms);
    int getBPM() const { return bpm_; }

private:
    static constexpr uint32_t REFRACTORY_MS = 200;
    static constexpr float    SIGNAL_WEIGHT = 0.3f;
    static constexpr float    NOISE_WEIGHT  = 0.7f;
    static constexpr float    EMA_ALPHA     = 0.125f;

    float    signal_peak_  = 500.0f;
    float    noise_peak_   = 100.0f;
    uint32_t last_peak_ms_ = 0;
    int      bpm_          = 0;

    uint16_t rr_buf_[8] = {};
    int      rr_idx_     = 0;
    int      rr_count_   = 0;
    uint32_t prev_peak_ms_ = 0;
};

extern SimplePeakDetector peak_detector;

/* ================================================================
 *  Public API
 * ================================================================ */

// Start ADC sampling task + lead-off GPIOs. Call once in setup().
void ecg_sensor_init();

// Read lead-off status (true = leads connected)
inline bool ecg_lead_ok() {
    return (digitalRead(LO_PLUS_PIN) == LOW) && (digitalRead(LO_MINUS_PIN) == LOW);
}

#endif // ECG_SENSOR_H
