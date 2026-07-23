#include <Arduino.h>

/**
 * Steins;Gate — 世界线变动探测仪 + AurolystantConsole
 * ESP32 + 8位数码管 (共阴极)
 *
 * 模式 A: 世界线变动探测仪 + NTP 时钟 (节能版)
 *   周期: SCROLLING → SETTLING → STABLE(1s) → SHOW_CLOCK(5s) → 循环
 *
 * 模式 B: AurolystantConsole (Web 控制面板)
 *   - 世界线自定义 + 原作预设
 *   - 倒计时 / 番茄钟
 *   - 倒计时归零 → 数码管持续随机滚动
 *   - 触摸 GPIO34 停止警报 / 非警报时切回模式A
 *
 * 段选: 13,12,14,27,26,25,33,32
 * 位选: 15,2,4,16,17,21,22,23
 */

#include <WiFi.h>
#include <time.h>
#include <WebServer.h>

// ============================================================
// WiFi / NTP 配置
// ============================================================
const char* WIFI_SSID  = "test";
const char* WIFI_PASS  = "27182818";
const char* NTP_SVR1   = "ntp.aliyun.com";
const char* NTP_SVR2   = "pool.ntp.org";
const long  GMT_OFFSET = 8 * 3600;
const int   DST_OFFSET = 0;

const unsigned long RETRY_INTERVAL   = 2 * 60000;
const unsigned long RESYNC_INTERVAL  = 24UL * 3600000;
const unsigned long RESYNC_RETRY_GAP = 2 * 60000;
const int           RESYNC_MAX_TRIES = 2;

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
const int TOUCH_PIN = 34;

// ============================================================
// 模式
// ============================================================
enum DeviceMode { MODE_DETECTOR, MODE_CONSOLE };
DeviceMode currentMode = MODE_DETECTOR;
volatile bool modeSwitchRequested = false;

// ============================================================
// 7段字形
// ============================================================
const int DIGIT_SEGMENTS[][8] = {
  {segA, segB, segC, segD, segE, segF, -1},
  {segB, segC, -1},
  {segA, segB, segD, segE, segG, -1},
  {segA, segB, segC, segD, segG, -1},
  {segB, segC, segF, segG, -1},
  {segA, segC, segD, segF, segG, -1},
  {segA, segC, segD, segE, segF, segG, -1},
  {segA, segB, segC, -1},
  {segA, segB, segC, segD, segE, segF, segG, -1},
  {segA, segB, segC, segD, segF, segG, -1},
  {segA, segB, segC, segE, segF, segG, -1},                    // A
  {segC, segD, segE, segF, segG, -1},                          // b
  {segA, segD, segE, segF, -1},                                // C
  {segB, segC, segD, segE, segG, -1},                          // d
  {segA, segD, segE, segF, segG, -1},                          // E
  {segA, segE, segF, segG, -1},                                // F
  {segA, segB, segC, segD, segE, segF, -1},                    // O (0x10)
  {segF, segE, segB, segC, segD, -1},                          // V (0x11)
  {segA, segF, segE, -1},                                      // R (0x12)
};
#define BLANK_DIGIT 0xFF
#define DASH_DIGIT  0xFE
#define LETTER_O    0x10
#define LETTER_V    0x11
#define LETTER_R    0x12
// LETTER_E = 14 (index 14 in table)

// ============================================================
// 模式 A: 世界线探测仪
// ============================================================
const float SPECIAL_LINES[] = { 1.048596, 0.571024, 0.000000 };
const int NUM_SPECIAL = sizeof(SPECIAL_LINES) / sizeof(SPECIAL_LINES[0]);

uint8_t targetDigits[NUM_DIGITS];
uint8_t displayBuffer[NUM_DIGITS];
bool    dotFlags[NUM_DIGITS];
float   targetDivergence;

bool         ntpSynced     = false;
unsigned long lastNtpSync  = 0;
unsigned long nextWifiTry  = 0;
int          resyncTries   = 0;
struct tm timeinfo;
int     lastDisplayedSecond = -1;

enum AnimState { SCROLLING, SETTLING, STABLE, SHOW_CLOCK };
AnimState state = SCROLLING;
unsigned long stateStartTime = 0, scrollDuration = 0;
const unsigned long CLOCK_DURATION = 5000;
unsigned long digitLastChange[NUM_DIGITS];
int  digitInterval[NUM_DIGITS];
int  settleIndex = 0;
enum SettleStep { STEP_FLASH1, STEP_FLASH2, STEP_HOLD };
SettleStep settleStep = STEP_FLASH1;
unsigned long settleStepStart = 0, lastFlashChange = 0;
bool settleDone[NUM_DIGITS];

