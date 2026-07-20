# 心电监测装置 — Heart Rate Monitor

基于 **ESP32-S3** + **AD8232** 的无线心电(ECG)实时监测装置。通过 WiFi 在手机浏览器上查看实时心电图波形和心率数值。

---

## 目录

1. [系统概览](#系统概览)
2. [硬件设计](#硬件设计)
3. [ECG 信号采集原理](#ecg-信号采集原理)
4. [心率检测算法](#心率检测算法)
5. [软件架构](#软件架构)
6. [WiFi 与 WebSocket 通信](#wifi-与-websocket-通信)
7. [前端 UI 设计](#前端-ui-设计)
8. [代码结构](#代码结构)
9. [搭建与烧录](#搭建与烧录)
10. [使用说明](#使用说明)

---

## 系统概览

```
┌──────────────────────────────────────────────────────────────┐
│                       ESP32-S3-N16R8                          │
│                                                              │
│  ┌──────────┐   ┌──────────────┐                             │
│  │ AD8232   │──▶│ ADC1 定时采样 │   @ 250Hz                  │
│  │ 心电前端 │   └──────┬───────┘                             │
│  └──────────┘          │                                     │
│                        ▼                                     │
│  ┌─────────────────────────────────────────────┐             │
│  │              环形缓冲区 + 导联检测             │             │
│  └────────┬───────────────────┬────────────────┘             │
│           │                   │                              │
│           ▼                   ▼                              │
│  ┌───────────────┐   ┌────────────────────────┐             │
│  │ 极简阈值 BPM   │   │    WebSocket Server     │             │
│  │ (轻量级,      │   │  每 50ms 推送:          │             │
│  │  备用参考值)   │   │  · 原始 ADC 采样        │             │
│  │               │   │  · 简易 BPM 参考值       │             │
│  │ 延迟: ~30ms   │   │  · 导联状态             │             │
│  └───────────────┘   └───────────┬─────────────┘             │
│                                  │                           │
│  WiFi AP ───────────────────────┼─── HTTP :80                │
│  SSID: "ECG-Monitor"            │                            │
│  192.168.4.1                    │                            │
└─────────────────────────────────┼────────────────────────────┘
                                  │
            手机/PC 连上 ECG-Monitor WiFi
            浏览器打开 http://192.168.4.1
                                  │
┌─────────────────────────────────┼────────────────────────────┐
│                         浏览器 (JavaScript)                   │
│                                  │                           │
│  ┌───────────────────────────────┼──────────────────────┐    │
│  │  WebSocket 接收: {samples, bpm_ref, lead}            │    │
│  │        │                                             │    │
│  │  ┌─────┴──────┐    ┌────────────┐                    │    │
│  │  │ QRS 检测   │───▶│ BPM + RR   │  主力算法          │    │
│  │  │ (Pan-Tompkins)  │ 移动平均   │  (高精度)          │    │
│  │  └────────────┘    └─────┬──────┘                    │    │
│  │                          │                           │    │
│  │         ┌────────────────┼────────────────┐          │    │
│  │         ▼                ▼                ▼          │    │
│  │  主力 BPM 值      bpm_ref(ESP32)    交叉校验状态     │    │
│  │  (大号显示)        (副显示)         一致/偏差告警    │    │
│  │                                                      │    │
│  │         ▼                                            │    │
│  │  Canvas 2D 实时心电图波形                              │    │
│  │  (深色网格 + 霓虹绿线)                                 │    │
│  └──────────────────────────────────────────────────────┘    │
│                                                              │
│       ┌────────────────────┬────────────────────┐            │
│       │  移动端布局         │   PC 端布局         │            │
│       │  BPM 为视觉中心     │   ECG 全屏主视图    │            │
│       │  波形紧凑排列       │   BPM 侧栏显示      │            │
│       └────────────────────┴────────────────────┘            │
└──────────────────────────────────────────────────────────────┘
```

### 设计原则

| 原则 | 说明 |
|------|------|
| **零依赖使用** | 无需安装 App，任何有浏览器的设备（手机/平板/PC）都能使用 |
| **响应式双端适配** | 移动端 BPM 优先，PC 端 ECG 波形为主视图，CSS 媒体查询自适应 |
| **本地直连** | AP 模式不依赖外部路由器，户外、医院、家中都能用 |
| **双路径心率计算** | ESP32 跑极简阈值法作为低延迟参考 + 离线备用；浏览器跑 Pan-Tompkins 作为高精度主力；两值交叉校验 |
| **双核分离** | Core 0 跑 WiFi，Core 1 跑定时采样+数据推送，互不干扰 |
| **实时性优先** | 硬件定时器驱动采样，250Hz 精确采样率保证波形质量 |

---

## 硬件设计

### 物料清单

| 物料 | 型号/规格 | 数量 | 备注 |
|------|-----------|------|------|
| MCU 开发板 | ESP32-S3-N16R8 | 1 | 16MB Flash + 8MB PSRAM |
| 心电模拟前端 | AD8232 模块 | 1 | 淘宝常见红色模块 |
| 电极片 | 一次性心电电极片 | 3 | RA/LA/RL 各一片 |
| 电极导线 | 3.5mm 音频插头转鳄鱼夹 | 1 | 匹配 AD8232 模块的 3.5mm 接口 |
| 面包板 + 杜邦线 | — | 若干 | 原型搭建 |

### AD8232 模块说明

AD8232 是 Analog Devices 的单导联心率监测模拟前端芯片。内部集成了：

```
                    ┌───────────────────┐
  RA(右臂) ─────────┤ INA              │
                    │  仪表放大器       │
  LA(左臂) ─────────┤ (Gain = 100)     │
                    │                   │
  RL(右腿) ─────────┤ RLD 驱动          │──→ 共模抑制
                    │                   │
                    ├───────────────────┤
                    │ 二阶高通滤波器     │  f_c ≈ 7Hz, 滤除电极直流偏移
                    ├───────────────────┤
                    │ 二阶低通滤波器     │  f_c ≈ 24Hz, 抗混叠 + 滤除肌电噪声
                    ├───────────────────┤
                    │ 运算放大器        │ 额外增益可调
                    ├───────────────────┤
                    │ 导联脱落检测       │──→ LO+ / LO- 引脚
                    └───────────────────┘
                             │
                          OUTPUT (模拟电压)
```

- **OUTPUT**: 经过放大和滤波后的心电模拟信号，以 VCC/2 为中心
- **LO+ / LO-**: 导联脱落检测输出，电极接触不良时拉高
- **SDN**: 关断引脚，低电平有效（拉低 = 正常工作；拉高 = 休眠）

### 引脚连接表

```
AD8232 模块          ESP32-S3 (引脚布局)
────────────────────────────────────────────
  GND      ────▶    GND
  3.3V     ────▶    3.3V (模块工作在 3.3V)
  OUTPUT   ────▶    GPIO4  (ADC1_CH3)
  LO+      ────▶    GPIO15 (GPIO 数字输入)
  LO-      ────▶    GPIO16 (GPIO 数字输入)
  SDN      ────▶    GND   (拉低 = 始终工作)
```

**引脚选择说明：**

- **ADC1 而非 ADC2**：ESP32-S3 有两组 ADC。ADC2 与 WiFi 射频共用内部资源，WiFi 使能时 ADC2 读数会受到严重干扰。因此 ECG 模拟信号必须接 ADC1 通道。
- **GPIO4 (ADC1_CH3)**：在 ESP32-S3-DevKitC-1 上默认为空闲引脚，不会与 JTAG、UART、SPI Flash 冲突。
- **GPIO15/16**：普通 GPIO 数字输入，用于读取 LO+/LO- 状态，判断电极是否脱落。

### 硬件调试：确认 AD8232 工作正常

烧录后打开串口监视器（`pio device monitor`），观察启动时的 ADC 自检输出：

```
[ECG] ADC self-test (should read ~1500-2500 with AD8232 powered):
  read #1: 2123
  read #2: 2145
  ...
```

#### 快速排查（无需万用表）

**第一步：验证模块供电**

只给 AD8232 模块接 VCC(3.3V) 和 GND 两根线，其他线全部断开。观察模块上的 **LED 是否亮起**：
- LED 亮 → 模块供电正常，继续下一步
- LED 不亮 → 换杜邦线/换面包板插孔重试；仍不亮则模块可能损坏

**第二步：逐根加线验证**

保持供电，按顺序逐根接线，每加一根观察 LED 是否保持亮：
1. 加 SDN → GND（LED 应保持亮）
2. 加 OUTPUT → GPIO4（LED 应保持亮）
3. 加 LO+ → GPIO15, LO- → GPIO16（LED 应保持亮）

如果某一步 LED 灭，说明那根线引入短路或接触不良。

**第三步：信号通路验证**

晃动贴好的电极片，观察网页上的波形是否有扰动——有扰动说明整个信号链路（皮肤→电极→AD8232→ADC→WiFi→浏览器）是通的。

#### 万用表排查

1. **测 AD8232 OUTPUT 脚电压**（相对于 GND）：
   - 正常值：约 **1.65V**（VCC/2）
   - 接近 0V 或 3.3V：模块可能损坏，或 SDN 未正确接 GND

2. **确认 SDN 接 GND**：SDN 必须拉低才工作。测 SDN ↔ GND 应导通。

3. **用已知电压测试 ADC**：断开 AD8232 OUTPUT，将 GPIO4 分别接 GND 和 3.3V，串口应分别输出接近 0 和 4095。

4. **导联检测测试**：测 LO+ / LO- 电压。电极贴好时应为 LOW (~0V)，摘除时应为 HIGH (~3.3V)。如果始终不变，模块的导联检测电路可能损坏（不影响 ECG 信号采集本身）。

### 电极贴放位置

AD8232 模块标配 3.5mm 音频插头引出三根电极线，常见颜色编码如下：

| 颜色 | 标识 | 贴放位置 | 说明 |
|------|------|----------|------|
| 🔴 **红** | **RA** (Right Arm) | 右锁骨下方约 5cm 处 | 信号正输入端 |
| 🟡 **黄** | **LA** (Left Arm) | 左锁骨下方约 5cm 处 | 信号负输入端 |
| 🟢 **绿** | **RL** (Right Leg) | 右下腹 / 右肋下方 | 右腿驱动 (参考地)，抑制共模干扰 |

```
       🔴 RA (红) — 右锁骨下
       🟡 LA (黄) — 左锁骨下
       🟢 RL (绿) — 右下腹 (参考驱动)

        🔴 ●              ● 🟡
              \          /
               \        /
                \      /
                 ● 🟢
```

> ⚠️ 不同厂商的电极线颜色可能略有差异，以模块上丝印的 RA/LA/RL 标识为准。上表为淘宝常见红色 AD8232 模块的默认配色。

> ⚠️ **安全警告**：本装置仅为原型/实验用途，不可替代医疗设备。不可用于临床诊断。

---

## ECG 信号采集原理

### 信号特征

心电信号由心脏每次搏动产生的电活动形成，典型特征：
![ECGfeature](/pics/ECGfeature.png)
- **P 波**：心房去极化，幅度约 0.1-0.3mV
- **QRS 波群**：心室去极化，R 波幅度约 0.5-2.0mV（最显著特征）
- **T 波**：心室复极化，幅度约 0.1-0.5mV
- **RR 间期**：两个 R 波之间的时间，用于计算心率

### 采样参数选择

| 参数 | 值 | 原因 |
|------|-----|------|
| 采样率 | **250 Hz** (4ms 间隔) | 满足奈奎斯特定理（ECG 主要能量集中在 0.5-40Hz）|
| ADC 分辨率 | **12-bit** (0-4095) | ESP32-S3 原生支持，对于监测足够 |
| 缓冲区大小 | **256 个采样点** (≈1 秒) | 平衡内存开销与波形连续性 |

### 定时器驱动的采样方式

使用 ESP32-S3 硬件定时器 (Timer Group) 产生精确中断，在 ISR 中读取 ADC 值并写入环形缓冲区。这比 `delay()` 或 RTOS 软件定时器更精确：

```
Timer ISR @ 250Hz:
  1. adc1_get_raw(ADC1_CH3)      → 读取 ADC
  2. ring_buffer[index] = value   → 写入环形缓冲区
  3. index = (index + 1) % 256    → 移动写指针
  4. samples_ready = true         → 通知主任务处理
```

**为什么用中断而非线程轮询？** 采样的时序准确性直接影响 R 波检测精度。如果采样间隔抖动过大，RR 间期计算会漂移，造成 BPM 误差。硬件定时器中断的抖动在微秒级，而 RTOS 任务调度抖动可能达到毫秒级。

---

## 心率检测算法

### 双路径架构

系统采用**两条独立的心率计算路径**，各自运行在不同的处理器上，最后在 UI 层交叉校验：

```
┌─────────────────────────────────────────────────────────┐
│                    ESP32-S3 (固件)                       │
│                                                         │
│  ADC 采样 @ 250Hz                                       │
│       │                                                 │
│       ▼                                                 │
│  ┌────────────────────────────────────┐                 │
│  │  路径 A: 极简阈值法 (Simple Peak)   │                 │
│  │  · 不做 FIR 滤波 (AD8232 已做模拟滤波)               │
│  │  · 不做差分/平方/积分                                │
│  │  · 直接对原始值做固定阈值穿越检测                     │
│  │  · 自适应阈值 = 0.3×recent_peak + 0.7×noise_floor   │
│  │  · 不应期 200ms                                      │
│  │  · 代码量: ~30 行 C++                                │
│  │  · CPU 开销: < 0.1ms / 批次                         │
│  │  · 输出: bpm_ref → 放入 WebSocket JSON               │
│  └────────────────────────────────────┘                 │
│                                                         │
│  ┌────────────────────────────────────┐                 │
│  │  原始采样数据 (不做 DSP)            │                 │
│  │  → 每 50ms 打包 → WebSocket        │                 │
│  └────────────────────────────────────┘                 │
└─────────────────────────┬───────────────────────────────┘
                          │  WebSocket JSON:
                          │  {t, samples[], bpm_ref, lead}
                          ▼
┌─────────────────────────────────────────────────────────┐
│                   浏览器 (JavaScript)                    │
│                                                         │
│  ┌────────────────────────────────────┐                 │
│  │  路径 B: Pan-Tompkins (主力算法)     │                 │
│  │  · FIR 带通滤波 5-15Hz (32阶)       │                 │
│  │  · 差分 + 平方 + 滑动窗口积分       │                 │
│  │  · 自适应阈值 R 峰检测              │                 │
│  │  · 移动平均 BPM (最近8次)           │                 │
│  │  · 输出: bpm, rr                    │                 │
│  └────────────────────────────────────┘                 │
│                                                         │
│  ┌────────────────────────────────────┐                 │
│  │  交叉校验                            │                 │
│  │  · 比较 |bpm - bpm_ref|             │                 │
│  │  · 差值 < 5 → ✅ 一致 (绿色)        │                 │
│  │  · 差值 5-10 → ⚠️ 偏差 (黄色)       │                 │
│  │  · 差值 > 10 → ❌ 异常 (红色)       │                 │
│  └────────────────────────────────────┘                 │
└─────────────────────────────────────────────────────────┘
```

### 为什么双路径？

| 考量 | 路径 A: ESP32 极简阈值 | 路径 B: 浏览器 Pan-Tompkins |
|------|----------------------|---------------------------|
| **精度** | 中等，易受运动伪差影响 | 高，FIR 滤波 + 多级 DSP 抑制噪声 |
| **延迟** | ~30ms（无传输等待）| ~30ms（受 50ms 批次间隔主导，算法本身 <1ms）|
| **离线可用** | ✅ 无浏览器也能工作 | ❌ 必须有浏览器连接 |
| **算法迭代** | 需编译烧录 | F5 刷新即生效 |
| **未来用途** | 蜂鸣器告警 / LED 指示 / 离线数据记录 | 高精度波形分析 / HRV |

### 延迟分析

两条路径的总延迟几乎一致（~30ms），主导因素是 WebSocket 的 50ms 批次间隔（R 波发生到下一包发出的平均等待时间 ≈ 25ms），而非 DSP 执行时间：

```
R 波发生 ──▶ ADC 捕获 (~2ms) ──▶ 等待下一批次发出 (~25ms avg) ──▶ 传输+渲染 (~3ms) ──▶ 屏幕更新
                                                                     ├── ESP32 BPM: 即时 (值与采样同包发出)
                                                                     └── 浏览器 BPM: +0.5ms (JS DSP)
```

所以 ESP32 端做轻量 BPM 并不能显著降低浏览器端的感知延迟。它的真正价值在于**离线备用**——当没有浏览器连接时，ESP32 仍然可以独立输出心率数据（串口/蓝牙/蜂鸣器/LED）。

### ESP32 端：极简阈值法 (Simple Peak Detection)

不做完整 Pan-Tompkins 流水线，利用 AD8232 已内置模拟带通滤波（7-24Hz）的优势，直接对 ADC 原始值做阈值穿越检测：

```cpp
// 极简 R 峰检测 — 仅 ~30 行
class SimplePeakDetector {
    static constexpr int REFRACTORY_MS = 200;  // 生理不应期
    static constexpr float SIGNAL_WEIGHT = 0.3f;
    static constexpr float NOISE_WEIGHT = 0.7f;

    float signal_peak = 500;   // 初始化假设值
    float noise_peak = 100;
    uint32_t last_peak_ms = 0;
    uint16_t rr_intervals[8] = {};
    int rr_idx = 0;

public:
    // 每收到一个 ADC 采样点调用一次，返回 "是否检测到 R 波"
    bool feed(uint16_t adc_value, uint32_t now_ms) {
        if (now_ms - last_peak_ms < REFRACTORY_MS) return false;

        float threshold = SIGNAL_WEIGHT * signal_peak + NOISE_WEIGHT * noise_peak;
        if (threshold < 50) threshold = 50;  // 最小阈值防止噪声触发

        if (adc_value > threshold) {
            signal_peak = 0.875f * signal_peak + 0.125f * adc_value;
            last_peak_ms = now_ms;
            return true;  // ← R 波!
        } else {
            noise_peak = 0.875f * noise_peak + 0.125f * adc_value;
            return false;
        }
    }

    // 返回移动平均 BPM (0 = 还不够数据)
    int getBPM(uint16_t new_rr_ms) {
        rr_intervals[rr_idx % 8] = new_rr_ms;
        rr_idx++;
        if (rr_idx < 2) return 0;
        int count = min(rr_idx, 8);
        float avg = 0;
        for (int i = 0; i < count; i++) avg += rr_intervals[i];
        return (int)(60000.0f / (avg / count));
    }
};
```

**设计要点：**

- **不滤波** — AD8232 芯片已提供 7-24Hz 硬件带通滤波，ESP32 端再做软件滤波是冗余操作
- **不做差分/平方/积分** — 这些是 Pan-Tompkins 为了提高极低 SNR 下的检出率；AD8232 输出的信号 SNR 足够高，阈值穿越即可有效检出 R 波
- **EMA 平滑** (`0.875 × old + 0.125 × new`) — 替代滑动窗口，单变量维护，无数组开销
- **CPU 开销极低** — 每个采样点仅 3 次浮点乘法和 3 次比较，在 250Hz 下远小于 1% CPU

### 浏览器端：Pan-Tompkins 主力算法

Pan-Tompkins (1985) 是 ECG QRS 检测的经典算法。在 JavaScript 中实现其核心流程：

```
WebSocket 接收原始 ADC 采样 (每 50ms 一批约 13 个点)
       │
       ▼  追加到前端环形缓冲区 (≈1250 点 / 5 秒历史)
       │
┌──────┴─────────────────────────────────────────┐
│              JavaScript DSP 流水线               │
│                                                 │
│  ┌──────────┐   ┌────────┐   ┌──────┐          │
│  │ 带通滤波  │──▶│ 差分   │──▶│ 平方 │          │
│  │ 5-15Hz   │   │ y[n]=  │   │y[n]² │          │
│  │ FIR 32阶 │   │ x[n]-  │   │      │          │
│  │          │   │ x[n-1] │   │      │          │
│  └──────────┘   └────────┘   └──┬───┘          │
│                                 │              │
│  ┌──────────┐   ┌──────────────▼───────────┐   │
│  │ BPM 值   │◀──│ 自适应阈值 R 峰检测       │   │
│  │ 移动平均 │   │ · 阈值 = 0.3×peak +      │   │
│  │ (最近8次)│   │   0.7×noise              │   │
│  └──────────┘   │ · 不应期 200ms           │   │
│                 └──────┬───────────────────┘   │
│                        │                       │
│                 ┌──────▼──────┐                │
│                 │ 滑动窗口积分 │  窗口 150ms    │
│                 │ y[n]=Σx/W  │                │
│                 └─────────────┘                │
└────────────────────────────────────────────────┘
```

### 各阶段详解 (JavaScript 实现要点)

#### 阶段 1 — 带通滤波 (5–15 Hz)

目的：去除基线漂移（<1Hz）和高频噪声（>40Hz，肌电、工频干扰）。

FIR 滤波器系数在 JS 中作为常量数组定义，卷积运算实现：

```javascript
// 32 阶 FIR 带通系数 (Python scipy.signal.firwin 离线计算)
const FIR_COEFFS = [0.0012, 0.0025, -0.0031, ...]; // 32 个 float

function applyFilter(samples) {
    return samples.map((_, i) => {
        let sum = 0;
        for (let j = 0; j < FIR_COEFFS.length; j++) {
            if (i >= j) sum += samples[i - j] * FIR_COEFFS[j];
        }
        return sum;
    });
}
```

#### 阶段 2 — 差分运算

`y[n] = x[n] - x[n-1]`

强调信号斜率，突出 QRS 波群的快速上升沿。

#### 阶段 3 — 平方运算

`y[n] = x[n]²`

非线性放大——R 波的高幅度被放大到远超 P/T 波和噪声的水平。

#### 阶段 4 — 滑动窗口积分

`y[n] = (1/W) × Σ(x[n-i]),  i = 0..W-1`

窗口宽度 W = 38 (≈150ms @ 250Hz)。积分产生平滑包络，QRS 波群表现为单峰。

#### 阶段 5 — 自适应阈值检测

```javascript
const SIGNAL_WEIGHT = 0.3;
const NOISE_WEIGHT = 0.7;
const REFRACTORY_MS = 200;  // 不应期

let threshold = 0;
let signalPeak = 0;  // 最近的 R 波峰值
let noisePeak = 0;   // 非 R 波区的噪声峰值
let lastPeakTime = 0;

function detectRPeak(integratedValue, timestamp) {
    // 不应期检查
    if (timestamp - lastPeakTime < REFRACTORY_MS) return false;

    threshold = SIGNAL_WEIGHT * signalPeak + NOISE_WEIGHT * noisePeak;

    if (integratedValue > threshold) {
        signalPeak = 0.875 * signalPeak + 0.125 * integratedValue; // EMA 平滑
        lastPeakTime = timestamp;
        return true;  // R 波检测到!
    } else {
        noisePeak = 0.875 * noisePeak + 0.125 * integratedValue;
        return false;
    }
}
```

#### 阶段 6 — BPM 计算

```javascript
let rrIntervals = [];  // 最近 8 次 RR 间期

function calcBPM(timestamp) {
    if (lastPeakTime === 0) return 0;
    const rr = timestamp - lastPeakTime;
    rrIntervals.push(rr);
    if (rrIntervals.length > 8) rrIntervals.shift();
    const avgRR = rrIntervals.reduce((a, b) => a + b) / rrIntervals.length;
    return Math.round(60000 / avgRR);
}
```

### 交叉校验 (Cross-Validation)

两条路径独立计算 BPM，浏览器端比较结果：

```javascript
function validateBPM(bpm_js, bpm_ref) {
    const diff = Math.abs(bpm_js - bpm_ref);

    if (bpm_ref === 0) return 'init';        // ESP32 还没算出有效值
    if (diff <= 5)   return 'match';         // ✅ 两个算法一致
    if (diff <= 10)  return 'deviation';     // ⚠️ 轻微偏差，可能一个误检
    return 'conflict';                       // ❌ 差异过大，信号可能异常
}
```

UI 层根据校验结果显示：

| 状态 | UI 表现 | 含义 |
|------|---------|------|
| `init` | 仅显示浏览器 BPM，无校验标记 | ESP32 尚在收集初始 R 波（需 2 次以上）|
| `match` | BPM 正常颜色，侧栏小字显示 "✓ 校验一致" | 两路径吻合，可信度高 |
| `deviation` | BPM 显示为黄色，侧栏显示两值差异 | 可能某路径偶发误检/漏检，下次心跳会收敛 |
| `conflict` | BPM 显示为红色闪烁，侧栏显示两值 | 信号质量可能差（运动/电极老化），建议检查 |

### 设计优势总结

| 优势 | 说明 |
|------|------|
| **离线可用** | 无浏览器时 ESP32 仍可独立提供心率（串口/蜂鸣器/LED），适合便携场景 |
| **高精度主力** | 浏览器端 Pan-Tompkins 充分利用 V8 JIT 浮点性能，算法不上 ESP32 固件的体积限制 |
| **互相校验** | 两个独立算法对比，能检测信号异常和算法漂移，提升可靠性 |
| **快速迭代** | 主力算法在 JS 层，改代码 → F5 刷新 → 立即看效果，无需编译烧录 |

---

## 软件架构

### FreeRTOS 任务分配

ESP32-S3 为双核 Xtensa LX7 处理器：

```
┌────────────── Core 0 (Protocol) ─────────────┐
│                                                │
│  · WiFi 协议栈 (ESP-IDF 自动调度)              │
│  · AsyncWebServer (HTTP 请求处理)              │
│  · AsyncWebSocket (推送采样数据)               │
│                                                │
└────────────────────────────────────────────────┘

┌────────────── Core 1 (Application) ───────────┐
│                                                │
│  · Timer Group ISR (250Hz ADC 采样)            │
│  · 环形缓冲区写入                              │
│  · SimplePeakDetector::feed() 轻量 R 波检测    │
│  · 导联脱落 GPIO 检测                          │
│  · WebSocket 数据打包并发送                     │
│                                                │
└────────────────────────────────────────────────┘
```

固件核心任务仅四项，极简阈值 BPM 的 CPU 开销可忽略不计。

### 任务协作流程

```
 Hardware Timer ISR                     WebSocket Push Task
  (Core 1, 250Hz)                       (Core 1, 优先级1)
       │                                      │
       │  每 4ms:                             │
       │  1. ADC 采样 → 环形缓冲区             │
       │  2. SimplePeakDetector.feed(value)    │
       │     → 若 R 波: 更新 bpm_ref          │
       │                                      │
       │                                      │  每 50ms 触发:
       │                                      │  1. 读取环形缓冲区中的新采样
       │                                      │  2. 读取 bpm_ref (ESP32 极简 BPM)
       │                                      │  3. 读取 LO+/LO- GPIO 状态
       │                                      │  4. 打包 JSON
       │                                      │  5. ws.textAll(json)
       │                                      │
       ▼                                      ▼
  ┌────────────────────────────────────────────────────┐
  │          环形缓冲区 (256 × uint16_t)                 │
  │  write_ptr ───────────────────────────── read_ptr  │
  └────────────────────────────────────────────────────┘
                                                  │
                                                  ▼
                                    JSON: {t, samples[], bpm_ref, lead}
                                                  │
                                                  ▼
                                    手机/PC 浏览器 WebSocket 接收
                                        ↓
                              ┌─────────┴─────────┐
                              ▼                   ▼
                      路径 B: Pan-Tompkins    读取 bpm_ref
                      主力 BPM (高精度)       ESP32 参考值
                              │                   │
                              └─────────┬─────────┘
                                        ▼
                                  交叉校验
                              |bpm - bpm_ref| ≤ 5?
                                        │
                                        ▼
                                  Canvas 实时渲染
```

### 缓冲区设计

使用**双缓冲 / 环形缓冲区**，写入者在 ISR 中，读取者在数据处理任务中：

```
环形缓冲区 (256 × 2 bytes = 512 bytes):

 write_ptr                    read_ptr
    │                            │
    ▼                            ▼
  ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬──
  │5│2│8│3│1│7│ │ │ │ │ │ │ │ │ │   ...
  └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴──
        ◄── 未读数据 ──►

每次 ISR 写 1 个点，write_ptr 前进 1
每次处理任务读 N 个点，read_ptr 前进 N
```

---

## WiFi 与 WebSocket 通信

### 为什么选择 AP 模式 + WebSocket

| 需求 | 方案 | 理由 |
|------|------|------|
| 手机/PC 与 ESP32 通信 | **AP 模式** | 无需路由器，户外/病房等场景也可用；ESP32 自己开热点 |
| 实时波形推送 | **WebSocket** | 全双工、低开销、持续连接；比 HTTP 轮询延迟低得多 |
| 页面加载 | **HTTP** | 首次连接时通过 HTTP 返回 HTML/JS 页面 |
| 数据格式 | **JSON** | 人类可读、调试方便、前端 `JSON.parse()` 原生支持 |
| 双端适配 | **CSS 媒体查询** | 同一份 HTML，根据屏幕宽度自动切换移动端/PC 端布局 |

### 连接流程

```
手机/PC                           ESP32-S3
 │                                    │
 │  1. 扫描 WiFi 列表                  │
 │     发现 "ECG-Monitor"             │
 │  2. 连接 WiFi                      │
 │────────────────────────────────────│  DHCP 分配 192.168.4.x
 │                                    │
 │  3. 浏览器打开 192.168.4.1          │
 │────────────────────────────────────│  HTTP GET /
 │                                    │  返回 HTML 页面 (gzip)
 │  4. 页面中 JS 发起 WebSocket       │
 │     new WebSocket("ws://192.168.4.1/ws")
 │────────────────────────────────────│  WebSocket 握手
 │                                    │
 │  5. 持续接收 WebSocket 消息        │
 │◀───────────────────────────────────│  每 50ms:
 │     onmessage →                    │  {t, samples[], bpm_ref, lead}
 │     路径B Pan-Tompkins DSP →       │
 │     交叉校验 bpm vs bpm_ref →      │
 │     Canvas 渲染 + BPM 更新          │
```

### WebSocket 数据格式

每 50ms 推送一条 JSON 消息（约 12-13 个采样点）：

```json
{
  "t": 12345,
  "samples": [2048, 2052, 2049, 2055, 2043, 2038, 2042, 2050, 2047, 2045, 2051, 2044],
  "bpm_ref": 72,
  "lead": true
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `t` | uint32 | 时间戳（毫秒），ESP32 上电以来累计 |
| `samples` | uint16[] | 最近一批 ADC 采样值 (0-4095)，未经任何处理 |
| `bpm_ref` | int | ESP32 极简阈值法算出的参考心率，0 = 尚未检测到足够 R 波。浏览器端用作交叉校验 |
| `lead` | bool | 导联连接状态，false = 电极脱落 |

**双路径说明：** `bpm_ref` 是 ESP32 用极简阈值法算的**参考值**（精度中等，延迟低），浏览器端用 Pan-Tompkins 算**主力值**（高精度），UI 上对比两值做交叉校验。即使浏览器断开，ESP32 仍可通过串口/蜂鸣器/LED 独立输出 `bpm_ref`。

**为什么每 50ms 一包而不是每个采样点一包？**

单个 WebSocket 文本帧有约 40-50 字节的帧头开销。逐个发送 250 个采样点/秒 ≈ 250 × 50 ≈ 12.5KB/秒 的纯开销。批量发送 (50ms = 约 13 个采样点/包 = 20 包/秒) 将开销降到 ~1KB/秒，手机端的 JS 处理负担也大幅减轻。

### WiFi AP 配置

```
SSID:       "ECG-Monitor"
密码:       无 (开放热点)
IP:         192.168.4.1
子网掩码:    255.255.255.0
信道:       6
最大连接数:  4
```

---

## 前端 UI 设计

### 响应式双端适配策略

整个页面采用 **CSS 媒体查询** 实现移动端和 PC 端的自适应布局。核心设计理念：

| 设备 | 断点 | 设计重心 | 布局方式 |
|------|------|----------|----------|
| 手机 | `width < 768px` | **BPM 数值为视觉中心**，波形紧凑排列在下方 | 单列纵向 `flexbox` |
| PC/平板 | `width ≥ 768px` | **ECG 波形图为主体**，全屏铺满，BPM/状态为侧栏 | CSS Grid 两栏布局 |

### 移动端布局 (< 768px) — "BPM 优先"

手机屏幕小，将最关键的 BPM 数值放大置顶，波形图占次要高度：

```
┌───────────────────────────┐
│  ◀ 375px (iPhone 宽) ▶    │
│                           │
│  ┌─────────────────────┐  │
│  │     ❤️  72          │  │  ← BPM 字体 72px，绝对视觉中心
│  │     次/分           │  │  ← 单位小字
│  │  🟢 正常            │  │  ← 心率区间标签 (颜色随 BPM 变化)
│  └─────────────────────┘  │
│                           │
│  ┌─────────────────────┐  │
│  │ ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  │  │  ← ECG 网格
│  │    ╱╲    ╱╲         │  │
│  │   ╱  ╲  ╱  ╲  ╱╲   │  │  ← 绿色波形, 高度 200px
│  │ ─╯    ╲╱    ╲╱  ╲── │  │
│  │ ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔  │  │
│  └─────────────────────┘  │
│                           │
│  ⚡ 导联正常 │ 📶 在线     │  ← 底部状态栏
└───────────────────────────┘
```

移动端 CSS 关键约束：

- BPM 区域固定在顶部，波形 Canvas 高度固定 200px
- 总内容高度不超出视口 (`100vh`)，避免滚动
- 手指触摸区域 ≥ 44px（符合 iOS HIG）

### PC 端布局 (≥ 768px) — "ECG 图像主体"

PC/平板屏幕宽裕，ECG 波形图应占据视觉主导地位，BPM 和状态信息作为辅助侧栏：

```
┌──────────────────────────────────────────────────────────┐
│  ◀ 1280px+ ▶                                             │
│                                                          │
│  ┌─────────────────────────────────────┬──────────────┐  │
│  │                                     │              │  │
│  │  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈     │  ❤️  72      │  │
│  │  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈     │  次/分       │  │
│  │  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈     │              │  │
│  │         ╱╲      ╱╲               │  🟢 正常     │  │  ← 右侧信息面板
│  │        ╱  ╲    ╱  ╲    ╱╲        │              │  │    (固定宽度 240px)
│  │  ─────╯    ╲──╯    ╲──╯  ╲─────  │  RR: 833ms   │  │
│  │  ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔     │              │  │
│  │  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈     │  ⚡ 导联正常 │  │
│  │  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈     │  📶 在线    │  │
│  │  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈     │              │  │
│  │                                     │              │  │
│  │    ◀ ECG Canvas 全高度铺满 ▶       │              │  │
│  │    flex: 1，自动占满剩余空间        │              │  │
│  └─────────────────────────────────────┴──────────────┘  │
│                                                          │
│  ← 左侧: Canvas (flex: 1 填充) →  ← 右侧: 侧栏 240px →  │
└──────────────────────────────────────────────────────────┘
```

PC 端 CSS 关键约束：

```
整体: display: grid; grid-template-columns: 1fr 240px; height: 100vh;
左侧: Canvas 占满整个 grid cell, resize 事件动态更新 Canvas 尺寸
右侧: 信息面板, flexbox 纵向排列, 垂直居中
```

### 浏览器端 JS 架构

前端页面承载了所有信号处理逻辑，代码按职责分层：

```
┌─────────────────────────────────────────────┐
│              index_html.h 单文件              │
│                                             │
│  ┌───────────────────────────────────────┐  │
│  │  WebSocket 层                         │  │
│  │  · 连接 ws://192.168.4.1/ws          │  │
│  │  · onmessage: JSON.parse →           │  │
│  │    samples → ringBuffer[]             │  │
│  │    bpm_ref → 交叉校验模块              │  │
│  └───────────────┬───────────────────────┘  │
│                  │                          │
│  ┌───────────────▼───────────────────────┐  │
│  │  DSP 流水线 (Pan-Tompkins 主力)       │  │
│  │  · FIR 带通滤波 (32阶)               │  │
│  │  · 差分 + 平方 + 滑动窗口积分        │  │
│  │  · 自适应阈值 R 峰检测               │  │
│  │  · BPM 移动平均 (最近8次)            │  │
│  │  输出: { bpm, rr, hrZone }            │  │
│  └───────────────┬───────────────────────┘  │
│                  │                          │
│  ┌───────────────▼───────────────────────┐  │
│  │  交叉校验                              │  │
│  │  · 比较 bpm (JS主力) vs bpm_ref (ESP) │  │
│  │  · diff≤5→✅, 5-10→⚠️, >10→❌        │  │
│  │  输出: { validation, displayBPM }      │  │
│  └───────────────┬───────────────────────┘  │
│                  │                          │
│  ┌───────────────▼───────────────────────┐  │
│  │  渲染层                               │  │
│  │  · Canvas 2D ECG 波形绘制             │  │
│  │  · 主力 BPM 大字 + bpm_ref 副显示     │  │
│  │  · 校验状态图标 (✅/⚠️/❌)            │  │
│  │  · 心跳动画 + 导联状态                │  │
│  └───────────────────────────────────────┘  │
│                                             │
└─────────────────────────────────────────────┘
```

**数据驱动更新模型**：

```
WebSocket 消息到达
  → samples 追加到 ringBuffer (1250 点 / 5秒)
  → 路径 B: DSP 流水线处理 → 若 R 波: 更新 bpm, rr, hrZone
  → 交叉校验: |bpm - bpm_ref| → 更新 validation 状态
  → requestAnimationFrame 每帧绘制 Canvas 波形
  → BPM DOM 仅在值变化时更新
```

### CSS 响应式实现要点

```css
/* 基础：移动端优先 (Mobile First) */
body {
    display: flex;
    flex-direction: column;
    height: 100vh;
    background: #0a0a0f;
}

.bpm-panel {
    text-align: center;
    padding: 16px 0;
}

.bpm-value {
    font-size: 72px;
    font-weight: 700;
}

.ecg-container {
    flex: 1;                      /* 移动端：占据 BPM 面板下方剩余空间 */
    min-height: 200px;
}

.status-bar {
    display: flex;
    justify-content: space-around;
    padding: 12px 0;
}

/* PC 端断点 ≥ 768px */
@media (min-width: 768px) {
    body {
        display: grid;
        grid-template-columns: 1fr 240px;   /* Canvas 自适应 + 固定侧栏 */
        grid-template-rows: 1fr;
    }

    .ecg-container {
        grid-column: 1;
        grid-row: 1;
        min-height: 0;             /* 允许 grid item 正常伸缩 */
    }

    .side-panel {
        grid-column: 2;
        grid-row: 1;
        display: flex;
        flex-direction: column;
        justify-content: center;   /* BPM + 状态垂直居中 */
        align-items: center;
        border-left: 1px solid rgba(0,255,136,0.2);
        padding: 24px;
    }

    .bpm-value {
        font-size: 96px;           /* PC 端 BPM 更大 */
    }

    .status-bar {
        flex-direction: column;
        gap: 16px;
    }
}
```

### Canvas 波形渲染方案

Canvas 元素在两种布局下的行为不同：

| 属性 | 移动端 | PC 端 |
|------|--------|-------|
| Canvas 宽度 | 屏幕宽度 (100vw) | 父容器宽度 (`1fr`，随窗口 resize) |
| Canvas 高度 | 固定 200px | 视口全高 (100vh)，即 grid row 的全高 |
| 网格密度 | 4 大格 × 细格 | 6 大格 × 细格（更宽的视野）|
| 波形缓冲长度 | ~500 个采样点 (≈2 秒) | ~1250 个采样点 (≈5 秒) |
| 线宽 | 2px | 3px |
| 发光效果 | shadowBlur: 4 | shadowBlur: 6 |

PC 端波形视野更长（5 秒 vs 2 秒），因为更大的 Canvas 宽度可以容纳更多历史采样点，方便观察心律的整体模式。

```javascript
// 渲染循环 (与 WebSocket 接收解耦)
function render() {
    const ctx = canvas.getContext('2d');

    // 1. 清空画布
    ctx.fillStyle = '#0a0a0f';
    ctx.fillRect(0, 0, width, height);

    // 2. 绘制网格线 (模拟心电图纸)
    drawGrid(ctx);      // 细线 = 小格，粗线 = 大格

    // 3. 绘制 ECG 波形
    ctx.beginPath();
    ctx.strokeStyle = '#00ff88';      // 霓虹绿
    ctx.lineWidth = isPC ? 3 : 2;
    ctx.shadowBlur = isPC ? 6 : 4;    // 发光效果
    ctx.shadowColor = '#00ff88';

    for (let i = 1; i < buffer.length; i++) {
        const x = (i / buffer.length) * width;
        const y = mapSampleToY(buffer[i]);
        ctx.lineTo(x, y);
    }
    ctx.stroke();

    // 4. 请求下一帧
    requestAnimationFrame(render);
}

// 窗口 resize 时重设 Canvas 尺寸
window.addEventListener('resize', () => {
    canvas.width = canvas.parentElement.clientWidth;
    canvas.height = canvas.parentElement.clientHeight;
});
```

### CSS 动效

| 动效 | 描述 | 实现方式 |
|------|------|----------|
| **BPM 心跳动画** | BPM 数值 `scale(1.0 → 1.05 → 1.0)`，周期与实际心率同步 | `animation-duration: ${60000/bpm}ms` 动态更新 |
| **波形发光** | Canvas shadowBlur 霓虹绿发光 | PC 端 blur=6，移动端 blur=4 |
| **导联脱落警告** | 红色闪烁遮罩层 + "电极脱落" 大字 | CSS `@keyframes blink`，`opacity` 交替 |
| **BPM 颜色过渡** | 心率区间切换时颜色渐变 | `transition: color 0.6s ease` |
| **侧栏滑入** | PC 端右侧面板加载时从右滑入 | `@keyframes slideInRight` 0.3s ease-out |

### 颜色方案

| 元素 | 颜色 | 说明 |
|------|------|------|
| 背景 | `#0a0a0f` | 深黑蓝，低光环境不刺眼 |
| 波形 | `#00ff88` | 霓虹绿，经典 ECG 风格 |
| 网格 | `rgba(0,255,136,0.15)` | 淡绿半透明 |
| BPM 正常 (60–100) | `#00ff88` | 绿色 |
| BPM 偏高/低 (50–59, 101–120) | `#ffb700` | 黄色 |
| BPM 危险 (<50, >120) | `#ff4444` | 红色 |
| 文字/侧栏边框 | `#cccccc` / `rgba(0,255,136,0.2)` | 浅灰 + 淡绿分隔

---

## 代码结构

```
HeartRateMonitor/
├── README.md                   ← 本文件
├── platformio.ini              ← PlatformIO 项目配置
├── src/
│   ├── main.cpp                ← 入口: WiFi 初始化、任务创建
│   ├── ecg_sensor.h            ← ECG 传感器模块头文件
│   ├── ecg_sensor.cpp          ← ADC 驱动、定时采样、导联检测
│   ├── web_server.h            ← Web 服务器模块头文件
│   ├── web_server.cpp          ← WiFi AP、HTTP 路由、WebSocket
│   └── index_html.h            ← 内嵌 HTML/CSS/JS 页面
├── include/
│   └── README                  ← (保留)
├── lib/
│   └── README                  ← (保留)
└── test/
    └── README                  ← (保留)
```

### 模块职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | `setup()` 初始化各模块；`loop()` 空闲，RTOS 任务接管 |
| `ecg_sensor.h/cpp` | 硬件定时器配置；ADC 采样 ISR；环形缓冲区读写；SimplePeakDetector 极简阈值 R 波检测 + 参考 BPM 计算；导联脱落 GPIO 检测 |
| `web_server.h/cpp` | `WiFi.softAP()` 配置；`AsyncWebServer` 路由；`AsyncWebSocket` 事件处理；每 50ms 定时读取缓冲区 + bpm_ref + 导联状态，打包 JSON 推送 |
| `index_html.h` | 作为 `const char index_html[] PROGMEM` 嵌入的完整前端页面，包含：CSS 响应式布局 + Canvas 波形渲染 + WebSocket 接收 + **JavaScript Pan-Tompkins 主力 DSP** + **交叉校验逻辑** |

---

## 搭建与烧录

### 1. 安装 PlatformIO

```bash
# VS Code 扩展安装
# 或命令行:
pip install platformio
```

### 2. 编译

```bash
cd /path/to/HeartRateMonitor
pio run
```

### 3. 烧录

```bash
# USB 连接 ESP32-S3 后:
pio run -t upload

# 如果烧录失败，按住 BOOT 键再按 RST，然后:
pio run -t upload
```

### 4. 查看串口日志

```bash
pio device monitor
```

### 5. 使用

1. ESP32-S3 上电后，手机/PC Wi-Fi 列表中会出现 `ECG-Monitor`
2. 连接这个 Wi-Fi（无密码）
3. 浏览器打开 `http://192.168.4.1`
   - **手机**：自动展示移动端布局，BPM 大字置顶，波形紧凑排列
   - **PC/平板**：自动切换为 ECG 全屏模式，波形主视图铺满左侧，BPM + 状态在右侧面板
4. 贴好电极，等待几秒稳定后即可看到心电图波形

---

## 使用说明

### 电极贴放指导

1. **RA (右臂)** — 贴在右锁骨下方约 5cm 处
2. **LA (左臂)** — 贴在左锁骨下方约 5cm 处
3. **RL (右腿)** — 贴在右下腹/右肋下方（参考地驱动）

### 注意事项

- **皮肤准备**：用酒精棉擦拭贴放部位，去除油脂和死皮，信号质量更好
- **保持静止**：测量时尽量保持静止，减少肌电干扰
- **电极寿命**：一次性电极片开封后应在几小时内使用完毕
- **环境**：远离大功率电器（马达、微波炉），减少 50Hz 工频干扰

### 信号异常排查

| 现象 | 可能原因 | 解决 |
|------|---------|------|
| 波形平坦/无波动 | 电极脱落/接触不良 | 检查 LO+/LO- 指示灯，重贴电极 |
| 波形噪声大/毛刺多 | 肌肉紧张/电极老化 | 放松身体，更换电极 |
| 波形持续漂移 | 电极与皮肤之间建立电化学平衡 | 贴好后等待 30 秒稳定 |
| BPM 显示 0 | 尚未检测到有效 R 波 | 确认电极贴放位置，等待信号稳定 |
| 波形 50Hz 周期性干扰 | 工频干扰 | 远离电源插座和电器 |

---

## 后续扩展方向

当前 MVP 版本聚焦于核心功能：**ESP32 ADC 采样 + 极简阈值 BPM → WebSocket 推送原始数据 + bpm_ref → 浏览器端 Pan-Tompkins 主力 DSP → 双路径交叉校验 → Canvas 实时渲染**。后续可扩展：

| 扩展方向 | 说明 |
|----------|------|
| **电池供电** | 锂电池 + 充放电管理模块，配合 ESP32-S3 深度睡眠降低功耗 |
| **数据记录** | MicroSD 长时间记录 ECG 原始数据，用于离线分析 |
| **HRV 分析** | SDNN / RMSSD 等心率变异性指标，评估自主神经状态 |
| **MQTT 上云** | 数据推送到云端平台（ThingsBoard）做长期存储和可视化 |
| **异常告警** | BPM 超出阈值 → LED/蜂鸣器/手机通知 |
| **多设备支持** | WebSocket 广播，多个手机/平板同时查看 |
| **BLE 替代方案** | 低功耗蓝牙透传模式，适配 React Native / Flutter App |
| **OTA 固件升级** | 支持 WiFi 无线更新固件 |

---