/**
 * Steins;Gate — 世界线变动探测仪 + NTP 时钟
 * ESP32 + 8位数码管 (共阴极)
 *
 * 周期:
 *   SCROLLING → SETTLING → STABLE(1s) → SHOW_CLOCK(5s) → 循环
 *
 * 时钟格式: HH-MM-SS (横线用 g 段, 8位全部用上)
 *
 *   H  H  -  M  M  -  S  S
 *  [ ] [ ] _ [ ] [ ] _ [ ] [ ]
 *   0   1  2  3   4  5  6   7
 *
 * 共阴极极性:
 *   段选: HIGH=亮, LOW=灭
 *   位选: LOW=开,  HIGH=关
 *
 * 段选 (a b c d e f g dp):
 *   13, 12, 14, 27, 26, 25, 33, 32
 *
 * 位选 (左→右 1~8):
 *   15, 2, 4, 16, 17, 21, 22, 23
 */

#include <WiFi.h>
#include <time.h>

// ============================================================
// WiFi 配置
// ============================================================
const char* WIFI_SSID = "test";
const char* WIFI_PASS = "27182818";

// NTP 服务器 (阿里云 + 标准池, 双保险)
const char* NTP_SERVER1 = "ntp.aliyun.com";
const char* NTP_SERVER2 = "pool.ntp.org";
const long   GMT_OFFSET = 8 * 3600;   // UTC+8 (北京时间)
const int    DST_OFFSET = 0;          // 无夏令时

// ============================================================
// 引脚
// ============================================================
const int segA  = 13;
const int segB  = 12;
const int segC  = 14;
const int segD  = 27;
const int segE  = 26;
const int segF  = 25;
const int segG  = 33;
const int segDP = 32;

const int SEG_PINS[] = {segA, segB, segC, segD, segE, segF, segG, segDP};

const int DIG_PINS[] = {15, 2, 4, 16, 17, 21, 22, 23};
const int NUM_DIGITS = 8;

// ============================================================
// 7段字形: 每个数字需要点亮的段引脚列表, 以 -1 结尾
// ============================================================
const int DIGIT_SEGMENTS[10][8] = {
  {segA, segB, segC, segD, segE, segF, -1},             // 0
  {segB, segC, -1},                                      // 1
  {segA, segB, segD, segE, segG, -1},                    // 2
  {segA, segB, segC, segD, segG, -1},                    // 3
  {segB, segC, segF, segG, -1},                         // 4
  {segA, segC, segD, segF, segG, -1},                   // 5
  {segA, segC, segD, segE, segF, segG, -1},             // 6
  {segA, segB, segC, -1},                                // 7
  {segA, segB, segC, segD, segE, segF, segG, -1},       // 8
  {segA, segB, segC, segD, segF, segG, -1}              // 9
};

#define BLANK_DIGIT 0xFF   // 空白位 (全部段灭)
#define DASH_DIGIT  0xFE   // 横线位 (只亮 g 段)

// ============================================================
// 特殊世界线 (5% 概率出现, 其余完全随机)
// ============================================================
const float SPECIAL_LINES[] = {
  1.048596,   // Steins Gate
  0.571024,   // Alpha 世界线
  0.000000,   // 零世界线
};
const int NUM_SPECIAL = sizeof(SPECIAL_LINES) / sizeof(SPECIAL_LINES[0]);

// ============================================================
// 全局状态
// ============================================================
uint8_t targetDigits[NUM_DIGITS];    // 目标数字 (探测仪=世界线, 时钟=时分秒)
uint8_t displayBuffer[NUM_DIGITS];   // 动画缓冲
bool    dotFlags[NUM_DIGITS];        // 小数点
float   targetDivergence;

bool    wifiConnected = false;
struct tm timeinfo;
int     lastDisplayedSecond = -1;    // 上次刷新时钟的秒数

// ============================================================
// 动画状态机
// ============================================================
enum AnimState {
  SCROLLING,     // 辉光管混沌闪烁
  SETTLING,      // 逐位减速锁定
  STABLE,        // 显示世界线 1s
  SHOW_CLOCK     // 显示时钟 10s
};
AnimState state = SCROLLING;