// ============================================================
// 模式 B: Console 状态机
// ============================================================
enum ConsoleState { CS_WORLDLINE, CS_COUNTDOWN, CS_ALARM };
ConsoleState consoleState = CS_WORLDLINE;
// 警报子状态
enum AlarmSubState { ALARM_OVER, ALARM_SCROLL };
AlarmSubState alarmSub = ALARM_OVER;
unsigned long alarmSubStart = 0;
const unsigned long ALARM_OVER_DUR   = 5000;
const unsigned long ALARM_SCROLL_DUR = 5000;

// 世界线: 值 (int64 ×10⁷) 和动画 (复刻模式A的 SCROLLING→SETTLING→STABLE)
int64_t  worldLineValue = 10485960;
enum WLAnimState { WL_SCROLLING, WL_SETTLING, WL_STABLE };
WLAnimState wlState = WL_STABLE;
unsigned long wlStateStart = 0, wlScrollDuration = 0;
unsigned long wlDigitLastChange[8];
int  wlDigitInterval[8];
int  wlSettleIdx = 0;
enum SettleStepWL { WL_FLASH1, WL_FLASH2, WL_HOLD };
SettleStepWL wlSettleStep = WL_FLASH1;
unsigned long wlSettleStepStart = 0, wlLastFlashChange = 0;
bool wlSettleDone[8];
uint8_t wlTargetDigits[8];   // 目标每一位的值

// 倒计时
unsigned long countdownTotalSec  = 0;       // 总秒数
unsigned long countdownRemaining = 0;       // 剩余秒数
unsigned long lastCountdownTick  = 0;
bool     countdownRunning = false;

// 警报: 随机滚动
unsigned long alarmDigitLastChange[8];
int  alarmDigitInterval[8];

// Web 服务器
WebServer server(80);
const char* CONSOLE_AP_SSID = "AurolystantConsole";
const char* CONSOLE_AP_PASS = "27182818";

// ============================================================
// 前置声明
// ============================================================
// 数码管
void allSegmentsOff();
void allDigitsOff();
void showDigit(int pos, uint8_t number, bool dot, unsigned long holdUs);

// 触摸
void updateTouch(unsigned long now);
void switchMode();

// 模式 A
bool tryNtpSync();
void scheduleNextWifi(unsigned long delayMs);
void updateClockBuffer();
void initNewCycle();
void updateAnimation(unsigned long now);
void checkWifiSchedule(unsigned long now);

// 模式 B
void consoleInit();
void consoleExit();
void consoleUpdateDisplay(unsigned long now);
void consoleSetWorldLine(int64_t value);
void consoleStartCountdown(unsigned long seconds);
void consoleStopAlarm();
void handleRoot();
void handleSet();
void handleCountdownStart();
void handleCountdownStop();

// ============================================================
// 触摸检测
// ============================================================
int  touchBaseline  = 0, touchRaw = 0;
bool touchCalibrated = false;
unsigned long lastTouchSwitch = 0;
const unsigned long TOUCH_COOLDOWN = 1200;
const int TOUCH_THRESHOLD = 500;

void calibrateTouch() {
  long sum = 0;
  for (int i = 0; i < 100; i++) { sum += analogRead(TOUCH_PIN); delay(5); }
  touchBaseline = sum / 100;
  touchCalibrated = true;
  Serial.printf("[触摸] 校准完成, 基准值=%d\n", touchBaseline);
}

void updateTouch(unsigned long now) {
  if (!touchCalibrated || now - lastTouchSwitch < TOUCH_COOLDOWN) return;
  touchRaw = analogRead(TOUCH_PIN);
  if (abs(touchRaw - touchBaseline) > TOUCH_THRESHOLD) {
    delay(50);
    if (abs(analogRead(TOUCH_PIN) - touchBaseline) > TOUCH_THRESHOLD) {
      lastTouchSwitch = now;
      modeSwitchRequested = true;
    }
  }
  touchBaseline = (touchBaseline * 49 + touchRaw) / 50;
}

