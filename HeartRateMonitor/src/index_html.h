#ifndef INDEX_HTML_H
#define INDEX_HTML_H

#include <Arduino.h>

const char index_html[] PROGMEM = R"raw(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<title>ECG Monitor</title>
<style>
/* =============================================================
   Mobile-First Base Styles
   ============================================================= */
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  background:#0a0a0f;color:#ccc;overflow:hidden;
  display:flex;flex-direction:column;height:100vh;height:100dvh;
  -webkit-tap-highlight-color:transparent;
  user-select:none;-webkit-user-select:none;
}

/* ---- BPM Panel (mobile: top, pc: sidebar) ---- */
.bpm-panel{text-align:center;padding:12px 0 8px;flex-shrink:0}
.bpm-value{font-size:72px;font-weight:700;line-height:1;transition:color .6s}
.bpm-unit{font-size:14px;opacity:.6;margin-top:2px}
.bpm-zone{font-size:13px;margin-top:4px;transition:color .6s}
.bpm-ref{font-size:11px;opacity:.45;margin-top:2px}
.bpm-ref span{margin:0 4px}
.validation{font-size:11px;margin-top:4px;transition:color .3s}

/* ---- ECG Canvas Container ---- */
.ecg-container{
  flex:1;min-height:180px;position:relative;
  background:#0a0a0f;
  overflow:hidden;
}
.ecg-container canvas{
  display:block;
  width:100%;height:100%;
}