unsigned long stateStartTime   = 0;
unsigned long scrollDuration   = 0;
const unsigned long CLOCK_DURATION = 5000;   // 时钟显示 5 秒

// 滚动阶段
unsigned long digitLastChange[NUM_DIGITS];
int  digitInterval[NUM_DIGITS];

// 锁定阶段
int  settleIndex = 0;
enum SettleStep { STEP_FLASH1, STEP_FLASH2, STEP_HOLD };
SettleStep settleStep = STEP_FLASH1;
unsigned long settleStepStart = 0;
bool   settleDone[NUM_DIGITS];
unsigned long lastFlashChange = 0;

// ============================================================
// 辅助函数
// ============================================================

void allSegmentsOff() {
  for (int i = 0; i < 8; i++) digitalWrite(SEG_PINS[i], LOW);
}

void allDigitsOff() {
  for (int i = 0; i < NUM_DIGITS; i++) digitalWrite(DIG_PINS[i], HIGH);
}

// 显示某一位: 设置段 → 开位 → 延时 → 关位
void showDigit(int pos, uint8_t number, bool dot, unsigned long holdUs) {
  allDigitsOff();
  allSegmentsOff();

  if (number != BLANK_DIGIT && number != DASH_DIGIT) {
    for (int i = 0; DIGIT_SEGMENTS[number][i] != -1; i++) {
      digitalWrite(DIGIT_SEGMENTS[number][i], HIGH);
    }
    if (dot) digitalWrite(segDP, HIGH);
  } else if (number == DASH_DIGIT) {
    // 横线分隔符: 只亮 g 段
    digitalWrite(segG, HIGH);
  }
  // BLANK_DIGIT → 段全部灭

  digitalWrite(DIG_PINS[pos], LOW);
  delayMicroseconds(holdUs);
  digitalWrite(DIG_PINS[pos], HIGH);
}

// ============================================================
// WiFi 连接 (带超时, 不阻塞)
// ============================================================
void connectWiFi() {
  Serial.print("连接 WiFi: ");
  Serial.println(WIFI_SSID);

  // 强制 STA 模式, 清除残留状态
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // 最多等 15 秒 (2.4G only, ESP32 不支持 5G)
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  wl_status_t status = WiFi.status();
  Serial.println();

  if (status == WL_CONNECTED) {
    wifiConnected = true;
    Serial.print("成功! IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("信号强度: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    wifiConnected = false;
    Serial.print("失败, 状态码: ");
    Serial.println(status);
    // 常见原因:
    //   1 = WL_NO_SSID_AVAIL  → SSID 搜不到 (确认是 2.4G WiFi)
    //   4 = WL_CONNECT_FAILED → 密码错误
    //   6 = WL_DISCONNECTED   → 信号太弱或被拒绝
  }
}

// ============================================================
// 更新时钟显示缓冲
// ============================================================
void updateClockBuffer() {
  if (!wifiConnected) return;

  if (!getLocalTime(&timeinfo)) {
    // NTP 尚未同步
    return;
  }

  // 只在秒数变化时更新 (避免每 16ms 刷新浪费)
  if (timeinfo.tm_sec == lastDisplayedSecond) return;
  lastDisplayedSecond = timeinfo.tm_sec;

  int h = timeinfo.tm_hour;
  int m = timeinfo.tm_min;
  int s = timeinfo.tm_sec;

  //  位0  位1   位2     位3  位4   位5     位6  位7
  //   Ht  Ho   -g-    Mt  Mo   -g-    St  So
  targetDigits[0] = h / 10;
  targetDigits[1] = h % 10;
  targetDigits[2] = DASH_DIGIT;    // 横线 (g段)
  targetDigits[3] = m / 10;
  targetDigits[4] = m % 10;
  targetDigits[5] = DASH_DIGIT;    // 横线 (g段)
  targetDigits[6] = s / 10;
  targetDigits[7] = s % 10;

  // 不用小数点
  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
}

// ============================================================
// 初始化
// ============================================================
void setup() {
  Serial.begin(115200);

  // 显示引脚
  for (int i = 0; i < 8; i++) {
    pinMode(SEG_PINS[i], OUTPUT);
    digitalWrite(SEG_PINS[i], LOW);
  }
  for (int i = 0; i < NUM_DIGITS; i++) {
    pinMode(DIG_PINS[i], OUTPUT);
    digitalWrite(DIG_PINS[i], HIGH);
  }

  randomSeed(analogRead(0));

  // WiFi + NTP
  connectWiFi();
  if (wifiConnected) {
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER1, NTP_SERVER2);
    Serial.println("NTP 对时中...");
  }

  // 启动第一轮探测仪
  initNewCycle();

  Serial.println("=== 世界线变动探测仪 启动 ===");
  Serial.println("El Psy Kongroo.");
}