void switchMode() {
  if (currentMode == MODE_DETECTOR) {
    Serial.println("=== 切换到 Console 模式 ===");
    currentMode = MODE_CONSOLE;
    consoleInit();
  } else {
    // Console → Detector
    if (consoleState == CS_ALARM) {
      // 警报态触摸 = 仅停止警报
      consoleStopAlarm();
      return;
    }
    Serial.println("=== 切换到探测仪模式 ===");
    consoleExit();
    currentMode = MODE_DETECTOR;
    state = SCROLLING;
    initNewCycle();
    if (ntpSynced) scheduleNextWifi(RESYNC_INTERVAL - (millis() - lastNtpSync));
  }
  modeSwitchRequested = false;
}

// ============================================================
// 数码管驱动
// ============================================================
void allSegmentsOff() { for (int i = 0; i < 8; i++) digitalWrite(SEG_PINS[i], LOW); }
void allDigitsOff()  { for (int i = 0; i < NUM_DIGITS; i++) digitalWrite(DIG_PINS[i], HIGH); }

void showDigit(int pos, uint8_t number, bool dot, unsigned long holdUs) {
  allDigitsOff(); allSegmentsOff();
  if (number != BLANK_DIGIT && number != DASH_DIGIT && number <= 0x12) {
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
// 模式 A: WiFi + NTP
// ============================================================
bool tryNtpSync() {
  Serial.printf("[WiFi] 连接 %s ...", WIFI_SSID);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  int wait = 0;
  while (WiFi.status() != WL_CONNECTED && wait < 30) { delay(500); Serial.print("."); wait++; }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WiFi] 连接失败, 状态码=%d\n", WiFi.status());
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    return false;
  }
  Serial.printf("[WiFi] 已连接, IP=%s\n", WiFi.localIP().toString().c_str());
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SVR1, NTP_SVR2);
  Serial.print("[NTP] 等待同步...");
  int ntpWait = 0; struct tm dummy;
  while (!getLocalTime(&dummy) && ntpWait < 40) { delay(250); Serial.print("."); ntpWait++; }
  Serial.println();
  if (getLocalTime(&dummy))
    Serial.printf("[NTP] 对时成功: %04d-%02d-%02d %02d:%02d:%02d\n",
                  dummy.tm_year+1900, dummy.tm_mon+1, dummy.tm_mday,
                  dummy.tm_hour, dummy.tm_min, dummy.tm_sec);
  else Serial.println("[NTP] 对时超时");
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  Serial.println("[WiFi] 已关闭");
  return getLocalTime(&dummy);
}
void scheduleNextWifi(unsigned long delayMs) { nextWifiTry = millis() + delayMs; }

void updateClockBuffer() {
  if (!ntpSynced || !getLocalTime(&timeinfo)) return;
  if (timeinfo.tm_sec == lastDisplayedSecond) return;
  lastDisplayedSecond = timeinfo.tm_sec;
  int h = timeinfo.tm_hour, m = timeinfo.tm_min, s = timeinfo.tm_sec;
  targetDigits[0] = h/10; targetDigits[1] = h%10;
  targetDigits[2] = DASH_DIGIT;
  targetDigits[3] = m/10; targetDigits[4] = m%10;
  targetDigits[5] = DASH_DIGIT;
  targetDigits[6] = s/10; targetDigits[7] = s%10;
  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
}

void initNewCycle() {
  if (random(100) < 5) {
    targetDivergence = SPECIAL_LINES[random(NUM_SPECIAL)];
    int64_t raw = (int64_t)(targetDivergence * 10000000.0 + 0.5);
    for (int i = 7; i >= 0; i--) { targetDigits[i] = raw % 10; raw /= 10; }
  } else {
    targetDigits[0] = random(4);
    for (int i = 1; i < NUM_DIGITS; i++) targetDigits[i] = random(10);
  }
  for (int i = 0; i < NUM_DIGITS; i++) displayBuffer[i] = random(10);
  for (int i = 0; i < NUM_DIGITS; i++) { dotFlags[i] = false; settleDone[i] = false; }
  dotFlags[0] = true;
  settleIndex = 0; settleStep = STEP_FLASH1;
  for (int i = 0; i < NUM_DIGITS; i++) {
    digitInterval[i] = random(30, 80); digitLastChange[i] = 0;
  }
  scrollDuration = random(1500, 2000); state = SCROLLING; stateStartTime = millis();
  Serial.print("目标世界线: ");
  for (int i = 0; i < NUM_DIGITS; i++) { Serial.print(targetDigits[i]); if (i==0) Serial.print("."); }
  Serial.println();
}

