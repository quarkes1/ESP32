# ST7789 屏幕调试记：一个 `-include` 引发的血案

> **日期**: 2026-07-10  
> **涉及项目**: `SekaisenDetector` (世界线变动探测仪)  
> **硬件**: ESP32 Dev Board + ST7789 240×240 IPS 屏 (7-pin 无 CS) + 8位共阴极数码管  
> **结论**: 屏幕没坏，是 TFT_eSPI 库在 PlatformIO 下的配置方式有问题。

---

## 背景

在已有的"世界线变动探测仪"项目基础上扩展 ST7789 彩屏。数码管已焊死，占用引脚如下：

| 功能 | 引脚 |
|------|------|
| 段选 | 13, 12, 14, 27, 26, 25, 33, 32 |
| 位选 | 15, 2, 4, 16, 17, 21, 22, 23 |

剩余可用 GPIO：5, 18, 19（以及 0/1/3 等特殊情况）。

---

## 接线方案

```
ST7789 (7-pin 无 CS) → ESP32
─────────────────────────────
GND          → GND
VCC          → 3.3V
SCK          → GPIO 18
SDA (MOSI)   → GPIO 5
RES          → 3.3V（最可靠，见下文）
DC           → GPIO 19
BLK          → 3.3V
```

---

## 故障症状

1. **初期**：偶尔能显示颜色块和几何图形，但文字始终渲染不出来。RES 最初接的是 EN 引脚。
2. **中期**：变间歇性，RES 在 EN 和 3.3V 之间反复切换后彻底黑屏，只有背光。
3. **后期**：换新开发板，换了所有杜邦线，降 SPI 频率到 1MHz，面包板直连——依然只有背光。

---

## 调试过程（按时间线）

### 第一阶段：以为是显示配置问题

TFT_eSPI 的配置通过 `src/User_Setup.h` 文件提供，在 `platformio.ini` 中用：

```ini
build_flags =
    -DUSER_SETUP_LOADED=1
    -include src/User_Setup.h
```

- 能看到 RGB 纯色和矩形方框 → **SPI 通信和基本绘图正常**
- 但 `drawString()`、`drawChar()`、`print()` 全部无效 → 以为是字体渲染 bug
- 试了 `invertDisplay(true)`、不同字体号、`setTextDatum`、`print` 替代 `drawString`
- 全部无效

### 第二阶段：以为是 RES 引脚问题

最初 RES 接 EN。EN 是 ESP32 的复位**输入**脚，不是稳定电源。噪声或负载波动会导致显示屏反复复位。

先后试了：
- RES → EN（间歇性失败）
- RES → 3.3V（略好但仍间歇）
- RES → GPIO 0（板子没引出这个脚）

这个方向并没有解决根本问题，但 RES 接 3.3V 比接 EN 更稳定。

### 第三阶段：以为是 GPIO 18 损坏

用 LED 测试 GPIO 5 和 18 均有输出，但接入屏幕 SPI 后 GPIO 18 表现异常。将 MOSI 和 SCK 互换（GPIO 5 跑时钟，GPIO 18 跑数据）也无改善。

### 第四阶段：以为是屏幕硬件损坏

换成 Adafruit 官方的 ST7789 库重新测试——仍然只有背光。几乎要认定屏幕模块的 SPI 接口烧毁了。

### 第五阶段：关键突破 — st7789test 项目

用另一块开发板跑了一个全新的 st7789test 项目，**同一个屏幕模块**，完全正常显示。屏幕没有坏。

---

## 根因分析

对比两个项目的 `platformio.ini`：

### 失败的 SekaisenDetector

```ini
build_flags =
    -DUSER_SETUP_LOADED=1
    -include src/User_Setup.h     # ← 元凶
```

### 成功的 st7789test

