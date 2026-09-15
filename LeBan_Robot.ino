/* ================================================
 * 乐伴 LeBan  v3.3-noconfig 
 * ESP32-C3 + SSD1306 OLED + 双N20电机 + WiFi调试面板
 * ------------------------------------------------
 *   - NTP网络校时(ntp.ntsc.ac.cn): AP+STA双模式, 联网自动校准
 *   - 完整时间表: 9:00/13:00/20:00吃药  15:00活动  21:00晚安
 *   - 待机时钟轮换: 笑脸10秒 -> 整屏数码管大时钟40分钟 -> 循环往复
 *   - 角标时间贯穿: 除笑脸外, 提醒/回应表情左上角都叠加当前HH:MM
 *   - 串口命令: T吃药 K拍击 N校时 C大时钟 F/B/L/R/S行驶 1-6逐个看表情
 *   - 网页全中文按钮 + 行驶转速滑块可调(80~240, 存NVS断电不丢)
 *   - 开机LeBan字标 + 眨眼开机动画
 *   - 生动表情(v3.0风格): 待机大眼睛自动眨眼/四下张望,
 *     网页行驶时瞳孔看方向, 摇摆时眯眼笑;
 *     跑步小人双帧摆腿, 晚安=睡觉脸+Z字慢漂(v3.0风格)
 *   6种语义表情 / 双通道提醒(仅吃药: 重试反色闪烁+原地晃动;
 *   活动/晚安静态不闪) / 遗忘保护1/3/5分钟 /
 *   拍击确认(吃药->点赞, 活动/晚安->爱心) /
 *   答辩自动演示 / 网页调试 / 电机软启动 / 掉电防护
 * ------------------------------------------------
 * 接线:
 *   左电机 INT1=GPIO3 INT2=GPIO2
 *   右电机 INT3=GPIO1 INT4=GPIO21 (原GPIO0为strapping脚, 已弃用)
 *   OLED   SDA=GPIO8 SCL=GPIO9 (I2C 0x3C)
 *   拍击按键 GPIO4 <-> GND  (无按键时内部上拉, 不影响运行)
 * 热点: DeskRobot  密码 12345678  页面: http://192.168.4.1
 * ================================================ */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>   // 纠偏值/转速存NVS闪存, 掉电不丢

// ---------------- 引脚 ----------------
#define IN1 3
#define IN2 2
#define IN3 1
#define IN4 21
#define TAP_PIN 4         // 拍击按键
#define OLED_SDA 8
#define OLED_SCL 9
byte oledAddr = 0x3C;
bool screenOK = false;

// ---------------- PWM / 速度 ----------------
#define PWM_FREQ 1000
#define PWM_RES 8
#define SPEED_MIN 80      // 转速滑块下限(必须大于START_PWM)
#define SPEED_MAX 240     // 转速滑块上限
int maxSpeed = 180;       // 网页调试行驶力度(网页滑块可调, 存NVS)
#define START_PWM 70      // 越过蜗轮蜗杆自锁区
#define SOFT_STEP 8
#define SOFT_DELAY 12
#define DOWN_STEP 12

// ---------------- 演示开关 ----------------
// demoEnabled=true 上电自动演示(答辩用, 时间压缩); 网页可随时暂停/开启
// false=按真实时钟9/15/21点触发
bool demoEnabled = true;
#define DEMO_BOOT_MS  3000   // 开机后多久开始演示
#define DEMO_GAP_MS   25000  // 每轮提醒间隔: 笑脸10秒+大时钟15秒(40分钟的演示压缩)
// 遗忘重试: 真实1/3/5分钟; 演示6/10/15秒
const uint32_t retryReal[3] = { 60000UL, 180000UL, 300000UL };
const uint32_t retryDemo[3] = { 6000UL,  10000UL, 15000UL };
#define REMIND_MS  5000     // 每次提醒持续5秒(与商家版一致)

// ---------------- 方向 ----------------
enum { DIR_NONE, DIR_FWD, DIR_BACK, DIR_LEFT, DIR_RIGHT };

