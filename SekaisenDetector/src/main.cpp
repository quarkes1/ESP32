#include <Arduino.h>

/**
 * Steins;Gate — 世界线变动探测仪 + NTP 时钟 (节能版)
 * ESP32 + 8位数码管 (共阴极)
 *
 * WiFi 策略:
 *   开机对时成功 → 关WiFi, 每24h对时一次(最多2次尝试,间隔2min)
 *   开机对时失败 → 关WiFi, 每2min重试直到首次成功
 *   其余时间WiFi完全关闭, 仅靠ESP32内部RTC走时
 *
 * 周期:
 *   SCROLLING → SETTLING → STABLE(1s) → SHOW_CLOCK(5s) → 循环
 *
 * 段选: 13,12,14,27,26,25,33,32
 * 位选: 15,2,4,16,17,21,22,23
 */

#include <WiFi.h>
#include <time.h>

// ============================================================
// WiFi / NTP 配置
// ============================================================
const char* WIFI_SSID  = "test";
const char* WIFI_PASS  = "27182818";
const char* NTP_SVR1   = "ntp.aliyun.com";
const char* NTP_SVR2   = "pool.ntp.org";
const long  GMT_OFFSET = 8 * 3600;
const int   DST_OFFSET = 0;

// 对时间隔 (ms)
const unsigned long RETRY_INTERVAL     = 2 * 60000;      // 初始失败: 2分钟
const unsigned long RESYNC_INTERVAL    = 24UL * 3600000;  // 24小时
const unsigned long RESYNC_RETRY_GAP   = 2 * 60000;      // 24h失败: 2分钟后再试
const int           RESYNC_MAX_TRIES   = 2;               // 24h最多试2次

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
// 7段字形
// ============================================================
const int DIGIT_SEGMENTS[10][8] = {
  {segA, segB, segC, segD, segE, segF, -1},
  {segB, segC, -1},
  {segA, segB, segD, segE, segG, -1},
  {segA, segB, segC, segD, segG, -1},
  {segB, segC, segF, segG, -1},
  {segA, segC, segD, segF, segG, -1},
  {segA, segC, segD, segE, segF, segG, -1},
  {segA, segB, segC, -1},
  {segA, segB, segC, segD, segE, segF, segG, -1},
  {segA, segB, segC, segD, segF, segG, -1}
};

#define BLANK_DIGIT 0xFF
#define DASH_DIGIT  0xFE

// ============================================================
// 特殊世界线 (5%概率)
// ============================================================
const float SPECIAL_LINES[] = {
  1.048596, 0.571024, 0.000000,
};
const int NUM_SPECIAL = sizeof(SPECIAL_LINES) / sizeof(SPECIAL_LINES[0]);

// ============================================================
// 全局状态
// ============================================================
uint8_t targetDigits[NUM_DIGITS];
uint8_t displayBuffer[NUM_DIGITS];
bool    dotFlags[NUM_DIGITS];
float   targetDivergence;

// ---- WiFi / NTP ----
bool         ntpSynced     = false;   // NTP是否曾对时成功
unsigned long lastNtpSync  = 0;       // 上次成功对时的 millis()
unsigned long nextWifiTry  = 0;       // 下次尝试WiFi对时的 millis()
int          resyncTries   = 0;       // 24h周期内已尝试次数

struct tm timeinfo;
int     lastDisplayedSecond = -1;

// ============================================================
// 动画状态机
// ============================================================
enum AnimState { SCROLLING, SETTLING, STABLE, SHOW_CLOCK };
AnimState state = SCROLLING;

unsigned long stateStartTime = 0;
unsigned long scrollDuration = 0;
const unsigned long CLOCK_DURATION = 5000;

// 滚动
unsigned long digitLastChange[NUM_DIGITS];
int  digitInterval[NUM_DIGITS];

// 锁定
int  settleIndex = 0;
enum SettleStep { STEP_FLASH1, STEP_FLASH2, STEP_HOLD };
SettleStep settleStep = STEP_FLASH1;
unsigned long settleStepStart = 0;
bool   settleDone[NUM_DIGITS];
unsigned long lastFlashChange = 0;

// ============================================================
// 前置声明 (Arduino .ino → C++ 需要)
// ============================================================
void allSegmentsOff();
void allDigitsOff();
void showDigit(int pos, uint8_t number, bool dot, unsigned long holdUs);
bool tryNtpSync();
void scheduleNextWifi(unsigned long delayMs);
void updateClockBuffer();
void initNewCycle();
void updateAnimation(unsigned long now);
void checkWifiSchedule(unsigned long now);

// ============================================================
// 数码管驱动
// ============================================================
void allSegmentsOff() {
  for (int i = 0; i < 8; i++) digitalWrite(SEG_PINS[i], LOW);
}
void allDigitsOff() {
  for (int i = 0; i < NUM_DIGITS; i++) digitalWrite(DIG_PINS[i], HIGH);
}