void updateAnimation(unsigned long now) {
  switch (state) {
    case SCROLLING:
      if (now - stateStartTime >= scrollDuration) {
        state = SETTLING; stateStartTime = now;
        settleIndex = 0; settleStep = STEP_FLASH1;
        settleStepStart = now; lastFlashChange = now;
      } break;
    case SETTLING: {
      if (settleIndex >= NUM_DIGITS) {
        for (int i = 0; i < NUM_DIGITS; i++) displayBuffer[i] = targetDigits[i];
        state = STABLE; stateStartTime = now; break;
      }
      unsigned long el = now - settleStepStart;
      switch (settleStep) {
        case STEP_FLASH1:
          if (el < 50) { if (now - lastFlashChange >= (el<20?8UL:25UL)) { displayBuffer[settleIndex]=random(10); lastFlashChange=now; } }
          else { displayBuffer[settleIndex]=random(10); settleStep=STEP_FLASH2; settleStepStart=now; lastFlashChange=now; } break;
        case STEP_FLASH2:
          if (el < 100) { if (now - lastFlashChange >= (el<40?20UL:60UL)) { displayBuffer[settleIndex]=random(10); lastFlashChange=now; } }
          else { displayBuffer[settleIndex]=targetDigits[settleIndex]; settleStep=STEP_HOLD; settleStepStart=now; } break;
        case STEP_HOLD:
          if (el >= 150) { settleDone[settleIndex]=true; displayBuffer[settleIndex]=targetDigits[settleIndex]; settleIndex++; settleStep=STEP_FLASH1; settleStepStart=now; lastFlashChange=now; } break;
      } break;
    }
    case STABLE:
      if (now - stateStartTime >= 1000) {
        if (ntpSynced) { lastDisplayedSecond=-1; updateClockBuffer(); state=SHOW_CLOCK; stateStartTime=now; }
        else initNewCycle();
      } break;
    case SHOW_CLOCK:
      if (now - stateStartTime >= CLOCK_DURATION) initNewCycle(); break;
  }
}

void checkWifiSchedule(unsigned long now) {
  if (now < nextWifiTry) return;
  Serial.println("--- WiFi 对时 ---");
  if (tryNtpSync()) {
    ntpSynced=true; lastNtpSync=millis(); resyncTries=0;
    scheduleNextWifi(RESYNC_INTERVAL);
    Serial.printf("[计划] %luh后再次对时\n", RESYNC_INTERVAL/3600000);
  } else {
    if (ntpSynced) {
      resyncTries++;
      if (resyncTries < RESYNC_MAX_TRIES) { scheduleNextWifi(RESYNC_RETRY_GAP); Serial.printf("[重试] 第%d次失败\n", resyncTries); }
      else { resyncTries=0; lastNtpSync=millis(); scheduleNextWifi(RESYNC_INTERVAL); Serial.println("[放弃] 本轮失败"); }
    } else { scheduleNextWifi(RETRY_INTERVAL); Serial.println("[重试] 首次对时失败"); }
  }
}

// ============================================================
// 模式 B: Console — 世界线/倒计时/警报
// ============================================================
void consoleSetWorldLine(int64_t value) {
  worldLineValue = value;
  // 提取每一位目标值
  int64_t raw = value;
  for (int i = 7; i >= 0; i--) { wlTargetDigits[i] = raw % 10; raw /= 10; }
  // 初始化: 所有位随机
  for (int i = 0; i < NUM_DIGITS; i++) displayBuffer[i] = random(10);
  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
  dotFlags[0] = true;
  // 开始 SCROLLING 阶段
  for (int i = 0; i < 8; i++) {
    wlDigitInterval[i] = random(30, 80);
    wlDigitLastChange[i] = 0;
    wlSettleDone[i] = false;
  }
  wlScrollDuration = random(1500, 2000);
  wlState = WL_SCROLLING;
  wlStateStart = millis();
  consoleState = CS_WORLDLINE;
  Serial.printf("[Console] 世界线: %.6f\n", value / 10000000.0);
}