// ---------------- OLED 显存 ----------------
uint8_t buf[1024];
void clearBuf() { memset(buf, 0, 1024); }
void px(int x, int y) {
  if (x < 0 || x > 127 || y < 0 || y > 63) return;
  buf[(y / 8) * 128 + x] |= (1 << (y & 7));
}
void fillRect(int x, int y, int w, int h, int c) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      if (c) px(x + i, y + j);
      else {
        int xx = x + i, yy = y + j;
        if (xx < 0 || xx > 127 || yy < 0 || yy > 63) continue;
        buf[(yy / 8) * 128 + xx] &= ~(1 << (yy & 7));
      }
    }
}
void drawLine(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (1) {
    px(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
void fillCircle(int cx, int cy, int r, int c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrt((float)(r * r - dy * dy));
    fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
  }
}

void oledCmd(uint8_t c) {
  Wire.beginTransmission(oledAddr);
  Wire.write(0x00); Wire.write(c);
  Wire.endTransmission();
}
void initOLED() {
  uint8_t cmds[] = {0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,0x8D,0x14,
    0x20,0x00,0xA1,0xC8,0xDA,0x12,0x81,0x9F,0xD9,0xF1,0xDB,0x40,0xA4,0xA6,0xAF};
  // 0x81 0x9F: 对比度从默认0xCF降到0x9F, 减少OLED刷新电流尖峰, 防单电池brownout
  for (int i = 0; i < 25; i++) oledCmd(cmds[i]);
}
void showOLED() {
  for (int p = 0; p < 8; p++) {
    oledCmd(0xB0 + p); oledCmd(0x00); oledCmd(0x10);
    Wire.beginTransmission(oledAddr);
    Wire.write(0x40);
    for (int c = 0; c < 64; c++) Wire.write(buf[p * 128 + c]);
    if (Wire.endTransmission() != 0) return;
    Wire.beginTransmission(oledAddr);
    Wire.write(0x40);
    for (int c = 64; c < 128; c++) Wire.write(buf[p * 128 + c]);
    Wire.endTransmission();
  }
}
void oledNormal()  { oledCmd(0xA6); }   // 正常显示
void oledInvert()  { oledCmd(0xA7); }   // 反色(10Hz闪烁用)

// ==================== 软时钟 (millis兜底, NTP后续接入) ====================
int sysHour = 8, sysMinute = 0;
unsigned long lastMinuteTick = 0;
void tickClock(unsigned long now) {
  if (now - lastMinuteTick >= 60000) {
    lastMinuteTick = now;
    sysMinute++;
    if (sysMinute >= 60) { sysMinute = 0; sysHour = (sysHour + 1) % 24; }
  }
}

// ==================== 时间显示 (5x7点阵数字) ====================
// 整屏大时钟(待机轮换用) + 定时提醒时表情左上角的角标时间
bool gCorner = false;    // true=在提醒表情左上角叠加当前HH:MM
const uint8_t DIG7[11][7] = {          // 0-9 和冒号
  {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},  // 0
  {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},  // 1
  {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},  // 2
  {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},  // 3
  {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},  // 4
  {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},  // 5
  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},  // 6
  {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},  // 7
  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},  // 8
  {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},  // 9
  {0x00,0x04,0x00,0x00,0x04,0x00,0x00},  // :
};
const uint8_t* glyphOf(char c) {
  if (c >= '0' && c <= '9') return DIG7[c - '0'];
  if (c == ':') return DIG7[10];
  return nullptr;
}
void drawGlyph(int x0, int y0, const uint8_t* g, int s) {   // s=放大倍数
  for (int r = 0; r < 7; r++)
    for (int b = 0; b < 5; b++)
      if (g[r] & (0x10 >> b)) fillRect(x0 + b * s, y0 + r * s, s, s, 1);
}
void stampCornerTime() {               // 左上角小时间(1倍, 提醒表情用)
  char t[6];
  snprintf(t, sizeof(t), "%02d:%02d", sysHour, sysMinute);
  for (int i = 0; t[i]; i++) {
    const uint8_t* g = glyphOf(t[i]);
    if (g) drawGlyph(i * 6, 0, g, 1);
  }
}
// 7段数码管字形 (a b c d e f g -> 0x40..0x01), 大时钟整屏用
const uint8_t SEG7DIG[11] = {        // 0-9 和冒号
  0x7E, 0x30, 0x6D, 0x79, 0x33, 0x5B, 0x5F, 0x70, 0x7F, 0x6F, 0x00
};
void drawSeg7(int x0, int yo, char c) {   // 单个数码管字符 24x48, 笔画粗8
  const int DW = 24, DH = 48, DT = 8;
  if (c == ':') {
    fillRect(x0 + 2, yo + 10, 8, 8, 1);
    fillRect(x0 + 2, yo + 30, 8, 8, 1);
    return;
  }
  if (c < '0' || c > '9') return;
  uint8_t m = SEG7DIG[c - '0'];
  if (m & 0x40) fillRect(x0 + 2, yo, DW - 4, DT, 1);                        // a 上
  if (m & 0x20) fillRect(x0 + DW - DT, yo + 2, DT, DH / 2 - 4, 1);          // b 右上
  if (m & 0x10) fillRect(x0 + DW - DT, yo + DH / 2 + 2, DT, DH / 2 - 4, 1); // c 右下
  if (m & 0x08) fillRect(x0 + 2, yo + DH - DT, DW - 4, DT, 1);              // d 下
  if (m & 0x04) fillRect(x0, yo + DH / 2 + 2, DT, DH / 2 - 4, 1);           // e 左下
  if (m & 0x02) fillRect(x0, yo + 2, DT, DH / 2 - 4, 1);                    // f 左上
  if (m & 0x01) fillRect(x0 + 2, yo + DH / 2 - DT / 2, DW - 4, DT, 1);      // g 中
}
void drawClockBig() {                  // 整屏大时钟(数码管风格, 占满整屏)
  clearBuf();
  char t[6];
  snprintf(t, sizeof(t), "%02d:%02d", sysHour, sysMinute);
  int x = (128 - (24 * 4 + 12 + 3 * 4)) / 2;   // 4数字x24 + 冒号12 + 4间隔x3 = 120宽
  for (int i = 0; t[i]; i++) {
    drawSeg7(x, 8, t[i]);
    x += (t[i] == ':') ? 12 : 24;
    x += 3;
  }
  showOLED();
}

// ==================== 生动大眼睛 (v3.0风格: 待机/开心表情) ====================
// mood 0=普通(大眼+瞳孔) 1=开心(^ ^);  dx/dy=瞳孔偏移;  blink=闭眼条
void drawEyes(int mood, int dx, int dy, bool blink) {
  clearBuf();
  const int EW = 30, EH = 36, EY = 14;
  int c0 = 31, c1 = 97, cy = EY + EH / 2;
  if (mood == 1) {                          // 开心眯眼 ^ ^
    for (int e = 0; e < 2; e++) {
      int ex = (e == 0) ? 16 : 82;
      drawLine(ex, EY + 22, ex + 15, EY + 6);
      drawLine(ex + 15, EY + 6, ex + EW - 1, EY + 22);
      drawLine(ex, EY + 23, ex + 15, EY + 7);
      drawLine(ex + 15, EY + 7, ex + EW - 1, EY + 23);
    }
    drawLine(48, 48, 64, 58); drawLine(64, 58, 80, 48);
  } else {
    for (int e = 0; e < 2; e++) {
      int ex = (e == 0) ? 16 : 82;
      if (blink) fillRect(ex, cy - 3, EW, 6, 1);              // 闭眼
      else {
        fillRect(ex, EY, EW, EH, 1);                          // 大眼白
        fillRect(ex + EW / 2 - 5 + dx, cy - 5 + dy, 10, 10, 0); // 瞳孔
      }
    }
    drawLine(54, 52, 74, 52);                                 // 平嘴
  }
  showOLED();
}

// 开机字标 LeBan (3x5点阵, 3倍放大)
void drawLogo() {
  clearBuf();
  static const uint8_t FONT[5][5] = {   // L e B a n
    {0b100,0b100,0b100,0b100,0b111},
    {0b111,0b100,0b111,0b001,0b111},
    {0b110,0b101,0b110,0b101,0b110},
    {0b111,0b001,0b111,0b101,0b111},
    {0b110,0b101,0b101,0b101,0b101},
  };
  const int s = 3, adv = 12, x0 = 35, y0 = 24;
  for (int c = 0; c < 5; c++)
    for (int r = 0; r < 5; r++)
      for (int b = 0; b < 3; b++)
        if (FONT[c][r] & (0b100 >> b))
          fillRect(x0 + c * adv + b * s, y0 + r * s, s, s, 1);
  showOLED();
}