void showDigit(int pos, uint8_t number, bool dot, unsigned long holdUs) {
  allDigitsOff();
  allSegmentsOff();
  if (number != BLANK_DIGIT && number != DASH_DIGIT) {
    for (int i = 0; DIGIT_SEGMENTS[number][i] != -1; i++)
      digitalWrite(DIGIT_SEGMENTS[number][i], HIGH);
    if (dot) digitalWrite(segDP, HIGH);
  } else if (number == DASH_DIGIT) {
    digitalWrite(segG, HIGH);
  }
  digitalWrite(DIG_PINS[pos], LOW);
  delayMicroseconds(holdUs);
  digitalWrite(DIG_PINS[pos], HIGH);
}

// ============================================================
// WiFi + NTP 对时 (会短暂阻塞显示 ~数秒)
// ============================================================
bool tryNtpSync() {
  Serial.print("[WiFi] 开启并连接 ");
  Serial.print(WIFI_SSID);
  Serial.print(" ...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int wait = 0;
  while (WiFi.status() != WL_CONNECTED && wait < 30) {
    delay(500);
    Serial.print(".");
    wait++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("[WiFi] 连接失败, 状态码=");
    Serial.println(WiFi.status());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  Serial.print("[WiFi] 已连接, IP=");
  Serial.print(WiFi.localIP());
  Serial.print(" RSSI=");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  // NTP 同步
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SVR1, NTP_SVR2);
  Serial.print("[NTP] 等待同步...");

  int ntpWait = 0;
  struct tm dummy;
  while (!getLocalTime(&dummy) && ntpWait < 40) {   // 最多等 10 秒
    delay(250);
    Serial.print(".");
    ntpWait++;
  }
  Serial.println();

  if (getLocalTime(&dummy)) {
    Serial.printf("[NTP] 对时成功: %04d-%02d-%02d %02d:%02d:%02d\n",
                  dummy.tm_year + 1900, dummy.tm_mon + 1, dummy.tm_mday,
                  dummy.tm_hour, dummy.tm_min, dummy.tm_sec);
  } else {
    Serial.println("[NTP] 对时超时");
  }

  // 无论NTP是否拿到, 先关WiFi
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[WiFi] 已关闭");

  return getLocalTime(&dummy);  // 只有拿到时间才算成功
}

// 安排下次WiFi对时
void scheduleNextWifi(unsigned long delayMs) {
  nextWifiTry = millis() + delayMs;
}

// ============================================================
// 时钟缓冲
// ============================================================
void updateClockBuffer() {
  if (!ntpSynced) return;
  if (!getLocalTime(&timeinfo)) return;
  if (timeinfo.tm_sec == lastDisplayedSecond) return;
  lastDisplayedSecond = timeinfo.tm_sec;

  int h = timeinfo.tm_hour;
  int m = timeinfo.tm_min;
  int s = timeinfo.tm_sec;

  targetDigits[0] = h / 10;
  targetDigits[1] = h % 10;
  targetDigits[2] = DASH_DIGIT;
  targetDigits[3] = m / 10;
  targetDigits[4] = m % 10;
  targetDigits[5] = DASH_DIGIT;
  targetDigits[6] = s / 10;
  targetDigits[7] = s % 10;

  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
}

// ============================================================
// 初始化
// ============================================================
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 8; i++) {
    pinMode(SEG_PINS[i], OUTPUT);
    digitalWrite(SEG_PINS[i], LOW);
  }
  for (int i = 0; i < NUM_DIGITS; i++) {
    pinMode(DIG_PINS[i], OUTPUT);
    digitalWrite(DIG_PINS[i], HIGH);
  }

  randomSeed(analogRead(0));

  // ---- 首次 WiFi 对时 ----
  Serial.println("=== 首次对时 ===");
  if (tryNtpSync()) {
    ntpSynced    = true;
    lastNtpSync  = millis();
    scheduleNextWifi(RESYNC_INTERVAL);   // 24h 后再试
    Serial.printf("[计划] %luh后再次对时\n", RESYNC_INTERVAL / 3600000);
  } else {
    ntpSynced = false;
    scheduleNextWifi(RETRY_INTERVAL);     // 2min 后重试
    Serial.printf("[计划] %dmin后重试\n", (int)(RETRY_INTERVAL / 60000));
  }

  initNewCycle();
  Serial.println("=== 世界线变动探测仪 启动 ===");
  Serial.println("El Psy Kongroo.");
}