void consoleStartCountdown(unsigned long seconds) {
  // 续传: 如果之前被中止且剩余时间>0, 继续; 否则重新开始
  if (countdownRemaining == 0 || consoleState == CS_ALARM) {
    countdownTotalSec = seconds;
    countdownRemaining = seconds;
  }
  // else: 保持 countdownRemaining (从中止位置继续)
  countdownRunning = true;
  lastCountdownTick = millis();
  consoleState = CS_COUNTDOWN;
  // 显示当前时间
  unsigned long h = countdownRemaining / 3600,
               m = (countdownRemaining % 3600) / 60,
               s = countdownRemaining % 60;
  displayBuffer[0] = h/10; displayBuffer[1] = h%10;
  displayBuffer[2] = DASH_DIGIT;
  displayBuffer[3] = m/10; displayBuffer[4] = m%10;
  displayBuffer[5] = DASH_DIGIT;
  displayBuffer[6] = s/10; displayBuffer[7] = s%10;
  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
  Serial.printf("[Console] 倒计时 %lu 秒 (%s)\n", countdownRemaining,
                countdownRemaining == seconds ? "新计时" : "续传");
}

void consoleStopAlarm() {
  consoleState = CS_WORLDLINE;
  countdownRunning = false;
  // 恢复显示当前世界线 (直接定格)
  int64_t raw = worldLineValue;
  for (int i = 7; i >= 0; i--) { displayBuffer[i] = raw % 10; raw /= 10; }
  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
  dotFlags[0] = true;
  wlState = WL_STABLE;
  Serial.println("[Console] 警报已停止");
}

void consoleUpdateDisplay(unsigned long now) {
  switch (consoleState) {
    case CS_WORLDLINE: {
      // 复刻模式A的 SCROLLING → SETTLING → STABLE 动画
      // SCROLLING: 所有位随机滚动
      if (wlState == WL_SCROLLING) {
        for (int i = 0; i < NUM_DIGITS; i++) {
          if (now - wlDigitLastChange[i] >= (unsigned long)wlDigitInterval[i]) {
            displayBuffer[i] = random(10);
            wlDigitLastChange[i] = now;
            wlDigitInterval[i] = random(30, 80);
          }
        }
        if (now - wlStateStart >= wlScrollDuration) {
          wlState = WL_SETTLING; wlStateStart = now;
          wlSettleIdx = 0; wlSettleStep = WL_FLASH1;
          wlSettleStepStart = now; wlLastFlashChange = now;
        }
      }

      // SETTLING: 逐位锁定
      if (wlState == WL_SETTLING) {
        // 已锁定的位
        for (int i = 0; i < wlSettleIdx; i++) displayBuffer[i] = wlTargetDigits[i];
        // 未锁定的位继续滚动
        for (int i = wlSettleIdx + 1; i < NUM_DIGITS; i++) {
          if (now - wlDigitLastChange[i] >= (unsigned long)wlDigitInterval[i]) {
            displayBuffer[i] = random(10); wlDigitLastChange[i] = now;
            wlDigitInterval[i] = random(30, 80);
          }
        }
        // 当前锁定位: 闪烁
        unsigned long el = now - wlSettleStepStart;
        switch (wlSettleStep) {
          case WL_FLASH1:
            if (el < 50) {
              if (now - wlLastFlashChange >= (el < 20 ? 8UL : 25UL))
                { displayBuffer[wlSettleIdx] = random(10); wlLastFlashChange = now; }
            } else {
              displayBuffer[wlSettleIdx] = random(10);
              wlSettleStep = WL_FLASH2; wlSettleStepStart = now; wlLastFlashChange = now;
            } break;
          case WL_FLASH2:
            if (el < 100) {
              if (now - wlLastFlashChange >= (el < 40 ? 20UL : 60UL))
                { displayBuffer[wlSettleIdx] = random(10); wlLastFlashChange = now; }
            } else {
              displayBuffer[wlSettleIdx] = wlTargetDigits[wlSettleIdx];
              wlSettleStep = WL_HOLD; wlSettleStepStart = now;
            } break;
          case WL_HOLD:
            if (el >= 150) {
              displayBuffer[wlSettleIdx] = wlTargetDigits[wlSettleIdx];
              wlSettleIdx++; wlSettleStep = WL_FLASH1;
              wlSettleStepStart = now; wlLastFlashChange = now;
              if (wlSettleIdx >= NUM_DIGITS) { wlState = WL_STABLE; }
            } break;
        }
      }
      break;
    }

    case CS_COUNTDOWN:
      if (countdownRunning && now - lastCountdownTick >= 1000) {
        lastCountdownTick += 1000;
        if (countdownRemaining > 0) {
          countdownRemaining--;
          unsigned long h = countdownRemaining / 3600,
                       m = (countdownRemaining % 3600) / 60,
                       s = countdownRemaining % 60;
          displayBuffer[0] = h/10; displayBuffer[1] = h%10;
          displayBuffer[2] = DASH_DIGIT;
          displayBuffer[3] = m/10; displayBuffer[4] = m%10;
          displayBuffer[5] = DASH_DIGIT;
          displayBuffer[6] = s/10; displayBuffer[7] = s%10;
          for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
        }
        if (countdownRemaining == 0) {
          // 进入警报态: 先显示 --OVER--
          consoleState = CS_ALARM;
          countdownRunning = false;
          alarmSub = ALARM_OVER;
          alarmSubStart = now;
          displayBuffer[0] = DASH_DIGIT;
          displayBuffer[1] = DASH_DIGIT;
          displayBuffer[2] = LETTER_O;
          displayBuffer[3] = LETTER_V;
          displayBuffer[4] = 14;          // E
          displayBuffer[5] = LETTER_R;
          displayBuffer[6] = DASH_DIGIT;
          displayBuffer[7] = DASH_DIGIT;
          for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
          Serial.println("[Console] ⏰ 倒计时归零! 警报模式");
        }
      }
      break;

    case CS_ALARM: {
      // --OVER-- (5s) ↔ 随机滚动 (5s) 循环
      unsigned long elapsed = now - alarmSubStart;
      if (alarmSub == ALARM_OVER && elapsed >= ALARM_OVER_DUR) {
        alarmSub = ALARM_SCROLL;
        alarmSubStart = now;
        for (int i = 0; i < NUM_DIGITS; i++) {
          alarmDigitInterval[i] = random(30, 80);
          alarmDigitLastChange[i] = 0;
          displayBuffer[i] = random(10);
        }
        for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
      } else if (alarmSub == ALARM_SCROLL) {
        if (elapsed >= ALARM_SCROLL_DUR) {
          alarmSub = ALARM_OVER;
          alarmSubStart = now;
          displayBuffer[0] = DASH_DIGIT;
          displayBuffer[1] = DASH_DIGIT;
          displayBuffer[2] = LETTER_O;
          displayBuffer[3] = LETTER_V;
          displayBuffer[4] = 14;          // E
          displayBuffer[5] = LETTER_R;
          displayBuffer[6] = DASH_DIGIT;
          displayBuffer[7] = DASH_DIGIT;
          for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
        } else {
          for (int i = 0; i < NUM_DIGITS; i++) {
            if (now - alarmDigitLastChange[i] >= (unsigned long)alarmDigitInterval[i]) {
              displayBuffer[i] = random(10);
              alarmDigitLastChange[i] = now;
              alarmDigitInterval[i] = random(30, 80);
            }
          }
        }
      }
      break;
    }
  }
}