// ==================== 语义表情图标 ====================
void drawPill() {       // 💊 用药提醒
  clearBuf();
  fillRect(44, 24, 40, 16, 1);
  fillCircle(44, 32, 8, 1);
  fillCircle(84, 32, 8, 1);
  fillRect(63, 24, 2, 16, 0);   // 胶囊接缝(细线)
  if (gCorner) stampCornerTime();
  showOLED();
}
void drawHeart() {      // ❤️ 活动/晚安的回应
  clearBuf();
  fillCircle(55, 28, 9, 1);
  fillCircle(73, 28, 9, 1);
  for (int y = 28; y <= 46; y++) {
    int hw = 46 - y;
    fillRect(64 - hw, y, 2 * hw, 1, 1);
  }
  if (gCorner) stampCornerTime();   // 角标时间贯穿: 回应表情也带
  showOLED();
}
void drawRun(int frame) {  // 🏃 活动提醒 (跑步姿态, 双帧摆腿)
  clearBuf();
  fillCircle(66, 14, 6, 1);                 // 头(微前倾)
  drawLine(64, 20, 58, 40);                 // 上身前倾
  drawLine(62, 26, 48, 22);                 // 前臂上摆
  drawLine(61, 29, 76, 36);                 // 后臂下摆
  if (frame == 0) {
    drawLine(58, 40, 48, 54);               // 后腿蹬地
    drawLine(58, 40, 74, 48);               // 前腿抬起
  } else {
    drawLine(58, 40, 54, 56);               // 换步态
    drawLine(58, 40, 78, 44);
  }
  if (gCorner) stampCornerTime();
  showOLED();
}
void drawThumb() {      // 👍 服药后的鼓励
  clearBuf();
  fillRect(46, 32, 32, 18, 1);              // 手掌
  fillCircle(59, 12, 5, 1);                 // 拇指指尖(圆头)
  fillRect(54, 12, 10, 22, 1);              // 竖起的拇指
  fillRect(78, 34, 4, 5, 1);                // 右侧食指关节凸起
  fillRect(78, 41, 4, 5, 1);                // 中指关节凸起
  if (gCorner) stampCornerTime();   // 角标时间贯穿: 鼓励表情也带
  showOLED();
}
void drawMoon() {       // 😴 月亮图标 (串口'4'测试用, 静态)
  clearBuf();
  fillCircle(64, 32, 20, 1);
  fillCircle(73, 25, 16, 0);                // 咬出月牙
  px(40, 16); px(39, 17); px(41, 17);       // 小星星(固定)
  px(92, 42); px(91, 43); px(93, 43);
  showOLED();
}

// 晚安表情(v3.0睡觉脸): 半闭眼 + Z字 (z=0/1双帧慢漂, 晚安提醒用)
void drawSleep(int z) {
  clearBuf();
  const int EW = 30, EY = 14;
  for (int e = 0; e < 2; e++) {
    int ex = (e == 0) ? 16 : 82;
    drawLine(ex, EY + 16, ex + EW - 1, EY + 16);   // 上眼睑
    fillRect(ex, EY + 18, EW, 14, 1);              // 闭眼下半
  }
  int zx = (z == 0) ? 96 : 100, zy = (z == 0) ? 6 : 2;   // Z字漂浮
  drawLine(zx, zy, zx + 12, zy);
  drawLine(zx + 12, zy, zx, zy + 10);
  drawLine(zx, zy + 10, zx + 12, zy + 10);
  if (gCorner) stampCornerTime();
  showOLED();
}
void showRemindFace(uint8_t type) {
  if (type == 1) drawRun(0);
  else if (type == 2) drawPill();
  else drawSleep(0);          // 晚安: 睡觉脸(v3.0风格)
}

// ==================== 电机 ====================
// 右轮镜像安装: INT4=右轮前进, INT3=右轮后退
// ---- 直行纠偏 ----
// 双电机出厂转速有差异, 直行会往"弱轮"一侧跑偏.
// fwdTrim>0 = 左轮加强(纠右偏), fwdTrim<0 = 右轮加强(纠左偏)
// 串口 ] 加5 / [ 减5, 自动存NVS断电不丢
int fwdTrim = 0;
void trimSave() {
  Preferences prefs;
  prefs.begin("leban", false);
  prefs.putInt("trim", fwdTrim);
  prefs.end();
}
void applyMotor(int dir, int pwm) {
  int a = 0, b = 0, c = 0, d = 0;
  int tL = constrain(pwm + fwdTrim, 0, 255);   // 左轮
  int tR = constrain(pwm - fwdTrim, 0, 255);   // 右轮
  switch (dir) {
    case DIR_FWD:   a = tL; d = tR; break;
    case DIR_BACK:  b = tL; c = tR; break;
    case DIR_LEFT:  b = pwm; d = pwm; break;
    case DIR_RIGHT: a = pwm; c = pwm; break;
  }
  ledcWrite(IN1, a); ledcWrite(IN2, b);
  ledcWrite(IN3, c); ledcWrite(IN4, d);
}

// ---- 网页调试行驶状态机(非阻塞软启动; 仅待机态使用) ----
int dbgDir = DIR_NONE;
int activeDir = DIR_NONE;
int currentPwm = 0;
int mState = 0;
unsigned long stateTimer = 0;
bool wiggleMode = false;
unsigned long wiggleTimer = 0;

void motorTick(unsigned long now) {
  if (wiggleMode && mState == 2 && now - wiggleTimer >= 450) {
    wiggleTimer = now;
    dbgDir = (activeDir == DIR_LEFT) ? DIR_RIGHT : DIR_LEFT;
  }
  switch (mState) {
    case 0:
      if (dbgDir != DIR_NONE) {
        activeDir = dbgDir;
        currentPwm = START_PWM;
        applyMotor(activeDir, currentPwm);
        mState = 1; stateTimer = now;
      }
      break;
    case 1:
    case 2:
      if (dbgDir != activeDir) { mState = 3; break; }
      if (mState == 1 && now - stateTimer >= SOFT_DELAY) {
        stateTimer = now;
        currentPwm += SOFT_STEP;
        if (currentPwm >= maxSpeed) { currentPwm = maxSpeed; mState = 2; }
        applyMotor(activeDir, currentPwm);
      }
      break;
    case 3:
      if (now - stateTimer >= SOFT_DELAY) {
        stateTimer = now;
        currentPwm -= DOWN_STEP;
        if (currentPwm <= START_PWM) {
          currentPwm = 0;
          applyMotor(DIR_NONE, 0);
          activeDir = dbgDir;
          if (activeDir == DIR_NONE) mState = 0;
          else { currentPwm = START_PWM; applyMotor(activeDir, currentPwm); mState = 1; }
          stateTimer = now;
        } else {
          applyMotor(activeDir, currentPwm);
        }
      }
      break;
  }
}

// ==================== 原地晃动(提醒动作, 防跌落) ====================
// 前进0.5s-停0.2s-后退0.5s-停0.2s 循环; 力度随提醒级别增强
// 关键: PWM软启动斜坡, 消除单节18650下的起步浪涌(防欠压重启)
enum { ROCK_FWD, ROCK_STOP1, ROCK_BACK, ROCK_STOP2 };
int rockPhase = ROCK_FWD;
unsigned long rockStart = 0;