// ============================================================
// 开始新一轮探测仪动画
// ============================================================
void initNewCycle() {
  // 5% 概率显示经典世界线, 95% 完全随机
  if (random(100) < 5) {
    targetDivergence = SPECIAL_LINES[random(NUM_SPECIAL)];
    long raw = (long)(targetDivergence * 10000000.0 + 0.5);
    for (int i = 7; i >= 0; i--) {
      targetDigits[i] = raw % 10;
      raw /= 10;
    }
  } else {
    // 真随机: 第0位 0-3, 其余 0-9 (范围 0.0000000 ~ 3.9999999)
    targetDigits[0] = random(4);  // 0,1,2,3
    for (int i = 1; i < NUM_DIGITS; i++) {
      targetDigits[i] = random(10);
    }
  }

  // displayBuffer 初始化为随机数 (滚动效果)
  for (int i = 0; i < NUM_DIGITS; i++) {
    displayBuffer[i] = random(10);
  }

  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
  dotFlags[0] = true;   // X.XXXXXXX

  for (int i = 0; i < NUM_DIGITS; i++) settleDone[i] = false;
  settleIndex = 0;
  settleStep  = STEP_FLASH1;

  for (int i = 0; i < NUM_DIGITS; i++) {
    digitInterval[i]   = random(30, 80);
    digitLastChange[i] = 0;
  }

  scrollDuration = random(1500, 2000);
  state = SCROLLING;
  stateStartTime = millis();

  Serial.print("目标世界线: ");
  for (int i = 0; i < NUM_DIGITS; i++) {
    Serial.print(targetDigits[i]);
    if (i == 0) Serial.print(".");
  }
  Serial.println();
}

// ============================================================
// 主循环
// ============================================================
void loop() {
  static int currentDig = 0;
  unsigned long now = millis();

  // ---- 判断该位状态 ----
  bool isScrolling  = false;
  bool isShowingClock = (state == SHOW_CLOCK);

  if (state == SCROLLING && !settleDone[currentDig]) {
    isScrolling = true;
  } else if (state == SETTLING && !settleDone[currentDig] && currentDig != settleIndex) {
    isScrolling = true;
  }

  // 辉光管闪烁 (仅滚动时)
  bool nixieBlank = isScrolling && (random(100) < 8);

  // 数字滚动
  if (isScrolling && now - digitLastChange[currentDig] >= (unsigned long)digitInterval[currentDig]) {
    displayBuffer[currentDig] = random(10);
    digitLastChange[currentDig] = now;
    digitInterval[currentDig] = random(30, 80);
  }

  // ---- 显示当前位 ----
  if (nixieBlank) {
    // 辉光管熄灭
    allDigitsOff();
    allSegmentsOff();
    digitalWrite(DIG_PINS[currentDig], LOW);
    delayMicroseconds(1800);
    digitalWrite(DIG_PINS[currentDig], HIGH);
  } else if (isScrolling) {
    showDigit(currentDig, displayBuffer[currentDig], dotFlags[currentDig], 1800);
  } else if (state == SETTLING && currentDig == settleIndex) {
    showDigit(currentDig, displayBuffer[currentDig], dotFlags[currentDig], 1800);
  } else if (isShowingClock) {
    // 时钟模式: 显示 targetDigits (已被 updateClockBuffer 填充)
    showDigit(currentDig, targetDigits[currentDig], dotFlags[currentDig], 1800);
  } else {
    // STABLE / 已锁定 → 显示目标世界线
    showDigit(currentDig, targetDigits[currentDig], dotFlags[currentDig], 1800);
  }

  // ---- 下一位置 ----
  currentDig = (currentDig + 1) % NUM_DIGITS;

  // ---- 每扫描一圈更新动画 ----
  if (currentDig == 0) {
    updateAnimation(now);

    // 时钟模式下, 每秒刷新一次时间
    if (state == SHOW_CLOCK) {
      updateClockBuffer();
    }
  }
}