/* ---- Lead-off Overlay ---- */
.lead-off-overlay{
  display:none;position:absolute;inset:0;
  background:rgba(255,0,0,.08);z-index:2;
  justify-content:center;align-items:center;flex-direction:column
}
.lead-off-overlay.show{display:flex}
.lead-off-icon{font-size:48px;animation:blink 1s infinite}
.lead-off-text{font-size:20px;color:#f44;font-weight:700;margin-top:8px}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.15}}

/* ---- Status Bar (mobile bottom) ---- */
.status-bar{
  display:flex;justify-content:space-around;align-items:center;
  padding:10px 0;flex-shrink:0;font-size:12px;opacity:.55;
  border-top:1px solid rgba(0,255,136,.08)
}
.status-dot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:4px}
.status-dot.green{background:#00ff88}
.status-dot.red{background:#f44}
.status-dot.yellow{background:#ffb700}

/* ---- BPM Color Zones ---- */
.bpm-normal{color:#00ff88}
.bpm-warn{color:#ffb700}
.bpm-danger{color:#ff4444}
.valid-match{color:#00ff88}
.valid-deviation{color:#ffb700}
.valid-conflict{color:#ff4444}

/* =============================================================
   PC Layout — screen width >= 768px
   ============================================================= */
@media(min-width:768px){
  body{
    display:grid;
    grid-template-columns:1fr 240px;
    grid-template-rows:1fr;
  }
  .ecg-container{
    grid-column:1;grid-row:1;
    min-height:0;
  }
  .side-panel{
    grid-column:2;grid-row:1;
    display:flex;flex-direction:column;
    justify-content:center;align-items:center;
    border-left:1px solid rgba(0,255,136,.15);
    padding:24px 16px;gap:12px;
  }
  .side-panel .bpm-panel{padding:0}
  .side-panel .bpm-value{font-size:96px}
  .status-bar{
    flex-direction:column;gap:12px;
    border-top:none;
    font-size:12px;
  }
}
</style>
</head>
<body>

<!-- BPM Panel -->
<div class="side-panel">
  <div class="bpm-panel" id="bpmPanel">
    <div class="bpm-value bpm-normal" id="bpmValue">--</div>
    <div class="bpm-unit">次/分</div>
    <div class="bpm-zone bpm-normal" id="bpmZone">--</div>
    <div class="bpm-ref" id="bpmRefLine">
      ESP32: <span id="bpmRefVal">--</span>
    </div>
    <div class="validation valid-match" id="validation"></div>
  </div>
  <div class="status-bar" id="statusBar">
    <div><span class="status-dot green" id="leadDot"></span>导联<span id="leadText">检测中</span></div>
    <div>RR: <span id="rrVal">--</span> ms</div>
    <div>📶 <span id="wsStatus">等待连接</span></div>
  </div>
</div>

<!-- ECG Waveform -->
<div class="ecg-container" id="ecgContainer">
  <canvas id="ecgCanvas"></canvas>
  <div class="lead-off-overlay" id="leadOverlay">
    <div class="lead-off-icon">⚠️</div>
    <div class="lead-off-text">电极脱落</div>
  </div>
</div>

<script>
/* ================================================================
   Constants
   ================================================================ */
var SAMPLE_RATE = 250;
var FIR_TAPS = 32;
var FIR_LOW = 5;
var FIR_HIGH = 15;
var MWI_WINDOW = 38;           // ~150ms @ 250Hz
var REFRACTORY_MS = 200;       // ms
var RECENT_MOBILE = 500;       // 2s visible
var RECENT_PC = 1250;          // 5s visible

/* ================================================================
   State
   ================================================================ */
var isPC = window.innerWidth >= 768;
var maxSamples = isPC ? RECENT_PC : RECENT_MOBILE;
var rawBuf = [];
var lastSampleTime = 0;

var dsp = {
    fir: null,
    firHistory: [],
    signalPeak: 100,
    noisePeak: 50,
    lastPeakTime: 0,
    prevPeakTime: 0,
    rrIntervals: [],
    bpm: 0,
    rr: 0,
    bpmRef: 0,
    validation: 'init',
    leadOk: true,
    _prevFiltered: undefined,
    _mwiBuf: null,
    _baseline: 2048      // rolling DC baseline, starts at ideal VCC/2
};

/* ================================================================
   DOM refs
   ================================================================ */
var ecgCont = document.getElementById('ecgContainer');
var canvas  = document.getElementById('ecgCanvas');
var ctx     = canvas.getContext('2d');

/* ================================================================
   Canvas Sizing — no DPR scaling, CSS pixels only
   ================================================================ */
function resizeCanvas() {
    var w = ecgCont.clientWidth;
    var h = ecgCont.clientHeight;
    if (w <= 0 || h <= 0) return;

    // Set internal resolution = CSS size (no DPR multiply)
    if (canvas.width !== w || canvas.height !== h) {
        canvas.width  = w;
        canvas.height = h;
        console.log('[Canvas] resize ' + w + 'x' + h + (isPC ? ' PC' : ' Mobile'));
    }
}

/* ================================================================
   Grid Drawing
   ================================================================ */
function drawGrid(w, h) {
    var major = 50;
    var minor = 10;

    // Minor grid
    ctx.strokeStyle = 'rgba(0,255,136,0.08)';
    ctx.lineWidth = 0.5;
    ctx.beginPath();
    for (var x = 0; x < w; x += minor) {
        ctx.moveTo(x, 0); ctx.lineTo(x, h);
    }
    for (var y = 0; y < h; y += minor) {
        ctx.moveTo(0, y); ctx.lineTo(w, y);
    }
    ctx.stroke();

    // Major grid
    ctx.strokeStyle = 'rgba(0,255,136,0.18)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (var x = 0; x < w; x += major) {
        ctx.moveTo(x, 0); ctx.lineTo(x, h);
    }
    for (var y = 0; y < h; y += major) {
        ctx.moveTo(0, y); ctx.lineTo(w, y);
    }
    ctx.stroke();
}

/* ================================================================
   Main Render Loop — runs every frame
   ================================================================ */
function render() {
    requestAnimationFrame(render);

    var w = ecgCont.clientWidth;
    var h = ecgCont.clientHeight;
    if (w <= 0 || h <= 0) return;

    // Keep canvas internal size in sync with CSS size
    if (canvas.width !== w || canvas.height !== h) {
        canvas.width  = w;
        canvas.height = h;
    }

    // Clear (no save/restore, identity transform)
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = '#0a0a0f';
    ctx.fillRect(0, 0, w, h);

    // Always draw grid
    drawGrid(w, h);

    // No real data → show status text
    if (rawBuf.length < 2) {
        ctx.fillStyle = 'rgba(0,255,136,0.3)';
        ctx.font = (isPC ? '18px' : '14px') + ' sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('等待 ECG 数据...', w / 2, h / 2);
        return;
    }

    // ---- Per-pixel averaging with rolling auto-baseline ----
    // Auto-centers the waveform regardless of ADC DC offset.
    // Medical monitors use this technique to handle baseline wander.
    var visible = isPC ? RECENT_PC : RECENT_MOBILE;
    var start   = Math.max(0, rawBuf.length - visible);
    var count   = rawBuf.length - start;
    if (count < 2) return;

    var midY    = h / 2;
    var amp     = h * 0.42;
    var spx     = count / w;  // samples per pixel column
    var BL_ALPHA = 0.002;     // baseline adaptation rate (slow)

    // First pass: compute the visible window's average for baseline init
    if (dsp._baseline < 10) {
        var initSum = 0;
        for (var j = start; j < rawBuf.length; j++) initSum += rawBuf[j];
        dsp._baseline = initSum / count;
    }

    ctx.beginPath();
    ctx.strokeStyle = '#00ff88';
    ctx.lineWidth   = isPC ? 3 : 2;
    ctx.shadowBlur  = isPC ? 6 : 4;
    ctx.shadowColor = '#00ff88';

    var firstPoint = true;

    for (var px = 0; px < w; px++) {
        var s0 = Math.floor(start + px * spx);
        var s1 = Math.floor(start + (px + 1) * spx);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > rawBuf.length) s1 = rawBuf.length;

        var sum = 0, n = 0;
        for (var j = s0; j < s1; j++) {
            var v = rawBuf[j];
            sum += v;
            n++;
            // Slowly track DC baseline
            dsp._baseline = (1 - BL_ALPHA) * dsp._baseline + BL_ALPHA * v;
        }
        if (n === 0) continue;

        var avg  = sum / n;
        // Normalize relative to rolling baseline (not fixed 2048)
        var norm = (avg - dsp._baseline) / 600;
        if (norm < -1) norm = -1;
        if (norm >  1) norm =  1;

        var y = midY - norm * amp;

        if (firstPoint) {
            ctx.moveTo(px, y);
            firstPoint = false;
        } else {
            ctx.lineTo(px, y);
        }
    }
    ctx.stroke();
    ctx.shadowBlur = 0;
}

/* ================================================================
   FIR Bandpass Coefficients — computed once at init
   ================================================================ */
function computeFIR(taps, lo, hi, fs) {
    var nyq = fs / 2;
    var w1 = 2 * Math.PI * (lo / nyq);
    var w2 = 2 * Math.PI * (hi / nyq);
    var c = (taps - 1) / 2;
    var coeffs = new Float64Array(taps);

    for (var i = 0; i < taps; i++) {
        var n = i - c;
        var h;
        if (Math.abs(n) < 1e-9) {
            h = (w2 - w1) / Math.PI;
        } else {
            h = (Math.sin(w2 * n) - Math.sin(w1 * n)) / (Math.PI * n);
        }
        // Hamming window
        coeffs[i] = h * (0.54 - 0.46 * Math.cos(2 * Math.PI * i / (taps - 1)));
    }

    // Unity gain at passband
    var sum = 0;
    for (var i = 0; i < taps; i++) sum += coeffs[i];
    for (var i = 0; i < taps; i++) coeffs[i] /= sum;

    return coeffs;
}

/* ================================================================
   FIR single-step filter
   ================================================================ */
function firStep(sample) {
    dsp.firHistory.push(sample);
    if (dsp.firHistory.length > FIR_TAPS) dsp.firHistory.shift();
    if (dsp.firHistory.length < FIR_TAPS) return sample;

    var sum = 0;
    for (var i = 0; i < FIR_TAPS; i++) {
        sum += dsp.firHistory[FIR_TAPS - 1 - i] * dsp.fir[i];
    }
    return sum;
}

/* ================================================================
   Pan-Tompkins DSP Pipeline — process one sample at a time
   ================================================================ */
function processSample(raw, t) {
    // Stage 1: FIR bandpass
    var filtered = firStep(raw);

    // Stage 2+3: diff + square
    if (dsp._prevFiltered === undefined) {
        dsp._prevFiltered = filtered;
        return;
    }
    var diff = filtered - dsp._prevFiltered;
    dsp._prevFiltered = filtered;
    var sq = diff * diff;

    // Stage 4: moving-window integration
    if (!dsp._mwiBuf) dsp._mwiBuf = [];
    dsp._mwiBuf.push(sq);
    if (dsp._mwiBuf.length > MWI_WINDOW) dsp._mwiBuf.shift();
    if (dsp._mwiBuf.length < MWI_WINDOW) return;

    var mwiSum = 0;
    for (var j = 0; j < dsp._mwiBuf.length; j++) mwiSum += dsp._mwiBuf[j];
    var integrated = mwiSum / MWI_WINDOW;

    // Stage 5: adaptive threshold R-peak detection
    var refractoryOk = (t - dsp.lastPeakTime) >= REFRACTORY_MS;
    if (!refractoryOk) {
        dsp.noisePeak = 0.875 * dsp.noisePeak + 0.125 * integrated;
        return;
    }

    var thresh = 0.3 * dsp.signalPeak + 0.7 * dsp.noisePeak;

    if (integrated > thresh && thresh > 1e-6) {
        // R-PEAK
        dsp.signalPeak = 0.875 * dsp.signalPeak + 0.125 * integrated;

        if (dsp.prevPeakTime > 0) {
            var rr = t - dsp.prevPeakTime;
            dsp.rrIntervals.push(rr);
            if (dsp.rrIntervals.length > 8) dsp.rrIntervals.shift();

            if (dsp.rrIntervals.length >= 2) {
                var sumRR = 0;
                for (var k = 0; k < dsp.rrIntervals.length; k++) sumRR += dsp.rrIntervals[k];
                dsp.bpm = Math.round(60000 / (sumRR / dsp.rrIntervals.length));
                dsp.rr  = Math.round(sumRR / dsp.rrIntervals.length);
            }
        }

        dsp.prevPeakTime = t;
        dsp.lastPeakTime = t;
    } else {
        dsp.noisePeak = 0.875 * dsp.noisePeak + 0.125 * integrated;
    }
}

/* ================================================================
   Cross-Validation
   ================================================================ */
function validateBPM(jsBpm, refBpm) {
    if (refBpm === 0 || jsBpm === 0) return 'init';
    var diff = Math.abs(jsBpm - refBpm);
    if (diff <= 5)  return 'match';
    if (diff <= 10) return 'deviation';
    return 'conflict';
}

/* ================================================================
   Heart Rate Zone
   ================================================================ */
function getHRZone(bpm) {
    if (bpm === 0) return ['--', 'bpm-normal'];
    if (bpm < 50)  return ['过缓', 'bpm-danger'];
    if (bpm < 60)  return ['偏缓', 'bpm-warn'];
    if (bpm <= 100) return ['正常', 'bpm-normal'];
    if (bpm <= 120) return ['偏快', 'bpm-warn'];
    return ['过速', 'bpm-danger'];
}

/* ================================================================
   UI Updates
   ================================================================ */
function updateUI() {
    // BPM value
    document.getElementById('bpmValue').textContent = dsp.bpm > 0 ? dsp.bpm : '--';

    // HR zone + color
    var z = getHRZone(dsp.bpm);
    document.getElementById('bpmZone').textContent = z[0];
    document.getElementById('bpmZone').className = 'bpm-zone ' + z[1];
    document.getElementById('bpmValue').className = 'bpm-value ' + z[1];

    // ESP32 reference BPM
    document.getElementById('bpmRefVal').textContent = dsp.bpmRef > 0 ? dsp.bpmRef : '--';

    // Cross-validation
    dsp.validation = validateBPM(dsp.bpm, dsp.bpmRef);
    var valMap = {
        'init':       ['--', 'valid-match'],
        'match':      ['✓ 校验一致', 'valid-match'],
        'deviation':  ['⚠ 轻微偏差', 'valid-deviation'],
        'conflict':   ['✗ 校验冲突', 'valid-conflict']
    };
    var v = valMap[dsp.validation] || valMap['init'];
    document.getElementById('validation').textContent = v[0];
    document.getElementById('validation').className = 'validation ' + v[1];

    // RR interval
    document.getElementById('rrVal').textContent = dsp.rr > 0 ? dsp.rr : '--';

    // Lead status
    var dot  = document.getElementById('leadDot');
    var text = document.getElementById('leadText');
    var ov   = document.getElementById('leadOverlay');
    if (dsp.leadOk) {
        dot.className  = 'status-dot green';
        text.textContent = '正常';
        ov.classList.remove('show');
    } else {
        dot.className  = 'status-dot red';
        text.textContent = '脱落';
        ov.classList.add('show');
    }
}

/* ================================================================
   WebSocket
   ================================================================ */
var ws = null;
var wsReconnect = null;

function connectWS() {
    if (ws) { try { ws.close(); } catch(e) {} ws = null; }

    var url = 'ws://' + location.host + '/ws';
    console.log('[WS] Connecting to ' + url);
    ws = new WebSocket(url);

    ws.onopen = function() {
        console.log('[WS] Connected');
        document.getElementById('wsStatus').textContent = '已连接';
        if (wsReconnect) { clearInterval(wsReconnect); wsReconnect = null; }
    };

    ws.onmessage = function(e) {
        try {
            var msg = JSON.parse(e.data);
            var samples = msg.samples || [];
            var tBase   = msg.t || Date.now();

            // Feed samples into DSP pipeline + ring buffer
            for (var i = 0; i < samples.length; i++) {
                rawBuf.push(samples[i]);
                var st = tBase - (samples.length - 1 - i) * 4;
                processSample(samples[i], st);
            }

            // Trim ring buffer
            while (rawBuf.length > maxSamples + 500) rawBuf.shift();

            // ESP32 metadata
            dsp.bpmRef = msg.bpm_ref || 0;
            dsp.leadOk = msg.lead !== false;
            lastSampleTime = tBase;

            updateUI();
        } catch(err) {
            console.error('[WS] Parse error:', err);
        }
    };

    ws.onclose = function() {
        console.log('[WS] Disconnected');
        document.getElementById('wsStatus').textContent = '重连中';
        if (!wsReconnect) wsReconnect = setInterval(connectWS, 3000);
    };

    ws.onerror = function() {
        if (ws) { try { ws.close(); } catch(e) {} }
    };
}

/* ================================================================
   Resize Handler
   ================================================================ */
function onResize() {
    isPC = window.innerWidth >= 768;
    maxSamples = isPC ? RECENT_PC : RECENT_MOBILE;

    // Trim if switching to smaller buffer
    if (rawBuf.length > maxSamples + 500) {
        rawBuf = rawBuf.slice(rawBuf.length - maxSamples);
    }

    resizeCanvas();
}

/* ================================================================
   Init
   ================================================================ */
function init() {
    dsp.fir = computeFIR(FIR_TAPS, FIR_LOW, FIR_HIGH, SAMPLE_RATE);
    console.log('[DSP] FIR ' + FIR_TAPS + ' taps computed');

    resizeCanvas();
    connectWS();

    window.addEventListener('resize', onResize);
    requestAnimationFrame(render);
}

document.addEventListener('DOMContentLoaded', init);
</script>
</body>
</html>
)raw";

#endif // INDEX_HTML_H