// 斜坡驱动: 实际出力向目标逼近, 换向前先回零
int rockCurDir = DIR_NONE, rockCurPwm = 0;
int rockWantDir = DIR_NONE, rockWantPwm = 0;
unsigned long rockRampLast = 0;
bool rockActive = false;     // 晃动子系统是否占用电机(停稳后释放给调试状态机)
void rockRamp(unsigned long now) {
  if (!rockActive) {
    if (rockWantDir == DIR_NONE && rockWantPwm == 0) return;  // 无任务: 不碰电机
    rockActive = true;                                         // 有新目标: 接管
  }
  if (now - rockRampLast < 8) return;          // 约125Hz刷新
  rockRampLast = now;
  if (rockCurDir != rockWantDir) {             // 需要换向/启动/停止: 先降到0
    if (rockCurPwm > 0) {
      rockCurPwm -= 40;
      if (rockCurPwm < 0) rockCurPwm = 0;
      applyMotor(rockCurDir, rockCurPwm);
      return;
    }
    rockCurDir = rockWantDir;
  }
  if (rockCurPwm < rockWantPwm) {              // 软加速: 每步+10
    rockCurPwm += 10;
    if (rockCurPwm > rockWantPwm) rockCurPwm = rockWantPwm;
  } else if (rockCurPwm > rockWantPwm) {
    rockCurPwm -= 40;
    if (rockCurPwm < rockWantPwm) rockCurPwm = rockWantPwm;
  }
  applyMotor(rockCurDir, rockCurPwm);
  if (rockWantDir == DIR_NONE && rockCurDir == DIR_NONE && rockCurPwm == 0)
    rockActive = false;                        // 已停稳, 释放电机控制权
}

void rockStop() {
  rockPhase = ROCK_FWD;
  rockWantDir = DIR_NONE; rockWantPwm = 0;      // 斜坡软停, 不突然断电
  rockCurDir = DIR_NONE;  rockCurPwm = 0;       // 重置斜坡当前状态, 防止rockRamp重新出力
  rockActive = false;
}
void rockTick(unsigned long now, int level) {
  // 三档力度: 首次轻 / 重试中 / 最强 (单节电池下降低峰值防掉压)
  int spdF = (level >= 3) ? 160 : (level == 2 ? 140 : 100);
  int spdB = (level >= 3) ? 120 : (level == 2 ? 105 : 80);
  unsigned long durs[4] = { 500, 200, 500, 200 };
  if (now - rockStart >= durs[rockPhase]) {
    rockStart = now;
    rockPhase = (rockPhase + 1) & 3;
    if      (rockPhase == ROCK_FWD)  { rockWantDir = DIR_FWD;  rockWantPwm = spdF; }
    else if (rockPhase == ROCK_BACK) { rockWantDir = DIR_BACK; rockWantPwm = spdB; }
    else                             { rockWantDir = DIR_NONE; rockWantPwm = 0;    }
  }
  rockRamp(now);
}

// ==================== 拍击按键 (GPIO4) ====================
// 防抖升级: 需低电平持续40ms才算真拍 (电机EMI毛刺撑不过40ms, 手拍可以)
// 开机2.5秒静默期: WiFi射频启动的RF脉冲会被铜丝天线接收产生假拍
bool tapped() {
  if (millis() < 2500) return false;
  static bool lastLow = false;
  static unsigned long lastTap = 0;
  static unsigned long lowSince = 0;
  bool nowLow = (digitalRead(TAP_PIN) == LOW);
  unsigned long now = millis();
  if (nowLow) {
    if (lowSince == 0) {
      lowSince = now;
      Serial.printf("[TapRaw] LOW起 pwm=%d want=%d @%lu\n", rockCurPwm, rockWantPwm, now);
    }
    if (now - lowSince >= 40 && !lastLow && (now - lastTap >= 50)) {
      lastLow = true; lastTap = now; lowSince = 0;
      Serial.printf("[TapOK] @%lu\n", now);
      return true;
    }
  } else {
    if (lowSince != 0) Serial.printf("[TapRaw] LOW止 dur=%lu @%lu\n", now - lowSince, now);
    lowSince = 0;
    lastLow = false;
  }
  return false;
}

// ==================== 提醒时间表 (商家完整版5项) ====================
struct ScheduleItem { int h, m; uint8_t type; };
const ScheduleItem schedule[] = {
  { 9, 0, 2 },   // 9:00  吃药
  {13, 0, 2 },   // 13:00 吃药
  {15, 0, 1 },   // 15:00 活动
  {20, 0, 2 },   // 20:00 吃药
  {21, 0, 3 },   // 21:00 晚安
};
#define SCHED_N 5
int lastTrigMin[SCHED_N] = {-1,-1,-1,-1,-1};

// ==================== NTP 网络校时 (可选, 断网照常跑) ====================
// AP(DeskRobot)一直保持调试面板; STA顺带连家里WiFi拿网络时间校准软时钟.
// staNetworks里改成你家WiFi; 都连不上则自动回纯AP, 用软时钟照常提醒.
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.ntsc.ac.cn", 8 * 3600, 600000); // UTC+8, 10分钟更新

// 备用硬编码WiFi(留空=跳过, 纯离线跑; 需联网校时才在这里填)
const char* staNetworks[][2] = {
  {"", ""},
};
#define STA_COUNT (sizeof(staNetworks) / sizeof(staNetworks[0]))
bool staActive = false;          // 是否已STA联网
unsigned long lastNtpSync = 0;
char curSsid[33] = "";           // 当前STA凭证(WiFi自愈重连用)
char curPwd[65] = "";

void syncClockFromNTP() {
  if ((WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    sysHour   = timeClient.getHours();
    sysMinute = timeClient.getMinutes();
    lastMinuteTick = millis();
    Serial.printf("[NTP] 校时 %02d:%02d\n", sysHour, sysMinute);
  } else {
    Serial.println("[NTP] 无网络, 沿用软时钟");
  }
}

void tryStaConnect() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.setAutoReconnect(true);
  // 备用硬编码列表(默认留空全部跳过)
  for (int i = 0; i < (int)STA_COUNT; i++) {
    if (!staNetworks[i][0][0]) continue;
    Serial.printf("[STA] 尝试: %s\n", staNetworks[i][0]);
    WiFi.begin(staNetworks[i][0], staNetworks[i][1]);
    int w = 0;
    while (WiFi.status() != WL_CONNECTED && w < 10) { delay(500); w++; }
    if (WiFi.status() == WL_CONNECTED) {
      strncpy(curSsid, staNetworks[i][0], 32);
      strncpy(curPwd,  staNetworks[i][1], 64);
      staActive = true;
      Serial.printf("[STA] 已连接 IP=%s\n", WiFi.localIP().toString().c_str());
      return;
    }
    WiFi.disconnect(true, false);
  }
  WiFi.setAutoReconnect(false);          // 关自动重连, 避免后台持续拉电流
  WiFi.mode(WIFI_AP);
  Serial.println("[STA] 未联通, 纯AP离线模式(软时钟兜底)");
}

void ntpTick(unsigned long now) {
  if (!staActive) return;
  if (now - lastNtpSync < 600000UL) return;
  lastNtpSync = now;
  syncClockFromNTP();
}