// ============================================================
// 模式 B: Console — Web UI
// ============================================================
const char HTML_HEAD[] PROGMEM = R"raw(<!DOCTYPE html><html lang="zh"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AurolystantConsole</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#0a0a0a;color:#ccc;max-width:420px;margin:auto;padding:16px}
h1{text-align:center;font-size:18px;margin:8px 0 16px;color:#fff;letter-spacing:4px}
.card{border:1px solid #333;padding:14px;margin:12px 0}
.card h2{font-size:14px;color:#fff;margin-bottom:10px;border-bottom:1px solid #333;padding-bottom:6px}
input,button,select{padding:8px 12px;font-family:monospace;font-size:14px;background:#111;color:#fff;border:1px solid #444}
input:focus{outline:none;border-color:#fff}
button{cursor:pointer;min-width:60px}
button:hover{background:#222;border-color:#fff}
button.primary{background:#fff;color:#000;border-color:#fff}
button.primary:hover{background:#ccc}
button.danger{border-color:#800;color:#f44}
button.danger:hover{background:#200}
.presets{display:flex;flex-wrap:wrap;gap:6px;margin:10px 0}
.presets button{font-size:12px;padding:6px 10px;min-width:auto}
.row{display:flex;gap:8px;align-items:center;margin:8px 0}
.row input{flex:1}
.timer-display{font-size:28px;text-align:center;color:#fff;margin:12px 0;letter-spacing:2px}
.shortcuts{display:flex;gap:6px;flex-wrap:wrap}
.shortcuts button{flex:1;min-width:55px;font-size:12px}
.status{font-size:12px;color:#888;text-align:center;margin-top:8px}
</style></head><body>
<h1>AurolystantConsole</h1>
)raw";

const char HTML_FOOT[] PROGMEM = R"raw(
<div class="status" id="status">就绪</div>
<script>
function api(url,cb){fetch(url).then(r=>r.json()).then(cb).catch(e=>st('错误'))}
function st(t){document.getElementById('status').innerText=t}
function setWL(){var v=document.getElementById('wlInput').value;if(!v)return;
 api('/set?value='+v,function(d){st(d.msg)})}
function preWL(v){document.getElementById('wlInput').value=v;setWL()}
function ctStart(){var h=parseInt(document.getElementById('ctH').value)||0;
 var m=parseInt(document.getElementById('ctM').value)||0;
 var s=parseInt(document.getElementById('ctS').value)||0;
 var t=h*3600+m*60+s;if(t<=0)return;api('/countdown/start?sec='+t,function(d){st(d.msg)})}
function ctStop(){api('/countdown/stop',function(d){st(d.msg)})}
function ctReset(){api('/countdown/reset',function(d){st(d.msg)})}
function ctQuick(s){api('/countdown/start?sec='+s,function(d){st(d.msg)})}
</script></body></html>
)raw";

void handleRoot() {
  String html = FPSTR(HTML_HEAD);

  // ---- 世界线 ----
  html += "<div class='card'><h2>世界线</h2>";
  html += "<div class='row'><input id='wlInput' type='number' step='0.000001' value='1.048596' placeholder='0.000000'><button class='primary' onclick='setWL()'>发送</button></div>";
  html += "<div class='presets'>";
  const char* names[] = {"Steins Gate","Alpha","Beta","Delta","Gamma","Omega"};
  const char* values[] = {"1.048596","0.571024","1.382733","0.337187","0.456789","0.000000"};
  for (int i = 0; i < 6; i++) {
    html += "<button onclick=\"preWL('"; html += values[i]; html += "')\">"; html += names[i]; html += "<br>"; html += values[i]; html += "</button>";
  }
  html += "</div></div>";

  // ---- 倒计时 ----
  html += "<div class='card'><h2>倒计时</h2>";
  html += "<div class='timer-display' id='timerDisplay'>--:--:--</div>";
  html += "<div class='row'>";
  html += "<input id='ctH' type='number' value='0' min='0' max='99' placeholder='时' style='flex:1'>";
  html += "<input id='ctM' type='number' value='25' min='0' max='59' placeholder='分' style='flex:1'>";
  html += "<input id='ctS' type='number' value='0' min='0' max='59' placeholder='秒' style='flex:1'>";
  html += "</div>";
  html += "<div class='row'><button class='primary' onclick='ctStart()' style='flex:2'>▶ 开始</button><button class='danger' onclick='ctStop()' style='flex:1'>停止</button><button onclick='ctReset()' style='flex:1'>复位</button></div>";
  html += "<div class='shortcuts'>";
  const int shortcuts[] = {300, 900, 1500, 3600};
  const char* slabels[] = {"5m","15m","25m","60m"};
  for (int i = 0; i < 4; i++) {
    html += "<button onclick=\"ctQuick("; html += String(shortcuts[i]); html += ")\">"; html += slabels[i]; html += "</button>";
  }
  html += "</div></div>";

  // ---- 状态 ----
  html += "<div class='card'><h2>状态</h2>";
  html += "<p>数码管: "; html += (consoleState==CS_ALARM?"⚠ 警报":consoleState==CS_COUNTDOWN?"⏱ 倒计时":"世界线");
  html += "</p><p>模式: AurolystantConsole</p>";
  html += "<p>触摸: "; html += (consoleState==CS_ALARM?"停止警报":"切回探测仪"); html += "</p></div>";

  html += FPSTR(HTML_FOOT);
  server.send(200, "text/html; charset=utf-8", html);
}

void handleSet() {
  if (server.hasArg("value")) {
    float val = server.arg("value").toFloat();
    if (val >= 0 && val < 100) {
      int64_t raw = (int64_t)(val * 10000000.0 + 0.5);
      consoleSetWorldLine(raw);
      server.send(200, "application/json", "{\"msg\":\"世界线已发送\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"msg\":\"无效值\"}");
}

void handleCountdownStart() {
  if (server.hasArg("sec")) {
    unsigned long sec = server.arg("sec").toInt();
    if (sec > 0 && sec <= 359999) {  // max 99h59m59s
      consoleStartCountdown(sec);
      server.send(200, "application/json", "{\"msg\":\"倒计时已开始\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"msg\":\"无效时间\"}");
}

void handleCountdownStop() {
  if (consoleState == CS_ALARM) { consoleStopAlarm(); countdownRemaining = 0; }
  else { countdownRunning = false; consoleState = CS_WORLDLINE; }
  server.send(200, "application/json", "{\"msg\":\"已停止\"}");
}

void handleCountdownReset() {
  countdownRemaining = 0;
  countdownRunning = false;
  consoleState = CS_WORLDLINE;
  // 恢复世界线显示
  int64_t raw = worldLineValue;
  for (int i = 7; i >= 0; i--) { displayBuffer[i] = raw % 10; raw /= 10; }
  for (int i = 0; i < NUM_DIGITS; i++) dotFlags[i] = false;
  dotFlags[0] = true;
  wlState = WL_STABLE;
  server.send(200, "application/json", "{\"msg\":\"已复位\"}");
}

void consoleInit() {
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONSOLE_AP_SSID, CONSOLE_AP_PASS);
  Serial.printf("[Console] AP: %s @ 192.168.4.1\n", CONSOLE_AP_SSID);
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/countdown/start", handleCountdownStart);
  server.on("/countdown/stop", handleCountdownStop);
  server.on("/countdown/reset", handleCountdownReset);
  server.begin();
  consoleSetWorldLine(10485960);  // 默认 1.048596
  Serial.println("[Console] 初始化完成");
}

void consoleExit() {
  countdownRunning = false;
  server.stop();
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(100);
  Serial.println("[Console] 已退出");
}

// ============================================================
// setup
// ============================================================
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 8; i++) { pinMode(SEG_PINS[i], OUTPUT); digitalWrite(SEG_PINS[i], LOW); }
  for (int i = 0; i < NUM_DIGITS; i++) { pinMode(DIG_PINS[i], OUTPUT); digitalWrite(DIG_PINS[i], HIGH); }
  randomSeed(analogRead(0));
  calibrateTouch();

  Serial.println("=== 首次对时 ===");
  if (tryNtpSync()) { ntpSynced=true; lastNtpSync=millis(); scheduleNextWifi(RESYNC_INTERVAL); }
  else { ntpSynced=false; scheduleNextWifi(RETRY_INTERVAL); }

  initNewCycle();
  Serial.println("=== 世界线变动探测仪 启动 ===");
  Serial.println("触摸 GPIO34 切换到 Console");
  Serial.println("El Psy Kongroo.");
}

// ============================================================
// loop
// ============================================================
void loop() {
  static int currentDig = 0;
  unsigned long now = millis();

  if (currentDig == 0) {
    updateTouch(now);
    if (modeSwitchRequested) switchMode();
  }

  // ---- 模式 A: 探测仪 ----
  if (currentMode == MODE_DETECTOR) {
    bool isScrolling = false, isShowingClock = (state == SHOW_CLOCK);
    if (state == SCROLLING && !settleDone[currentDig]) isScrolling = true;
    else if (state == SETTLING && !settleDone[currentDig] && currentDig != settleIndex) isScrolling = true;
    bool nixieBlank = isScrolling && (random(100) < 8);
    if (isScrolling && now - digitLastChange[currentDig] >= (unsigned long)digitInterval[currentDig]) {
      displayBuffer[currentDig] = random(10); digitLastChange[currentDig] = now;
      digitInterval[currentDig] = random(30, 80);
    }
    if (nixieBlank) {
      allDigitsOff(); allSegmentsOff();
      digitalWrite(DIG_PINS[currentDig], LOW); delayMicroseconds(1800);
      digitalWrite(DIG_PINS[currentDig], HIGH);
    } else if (isScrolling) showDigit(currentDig, displayBuffer[currentDig], dotFlags[currentDig], 1800);
    else if (state == SETTLING && currentDig == settleIndex) showDigit(currentDig, displayBuffer[currentDig], dotFlags[currentDig], 1800);
    else if (isShowingClock) showDigit(currentDig, targetDigits[currentDig], dotFlags[currentDig], 1800);
    else showDigit(currentDig, targetDigits[currentDig], dotFlags[currentDig], 1800);
    currentDig = (currentDig + 1) % NUM_DIGITS;
    if (currentDig == 0) { updateAnimation(now); if (state == SHOW_CLOCK) updateClockBuffer(); checkWifiSchedule(now); }
    return;
  }

  // ---- 模式 B: Console ----
  if (currentMode == MODE_CONSOLE) {
    consoleUpdateDisplay(now);
    showDigit(currentDig, displayBuffer[currentDig], dotFlags[currentDig], 1800);
    currentDig = (currentDig + 1) % NUM_DIGITS;
    if (currentDig == 0) server.handleClient();
    return;
  }
}