```ini
build_flags =
    -DUSER_SETUP_LOADED
    -DST7789_DRIVER
    -DTFT_WIDTH=240
    -DTFT_HEIGHT=240
    -DTFT_MOSI=23
    -DTFT_SCLK=18
    -DTFT_CS=-1
    -DTFT_DC=2
    -DTFT_RST=4
    -DTFT_BL=15
    -DSPI_FREQUENCY=40000000
    # ... 全部用 -D 定义
```

### `-include` 为什么在 PlatformIO 下会失败？

GCC 的 `-include file` 指令在编译前强制包含一个文件，但**文件路径是相对于每个编译单元的工作目录解析的**。

PlatformIO 编译不同源文件时的工作目录不同：

| 编译单元 | 工作目录 | `-include src/User_Setup.h` 解析结果 |
|----------|----------|-------------------------------------|
| `src/main.cpp` | 项目根目录 `/path/to/project/` | `/path/to/project/src/User_Setup.h` ✅ **找到** |
| `.pio/libdeps/.../TFT_eSPI.cpp` | 库目录 `.pio/libdeps/.../TFT_eSPI/` | `.pio/libdeps/.../TFT_eSPI/src/User_Setup.h` ❌ **不存在** |

**结果**：
- `main.cpp` 拿到了正确的引脚定义（因为 `src/User_Setup.h` 在它的工作目录下能被找到）
- `TFT_eSPI.cpp` **没拿到任何配置**——`-include` 找不到文件，加上 `USER_SETUP_LOADED` 阻止了库自带的默认配置
- 库被编译时用了**垃圾配置**（未定义的宏、错误的默认值）
- 你的代码和库代码对引脚的理解**完全不同**，数据根本没发到正确的 GPIO 上

这就是为什么：
- **颜色块能显示、文字不行**——`fillRect` 和 `drawString` 走的是不同的代码路径，碰巧某些指令在错误配置下也能到达屏幕，但文字渲染不行
- **间歇性成功**——编译缓存未清时，有些 `.o` 文件碰巧拿到了配置
- **完全失败**——`pio run -t clean` 后全部重新编译，所有库文件都用错误配置

### 为什么 `-D` 是正确的？

`-D` 定义的是**全局预处理器宏**，不论哪个编译单元、哪个工作目录，宏都在那里。库代码和用户代码看到的是**同一个值**。

---

## 经验教训

### 1. PlatformIO + TFT_eSPI 的正确配置方式

**永远用 `-D` 宏，不要用 `-include`**：

```ini
build_flags =
    -DUSER_SETUP_LOADED
    -DST7789_DRIVER
    -DTFT_WIDTH=240
    -DTFT_HEIGHT=240
    -DTFT_MOSI=23
    -DTFT_SCLK=18
    -DTFT_CS=-1
    -DTFT_DC=2
    -DTFT_RST=4
    -DTFT_BL=15
    -DSPI_FREQUENCY=40000000
```

不需要 `User_Setup.h` 文件。

### 2. ST7789 RES 引脚的正确接法

| 接法 | 可靠性 | 说明 |
|------|--------|------|
| **3.3V** | ✅ 最可靠 | 模块内部有 RC 复位电路，上电自动复位 |
| GPIO | ✅ 可靠 | 库可以软件控制复位脉冲 |
| **EN** | ❌ 危险 | EN 是输入脚不是电源，电平不稳可能损坏模块 |

### 3. 面包板 + SPI = 降频

面包板和杜邦线的寄生电容大，SPI 频率不要超过 10MHz。稳定后再调高。

### 4. 调试方法论

- 用**颜色块**（`fillScreen`/`fillRect`）做基准确认，比文字更可靠
- 每步停留 1-2 秒，串口同步打印日志
- 怀疑硬件损坏前，**用另一个最简单的项目验证**
- 松线排查：晃线 + 循环闪屏，哪根动了屏就灭了 = 哪根松

---

## 相关文件

- 成功的参考项目：`~/项目/ESP32/st7789test/`
- 世界线探测仪（已还原）：`~/项目/ESP32/SekaisenDetector/`