// ==================== 应用状态机 ====================
enum { ST_IDLE, ST_REMIND, ST_FORGOT, ST_RESPOND, ST_PET };
int appState = ST_IDLE;
uint8_t remindType = 0;      // 1活动 2吃药 3晚安
int remindLevel = 0;         // 0首次 1/2/3重试
bool encourage = false;      // true=点赞(吃药) false=爱心
unsigned long stateStart = 0;
unsigned long nextRetryAt = 0;
unsigned long bootStart = 0, nextDemoAt = 0;
uint8_t demoSeq = 0;
unsigned long manualLockUntil = 0;   // 网页手动操作后暂停自动演示20秒

// ==================== 待机时钟轮换 (笑脸10秒 <-> 大时钟40分钟) ====================
// 待机先亮10秒笑脸, 然后整屏大时钟40分钟, 再回笑脸10秒, 循环往复.
// 演示模式压缩为15秒时钟便于答辩展示; 提醒/拍击/网页行驶随时打断.
#define IDLE_FACE_MS 10000UL          // 笑脸阶段时长
bool idleClockMode = false;           // true=当前整屏显示大时钟
unsigned long idlePhaseAt = 0;        // 当前阶段开始时刻
int lastClockMin = -1;                // 大时钟已显示的分钟(每分钟刷新一次)
int eyeMood = -1, eyeDx = -99, eyeDy = -99;   // 眼睛当前帧(-1=强制重画)
bool eyeBlink = false;

unsigned long idleClockDur() { return demoEnabled ? 15000UL : 2400000UL; }

void idleEnterFace() {                // 切回笑脸阶段
  idleClockMode = false;
  eyeMood = -1;                       // 强制idleFaceTick重画
}
void idleEnterClock(unsigned long now) {   // 切到整屏大时钟
  idleClockMode = true;
  idlePhaseAt = now;
  lastClockMin = -1;
  Serial.println("[Idle] 整屏大时钟");
}
void idleCycleTick(unsigned long now) {
  if (appState != ST_IDLE) return;
  if (!idleClockMode) {
    if (now - idlePhaseAt >= IDLE_FACE_MS) idleEnterClock(now);
    return;
  }
  if (dbgDir != DIR_NONE || wiggleMode) {    // 行驶调试中: 切回笑脸看眼睛
    idlePhaseAt = now; idleEnterFace(); return;
  }
  int curMin = sysHour * 60 + sysMinute;
  if (curMin != lastClockMin) {              // 每分钟才刷新一次, 省电
    lastClockMin = curMin;
    if (screenOK) drawClockBig();
  }
  if (now - idlePhaseAt >= idleClockDur()) {
    idlePhaseAt = now;
    idleEnterFace();
  }
}

void triggerRemind(uint8_t type, unsigned long now) {
  rockStop();                                   // 统一先停掉上一次晃动, 电机不残留
  remindType = type;
  remindLevel = 0;
  Serial.printf("[Trig] type=%u\n", type);
  encourage = (type == 2);
  appState = ST_REMIND;
  stateStart = now;
  rockPhase = ROCK_FWD; rockStart = now;
  oledNormal();
  gCorner = true;                               // 定时时段: 表情左上角显示当前时间
  showRemindFace(type);
  if (type == 2) { rockWantDir = DIR_FWD; rockWantPwm = 100; }  // 吃药: 斜坡软启动晃动
}
void backToIdle(unsigned long now) {
  appState = ST_IDLE;
  rockStop(); oledNormal(); drawEyes(0, 0, 0, false);   // 睁眼回待机
  eyeMood = 0; eyeDx = 0; eyeDy = 0; eyeBlink = false;  // 同步眼睛帧防重画
  gCorner = false;                                      // 离开提醒时段, 关角标
  idleClockMode = false; idlePhaseAt = now;             // 先看10秒笑脸再进时钟
  stateStart = now; nextDemoAt = now + DEMO_GAP_MS;   // 两轮演示间留白
}
void enterRespond(unsigned long now) {
  appState = ST_RESPOND;
  stateStart = now;
  rockStop();
  // 让 rockRamp 在等待期间持续跑斜坡, 直到电机完全停稳再显示回应
  unsigned long t0 = millis();
  while (rockActive && millis() - t0 < 500) rockRamp(millis());
  delay(2);
  oledNormal();
  if (encourage) drawThumb(); else drawHeart();
  oledNormal();
  Serial.println("[Respond] show face for 4s");
}
void enterPet(unsigned long now) {        // 空闲时被拍: 眯眼笑+轻蠕动一下(撒娇)
  appState = ST_PET;
  stateStart = now;
  oledNormal(); drawEyes(1, 0, 0, false);     // ^ ^ 开心眼
  rockPhase = ROCK_FWD; rockStart = now;
  rockWantDir = DIR_FWD; rockWantPwm = 100;   // 斜坡轻蠕动(撒娇)
}

void appTick(unsigned long now) {
  // 拍击: 提醒/遗忘阶段=确认; 空闲时=撒娇回应
  if (tapped()) {
    if (appState == ST_REMIND || appState == ST_FORGOT) {
      enterRespond(now);
      return;
    }
    if (appState == ST_IDLE) {
      enterPet(now);
      return;
    }
  }

  switch (appState) {
    case ST_IDLE: {
      idleCycleTick(now);                // 笑脸10秒/大时钟40分钟轮换
      if (demoEnabled) {
        if (now < manualLockUntil) break;
        if (now - bootStart < DEMO_BOOT_MS) break;
        if (now >= nextDemoAt) {
          static const uint8_t seq[3] = {2, 1, 3};   // 吃药->活动->晚安
          triggerRemind(seq[demoSeq % 3], now);
          demoSeq++;
        }
      } else {
        // 真实模式: 按软时钟检查时间表
        for (int i = 0; i < SCHED_N; i++) {
          if (sysHour == schedule[i].h && sysMinute == schedule[i].m
              && lastTrigMin[i] != sysMinute) {
            lastTrigMin[i] = sysMinute;
            triggerRemind(schedule[i].type, now);
            break;
          }
        }
      }
      break;
    }

    case ST_REMIND: {
      bool isMed = (remindType == 2);
      // 只有吃药提醒做原地晃动; 活动/晚安为静态表情
      if (isMed) rockTick(now, remindLevel);
      // 只有吃药才闪: 首次只动不闪, 遗忘重试阶段才10Hz反色闪烁
      // (运动/晚安: 不动不闪, 静态表情, 与商家版规则一致)
      if (isMed && remindLevel >= 1) {
        if ((now / 50) % 2 == 0) oledInvert(); else oledNormal();
      }
      if (now - stateStart >= REMIND_MS) {
        Serial.printf("[RemindEnd] lv=%d\n", remindLevel);
        rockStop(); oledNormal();
        if (demoEnabled && remindLevel >= 1) {
          // 演示模式: 首次+一轮重试演完后, 自动模拟"老人已回应" -> 点赞/爱心
          // 这样6个表情无需拍击也能在答辩时全部展示
          enterRespond(now);
        } else if (remindLevel < 3) {
          remindLevel++;
          appState = ST_FORGOT;
          const uint32_t* iv = demoEnabled ? retryDemo : retryReal;
          nextRetryAt = now + iv[remindLevel - 1];
        } else {
          // 3次重试(1/3/5分钟)都未确认 -> 放弃本轮提醒, 回待机
          // (按需求只提醒三轮, 不做无限闹钟; 下个时间点再触发)
          Serial.println("[GiveUp] 3次重试未确认, 回待机");
          backToIdle(now);
        }
      }
      break;
    }

    case ST_FORGOT: {
      rockStop();
      if (now >= nextRetryAt) {                 // 重试时刻到, 再次提醒
        Serial.printf("[Retry] lv=%d\n", remindLevel);
        appState = ST_REMIND;
        stateStart = now;
        rockPhase = ROCK_FWD; rockStart = now;
        oledNormal();
        showRemindFace(remindType);
        if (remindType == 2) {
          rockWantDir = DIR_FWD;
          rockWantPwm = remindLevel >= 3 ? 160 : (remindLevel == 2 ? 140 : 100);
        }
      }
      break;
    }

    case ST_RESPOND: {
      oledNormal();   // 每轮都确认正常显示, 杜绝任何反色残留
      if (now - stateStart >= 4000) {
        Serial.println("[Respond] -> idle");
        backToIdle(now);   // 回应显示4秒回笑脸(让表情看清)
      }
      break;
    }

    case ST_PET: {                                       // 撒娇: 爱心+轻蠕动2秒
      rockTick(now, 0);
      if (now - stateStart >= 2000) backToIdle(now);
      break;
    }
  }
}