// ============================================================
// 动画状态机
// ============================================================
void updateAnimation(unsigned long now) {

  switch (state) {

    // ============================================================
    // SCROLLING → SETTLING
    // ============================================================
    case SCROLLING:
      if (now - stateStartTime >= scrollDuration) {
        state = SETTLING;
        stateStartTime   = now;
        settleIndex      = 0;
        settleStep       = STEP_FLASH1;
        settleStepStart  = now;
        lastFlashChange  = now;
        Serial.println("→ SETTLING");
      }
      break;

    // ============================================================
    // SETTLING: 逐位减速锁定 (~2.4s)
    // ============================================================
    case SETTLING: {
      if (settleIndex >= NUM_DIGITS) {
        for (int i = 0; i < NUM_DIGITS; i++) displayBuffer[i] = targetDigits[i];
        state = STABLE;
        stateStartTime = now;
        Serial.println("→ STABLE (1s)");
        break;
      }

      unsigned long elapsed = now - settleStepStart;

      switch (settleStep) {

        case STEP_FLASH1:
          if (elapsed < 50) {
            unsigned long ci = (elapsed < 20) ? 8UL : 25UL;
            if (now - lastFlashChange >= ci) {
              displayBuffer[settleIndex] = random(10);
              lastFlashChange = now;
            }
          } else {
            displayBuffer[settleIndex] = random(10);
            settleStep = STEP_FLASH2;
            settleStepStart = now;
            lastFlashChange = now;
          }
          break;

        case STEP_FLASH2:
          if (elapsed < 100) {
            unsigned long ci = (elapsed < 40) ? 20UL : 60UL;
            if (now - lastFlashChange >= ci) {
              displayBuffer[settleIndex] = random(10);
              lastFlashChange = now;
            }
          } else {
            displayBuffer[settleIndex] = targetDigits[settleIndex];
            settleStep = STEP_HOLD;
            settleStepStart = now;
          }
          break;

        case STEP_HOLD:
          if (elapsed >= 150) {
            settleDone[settleIndex] = true;
            displayBuffer[settleIndex] = targetDigits[settleIndex];
            settleIndex++;
            settleStep = STEP_FLASH1;
            settleStepStart = now;
            lastFlashChange = now;
          }
          break;
      }
      break;
    }

    // ============================================================
    // STABLE (1s) → SHOW_CLOCK 或 SCROLLING
    // ============================================================
    case STABLE:
      if (now - stateStartTime >= 1000) {
        if (wifiConnected) {
          // 初始化时钟缓冲并进入时钟模式
          lastDisplayedSecond = -1;   // 强制立即刷新
          updateClockBuffer();
          state = SHOW_CLOCK;
          stateStartTime = now;
          Serial.println("→ SHOW_CLOCK (10s)");
        } else {
          // WiFi 没连上, 跳过时钟直接下一轮
          initNewCycle();
          Serial.println("→ SCROLLING (无WiFi,跳过时钟)");
        }
      }
      break;

    // ============================================================
    // SHOW_CLOCK (10s) → 新一轮探测仪
    // ============================================================
    case SHOW_CLOCK:
      if (now - stateStartTime >= CLOCK_DURATION) {
        initNewCycle();
        Serial.println("→ SCROLLING (新一轮探测仪)");
      }
      break;
  }
}