// ============================================================
// 新一轮探测仪
// ============================================================
void initNewCycle() {
  if (random(100) < 5) {
    targetDivergence = SPECIAL_LINES[random(NUM_SPECIAL)];
    long raw = (long)(targetDivergence * 10000000.0 + 0.5);
    for (int i = 7; i >= 0; i--) {
      targetDigits[i] = raw % 10;
      raw /= 10;
    }
  } else {
    targetDigits[0] = random(4);
    for (int i = 1; i < NUM_DIGITS; i++) targetDigits[i] = random(10);
  }

  for (int i = 0; i < NUM_DIGITS; i++) displayBuffer[i] = random(10);
  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
  dotFlags[0] = true;
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
// 动画状态机
// ============================================================
void updateAnimation(unsigned long now) {
  switch (state) {

    case SCROLLING:
      if (now - stateStartTime >= scrollDuration) {
        state = SETTLING;
        stateStartTime  = now;
        settleIndex     = 0;
        settleStep      = STEP_FLASH1;
        settleStepStart = now;
        lastFlashChange = now;
      }
      break;

    case SETTLING: {
      if (settleIndex >= NUM_DIGITS) {
        for (int i = 0; i < NUM_DIGITS; i++) displayBuffer[i] = targetDigits[i];
        state = STABLE;
        stateStartTime = now;
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

    case STABLE:
      if (now - stateStartTime >= 1000) {
        if (ntpSynced) {
          lastDisplayedSecond = -1;
          updateClockBuffer();
          state = SHOW_CLOCK;
          stateStartTime = now;
        } else {
          initNewCycle();
        }
      }
      break;

    case SHOW_CLOCK:
      if (now - stateStartTime >= CLOCK_DURATION) {
        initNewCycle();
      }
      break;
  }
}

// ============================================================
// WiFi 调度 (每 ~16ms 检查一次, 仅比较时间戳, 极快)
// ============================================================
void checkWifiSchedule(unsigned long now) {
  if (now < nextWifiTry) return;   // 还没到时间

  Serial.println("--- WiFi 对时 ---");

  if (tryNtpSync()) {
    // 对时成功
    ntpSynced    = true;
    lastNtpSync  = millis();
    resyncTries  = 0;
    scheduleNextWifi(RESYNC_INTERVAL);
    Serial.printf("[计划] 下次对时: %luh后\n", RESYNC_INTERVAL / 3600000);

  } else {
    // 对时失败
    if (ntpSynced) {
      // 之前成功过 → 24h周期内重试
      resyncTries++;
      if (resyncTries < RESYNC_MAX_TRIES) {
        scheduleNextWifi(RESYNC_RETRY_GAP);
        Serial.printf("[重试] 第%d次失败, %dmin后再试\n",
                      resyncTries, (int)(RESYNC_RETRY_GAP / 60000));
      } else {
        // 达到最大尝试次数, 放弃本轮, 等下一个24h
        resyncTries = 0;
        lastNtpSync = millis();   // 重置计时起点
        scheduleNextWifi(RESYNC_INTERVAL);
        Serial.printf("[放弃] 本轮2次均失败, %luh后再试\n", RESYNC_INTERVAL / 3600000);
      }
    } else {
      // 从未成功过 → 持续2分钟重试
      scheduleNextWifi(RETRY_INTERVAL);
      Serial.printf("[重试] 首次对时仍未成功, %dmin后再试\n",
                    (int)(RETRY_INTERVAL / 60000));
    }
  }
}

// ============================================================
// 主循环
// ============================================================
void loop() {
  static int currentDig = 0;
  unsigned long now = millis();

  // ---- 判断该位状态 ----
  bool isScrolling = false;
  bool isShowingClock = (state == SHOW_CLOCK);

  if (state == SCROLLING && !settleDone[currentDig])
    isScrolling = true;
  else if (state == SETTLING && !settleDone[currentDig] && currentDig != settleIndex)
    isScrolling = true;

  bool nixieBlank = isScrolling && (random(100) < 8);

  if (isScrolling && now - digitLastChange[currentDig] >= (unsigned long)digitInterval[currentDig]) {
    displayBuffer[currentDig] = random(10);
    digitLastChange[currentDig] = now;
    digitInterval[currentDig] = random(30, 80);
  }

  // ---- 显示 ----
  if (nixieBlank) {
    allDigitsOff(); allSegmentsOff();
    digitalWrite(DIG_PINS[currentDig], LOW);
    delayMicroseconds(1800);
    digitalWrite(DIG_PINS[currentDig], HIGH);
  } else if (isScrolling) {
    showDigit(currentDig, displayBuffer[currentDig], dotFlags[currentDig], 1800);
  } else if (state == SETTLING && currentDig == settleIndex) {
    showDigit(currentDig, displayBuffer[currentDig], dotFlags[currentDig], 1800);
  } else if (isShowingClock) {
    showDigit(currentDig, targetDigits[currentDig], dotFlags[currentDig], 1800);
  } else {
    showDigit(currentDig, targetDigits[currentDig], dotFlags[currentDig], 1800);
  }

  currentDig = (currentDig + 1) % NUM_DIGITS;

  // ---- 每扫描一圈 ----
  if (currentDig == 0) {
    updateAnimation(now);
    if (state == SHOW_CLOCK) updateClockBuffer();

    // WiFi 对时调度检查 (非阻塞: 只检查时间到了没)
    checkWifiSchedule(now);
  }
}