// 网页手动触发提醒/取消自动流程
void webTrigger(uint8_t type) {
  unsigned long now = millis();
  manualLockUntil = now + 20000;
  dbgDir = DIR_NONE; wiggleMode = false; activeDir = DIR_NONE; mState = 0;
  triggerRemind(type, now);
}
void webCancel() {
  unsigned long now = millis();
  manualLockUntil = now + 20000;   // 手动接管后20秒内不自动演示
  dbgDir = DIR_NONE; wiggleMode = false; activeDir = DIR_NONE; mState = 0;
  rockStop();                      // 晃动软停
  applyMotor(DIR_NONE, 0);         // 立即切断电机PWM, 防止调试行驶后保持出力
  backToIdle(now);
}
// 网页: 暂停/开启自动演示
void webSetAuto(bool on) {
  unsigned long now = millis();
  demoEnabled = on;
  if (!on) {                // 暂停: 立即中止正在演的提醒, 停车回笑脸
    manualLockUntil = now + 600000UL;
    dbgDir = DIR_NONE; wiggleMode = false; activeDir = DIR_NONE; mState = 0;
    rockStop();
    applyMotor(DIR_NONE, 0);    // 立即切断电机PWM
    if (appState != ST_IDLE) backToIdle(now);
  } else {                  // 开启: 1.5秒后开始下一轮
    manualLockUntil = now;
    nextDemoAt = now + 1500;
    bootStart = 0;
  }
}
// 网页: 直接显示回应表情2秒 (thumb=true点赞 false爱心)
void webFace(bool thumb) {
  unsigned long now = millis();
  manualLockUntil = now + 20000;
  dbgDir = DIR_NONE; wiggleMode = false; activeDir = DIR_NONE; mState = 0;
  encourage = thumb;
  gCorner = true;                        // 回应表情也带左上角时间(贯穿除笑脸外)
  enterRespond(now);
}

// ==================== 表情动画调度 (生动版核心) ====================
// 待机: 大眼睛眨眼/张望(笑脸10秒) -> 整屏大时钟(40分钟)轮换; 行驶时瞳孔看方向; 摇摆时眯眼笑
// 提醒中: 活动小人双帧摆腿, 晚安月亮星星闪烁
// (反色闪烁由提醒状态机独立控制, 与帧刷新互不影响)
void idleFaceTick(unsigned long now) {
  static unsigned long nextGlance = 0;
  static int glance = 0;
  int mood = wiggleMode ? 1 : 0;
  int dx = 0, dy = 0;
  if (mood == 0) {
    if (activeDir == DIR_LEFT)       dx = -9;
    else if (activeDir == DIR_RIGHT) dx = 9;
    else if (activeDir == DIR_FWD)   dy = -9;
    else if (activeDir == DIR_BACK)  dy = 8;
    else {                                     // 静止: 自主张望
      if (now >= nextGlance) {
        nextGlance = now + 1500 + random(4) * 400;
        glance = random(5);
      }
      static const int8_t GX[5] = {0, -9, 9, 0, -7};
      static const int8_t GY[5] = {0, 0, 0, -8, 6};
      dx = GX[glance]; dy = GY[glance];
    }
  }
  bool blink = (mood == 0) && ((now % 3200) < 130);   // 每3.2秒眨一次
  if (mood != eyeMood || dx != eyeDx || dy != eyeDy || blink != eyeBlink) {
    eyeMood = mood; eyeDx = dx; eyeDy = dy; eyeBlink = blink;
    drawEyes(mood, dx, dy, blink);
  }
}

void faceAnimTick(unsigned long now) {
  if (appState == ST_IDLE) {
    if (!idleClockMode) idleFaceTick(now);   // 大时钟阶段: 整屏时间不画表情
    return;
  }
  if (appState == ST_REMIND) {
    static int runFrame = 0, zFrame = 0;
    if (remindType == 1) {                     // 跑步摆腿 350ms/帧
      int f = (now / 350) & 1;
      if (f != runFrame) { runFrame = f; drawRun(f); }
    } else if (remindType == 3) {              // Z字慢漂 900ms/帧 (睡觉脸)
      int f = (now / 900) & 1;
      if (f != zFrame) { zFrame = f; drawSleep(f); }
    }
  }
}

// ==================== 串口调试命令 (与商家版一致) ====================
// T=触发吃药 K=模拟拍击 N=立即校时 C=看大时钟 F/B/L/R/S=行驶 1-6=逐个看表情
void serialTick() {
  while (Serial.available()) {
    char c = Serial.read();
    unsigned long now = millis();
    switch (c) {
      case 'T': Serial.println("[串口] 触发吃药提醒"); webTrigger(2); break;
      case 'K': if (appState == ST_REMIND || appState == ST_FORGOT) enterRespond(now); break;
      case 'N': syncClockFromNTP(); break;
      case '1': drawEyes(0, 0, 0, false); break;   // 笑脸(大眼)
      case '2': drawPill();  break;
      case '3': drawRun(0);  break;
      case '4': drawMoon();  break;
      case '5': drawThumb(); break;
      case '6': drawHeart(); break;
      case 'C': webCancel(); idleEnterClock(now); break;   // 强制整屏大时钟
      case '[': fwdTrim -= 5; trimSave(); Serial.printf("[纠偏] fwdTrim=%d\n", fwdTrim); break;
      case ']': fwdTrim += 5; trimSave(); Serial.printf("[纠偏] fwdTrim=%d\n", fwdTrim); break;
      case 'F': case 'f': webCancel(); dbgDir = DIR_FWD;   break;
      case 'B': case 'b': webCancel(); dbgDir = DIR_BACK;  break;
      case 'L': case 'l': webCancel(); dbgDir = DIR_LEFT;  break;
      case 'R': case 'r': webCancel(); dbgDir = DIR_RIGHT; break;
      case 'S': case 's': webCancel(); break;
    }
  }
}

// ==================== WiFi 调试面板 ====================
const char HTML[] PROGMEM = R"===(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" href="data:,">
<title>LeBan 乐伴</title>
<style>
body{font-family:Arial;background:#1a1a2e;color:#fff;margin:0;padding:15px;text-align:center}
h1{color:#00ffd1;margin:6px 0 2px}
h2{font-size:14px;color:#8a8aa3;font-weight:normal;margin:16px 0 4px}
button{background:rgba(0,255,225,.1);border:1px solid rgba(0,255,225,.3);color:#fff;padding:16px 0;font-size:16px;border-radius:10px;margin:4px;cursor:pointer;width:100%;user-select:none;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
button:active{background:rgba(0,255,225,.35)}
.btns{display:grid;grid-template-columns:1fr 1fr 1fr;max-width:400px;margin:6px auto}
.stop{background:rgba(255,68,68,.2);border-color:rgba(255,68,68,.3)}
.auto{background:rgba(255,190,60,.15);border-color:rgba(255,190,60,.35)}
.wide{grid-column:1/4}
#status{max-width:400px;margin:8px auto;padding:10px;border-radius:8px;font-size:14px;min-height:18px;background:rgba(255,255,255,.07)}
.ok{color:#00ff9d}.err{color:#ff6b6b}
</style></head><body>
<h1>乐伴 LeBan</h1>
<div class="btns">
<button class="wide auto" onclick="go('/auto0','暂停自动演示')">⏸ 暂停自动演示(手动调试用)</button>
<button class="wide auto" onclick="go('/auto1','开启自动演示')">▶ 开启自动演示(答辩用)</button>
</div>
<h2>提醒演示</h2>
<div class="btns">
<button onclick="go('/pill','吃药提醒')">💊 吃药</button>
<button onclick="go('/run','活动提醒')">🏃 活动</button>
<button onclick="go('/moon','晚安提醒')">😴 晚安</button>
<button onclick="go('/thumb','点赞')">👍 点赞</button>
<button onclick="go('/heart','爱心')">❤️ 爱心</button>
<button onclick="go('/home','返回笑脸')">😊 待机</button>
</div>
<h2>运动调试</h2>
<div class="btns">
<button onclick="go('/f','前进')">⬆ 前进</button>
<button class="stop" onclick="go('/s','停止')">⏹ 停止</button>
<button onclick="go('/b','后退')">⬇ 后退</button>
<button onclick="go('/l','左转')">⬅ 左转</button>
<button onclick="go('/r','右转')">➡ 右转</button>
<button class="wide" onclick="go('/wiggle','摇摆')">🔁 原地摇摆</button>
</div>
<h2>行驶转速</h2>
<div style="max-width:400px;margin:6px auto">
<input id="spd" type="range" min="80" max="240" step="10" value="180" style="width:100%;accent-color:#00ffd1" oninput="spdShow()" onchange="saveSpd()">
<div id="spdval" style="font-size:14px;color:#8a8aa3;margin:2px 0">当前: 180</div>
</div>
<h2>时间同步</h2>
<div class="btns">
<button class="wide" onclick="syncTime()" style="grid-column:1/4">🕐 同步手机时间到机器人</button>
</div>
<div id="robottime" style="font-size:14px;color:#8a8aa3;margin:4px 0">机器人时间: --:--</div>
<div id="status">就绪 · 连本热点时请关闭手机移动数据</div>
<script>
function go(c,name){
  var st=document.getElementById('status');
  st.className=''; st.textContent='发送中: '+name+'...';
  fetch(c,{cache:'no-store'}).then(function(r){
    if(!r.ok) throw new Error('bad'); return r.text();
  }).then(function(){
    st.className='ok'; st.textContent='✓ 已收到: '+name;
  }).catch(function(){
    st.className='err'; st.textContent='✗ 没送到, 关掉手机移动数据再试';
  });
}
function spdShow(){
  document.getElementById('spdval').textContent='当前: '+document.getElementById('spd').value;
}
function saveSpd(){
  var v=document.getElementById('spd').value;
  fetch('/spd?val='+v,{cache:'no-store'}).then(function(r){return r.text();})
  .then(function(t){document.getElementById('spdval').textContent='✓ 已保存 转速: '+t;});
}
function syncTime(){
  var d=new Date();
  var h=d.getHours();
  var m=d.getMinutes();
  var st=document.getElementById('status');
  st.className=''; st.textContent='同步时间中...';
  fetch('/settime?h='+h+'&m='+m,{cache:'no-store'}).then(function(r){return r.text();})
  .then(function(t){
    st.className='ok'; st.textContent='✓ 已同步: '+t;
    document.getElementById('robottime').textContent='机器人时间: '+t;
  }).catch(function(){
    st.className='err'; st.textContent='✗ 同步失败, 关掉手机移动数据再试';
  });
}
var spdInit=false;
function poll(){
  fetch('/wifistat',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){
    if(!spdInit && j.spd){spdInit=true;document.getElementById('spd').value=j.spd;spdShow();}
    if(j.time){document.getElementById('robottime').textContent='机器人时间: '+j.time;}
  }).catch(function(){});
}
setInterval(poll,2500); poll();
</script>
</body></html>
)===";

WebServer server(80);

void onWifiStat() {                  // 网页滑块同步当前转速/时间用
  char t[6];
  snprintf(t, sizeof(t), "%02d:%02d", sysHour, sysMinute);
  String j = "{\"spd\":" + String(maxSpeed) + ",\"time\":\"" + String(t) + "\"}";
  server.send(200, "application/json", j);
}

void setupServer() {
  server.on("/", []() { server.send_P(200, "text/html", HTML); });
  server.on("/wifistat", onWifiStat);
  server.on("/pill", []() { webTrigger(2); server.send(200, "text/plain", "PILL"); });
  server.on("/run",  []() { webTrigger(1); server.send(200, "text/plain", "RUN"); });
  server.on("/moon", []() { webTrigger(3); server.send(200, "text/plain", "MOON"); });
  server.on("/thumb",[]() { webFace(true);  server.send(200, "text/plain", "THUMB"); });
  server.on("/heart",[]() { webFace(false); server.send(200, "text/plain", "HEART"); });
  server.on("/home", []() { webCancel();  server.send(200, "text/plain", "HOME"); });
  server.on("/auto0",[]() { webSetAuto(false); server.send(200, "text/plain", "AUTO0"); });
  server.on("/auto1",[]() { webSetAuto(true);  server.send(200, "text/plain", "AUTO1"); });
  server.on("/f", []() { webCancel(); dbgDir = DIR_FWD;   server.send(200, "text/plain", "F"); });
  server.on("/b", []() { webCancel(); dbgDir = DIR_BACK;  server.send(200, "text/plain", "B"); });
  server.on("/l", []() { webCancel(); dbgDir = DIR_LEFT;  server.send(200, "text/plain", "L"); });
  server.on("/r", []() { webCancel(); dbgDir = DIR_RIGHT; server.send(200, "text/plain", "R"); });
  server.on("/s", []() { webCancel(); server.send(200, "text/plain", "S"); });
  server.on("/wiggle", []() { webCancel(); wiggleMode = true; dbgDir = DIR_LEFT; server.send(200, "text/plain", "W"); });
  server.on("/spd", []() {                       // 行驶转速滑块(存NVS断电不丢)
    int v = constrain(server.arg("val").toInt(), SPEED_MIN, SPEED_MAX);
    maxSpeed = v;
    Preferences prefs;
    prefs.begin("leban", false);
    prefs.putInt("spd", v);
    prefs.end();
    Serial.printf("[转速] maxSpeed=%d\n", maxSpeed);
    server.send(200, "text/plain", String(v));
  });
  server.on("/settime", []() {            // 网页同步手机时间到机器人软时钟
    int h = constrain(server.arg("h").toInt(), 0, 23);
    int m = constrain(server.arg("m").toInt(), 0, 59);
    sysHour = h; sysMinute = m;
    lastMinuteTick = millis();            // 重置软时钟计时基准
    char t[6];
    snprintf(t, sizeof(t), "%02d:%02d", h, m);
    Serial.printf("[网页校时] %s\n", t);
    server.send(200, "text/plain", String(t));
  });
}

// ==================== 开机 ====================
void setup() {
  Serial.begin(115200);
  {   // 掉电诊断: 打印上次复位原因 (9=brownout, 电池电压瞬跌重启的证据)
    int rr = esp_reset_reason();
    Serial.printf("\n[复位原因] %d %s\n", rr,
      rr == 9 ? "<- BROWNOUT: 电池电压瞬跌导致重启!" : "");
  }
  {   // 读回直行纠偏值和转速
    Preferences prefs;
    prefs.begin("leban", false);
    fwdTrim = prefs.getInt("trim", 0);
    maxSpeed = prefs.getInt("spd", 180);
    prefs.end();
    Serial.printf("[纠偏] fwdTrim=%d  [转速] maxSpeed=%d\n", fwdTrim, maxSpeed);
  }
  // Strapping保护: 电机脚先全部拉低, 跨过启动采样窗口
  pinMode(IN1, OUTPUT); digitalWrite(IN1, LOW);
  pinMode(IN2, OUTPUT); digitalWrite(IN2, LOW);
  pinMode(IN3, OUTPUT); digitalWrite(IN3, LOW);
  pinMode(IN4, OUTPUT); digitalWrite(IN4, LOW);
  delay(5);

  ledcAttach(IN1, PWM_FREQ, PWM_RES);
  ledcAttach(IN2, PWM_FREQ, PWM_RES);
  ledcAttach(IN3, PWM_FREQ, PWM_RES);
  ledcAttach(IN4, PWM_FREQ, PWM_RES);
  applyMotor(DIR_NONE, 0);

  pinMode(TAP_PIN, INPUT_PULLUP);   // 拍击按键

  // Strapping 保护: GPIO9/10 是 C3 启动模式采样脚, 设为 PULLUP 防误进下载模式
  pinMode(GPIO_NUM_9,  INPUT_PULLUP);
  pinMode(GPIO_NUM_10, INPUT_PULLUP);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() == 0) { oledAddr = 0x3C; screenOK = true; }
  else {
    Wire.beginTransmission(0x3D);
    if (Wire.endTransmission() == 0) { oledAddr = 0x3D; screenOK = true; }
  }
  if (screenOK) { initOLED(); drawLogo(); }   // 开机字标(联网期间一直显示)

  // WiFi 热点
  Serial.println("WiFi AP starting...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_11dBm);   // 降低射频峰值电流, 给电机留余量
  bool apOK = false;
  for (int t = 0; t < 3 && !apOK; t++) {
    apOK = WiFi.softAP("DeskRobot", "12345678", 1, 0, 4);
    Serial.printf("softAP attempt %d -> %s\n", t + 1, apOK ? "OK" : "FAIL");
    if (!apOK) { delay(500); WiFi.mode(WIFI_AP); }
  }
  setupServer();
  server.begin();
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  // STA联网校时(可选): 连不上自动回纯AP, 不影响任何功能
  tryStaConnect();
  if ((WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED) {
    staActive = true;
    timeClient.begin();
    syncClockFromNTP();
    lastNtpSync = millis();
  }

  // 睁眼开机动画: 字标期间闭着眼, 联网结束后睁开
  if (screenOK) { drawEyes(0, 0, 0, true); delay(350); drawEyes(0, 0, 0, false); }

  unsigned long now = millis();
  bootStart = now; nextDemoAt = now + DEMO_BOOT_MS;
  lastMinuteTick = now;
  idlePhaseAt = now;                 // 开机: 先亮10秒笑脸再进大时钟
}

// ==================== WiFi 自愈 ====================
// 单节电池下电机浪涌可能拽低电压导致WiFi掉线, 检测到AP丢失则自动重启
// (STA联网时用AP_STA模式恢复, 不影响NTP校时)
unsigned long lastWifiCheck = 0;
void wifiWatchdog(unsigned long now) {
  if (now - lastWifiCheck < 5000) return;
  lastWifiCheck = now;
  if (!(WiFi.getMode() & WIFI_AP) || !WiFi.softAPIP()) {
    Serial.println("[WiFi] AP lost, restarting...");
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(staActive ? WIFI_AP_STA : WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    if (staActive && curSsid[0]) WiFi.begin(curSsid, curPwd);
    if (WiFi.softAP("DeskRobot", "12345678", 1, 0, 4))
      Serial.println("[WiFi] AP restored");
    else
      Serial.println("[WiFi] AP restore FAILED");
  }
}

void loop() {
  unsigned long now = millis();
  server.handleClient();
  serialTick();                                // 串口调试命令
  tickClock(now);
  appTick(now);
  faceAnimTick(now);                           // 待机眨眼/张望 + 提醒动画
  rockRamp(now);                               // 晃动斜坡在所有状态下都可软停
  wifiWatchdog(now);                           // WiFi 自愈检测
  ntpTick(now);                                // NTP 周期校时
  if (appState == ST_IDLE && !rockActive) motorTick(now);   // 调试行驶(待机且晃动已停稳)
}
