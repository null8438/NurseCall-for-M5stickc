// =====================================================================
//  ナースコール 送信機 (M5StickC / StickC Plus / StickC Plus2)
//
//  普段はディープスリープ。メインボタン(BtnA / GPIO37)で起床し、
//  長押しが確認できたら Wi-Fi(ルーター経由)で Core2 に UDP で呼び出しを送り、
//  Core2 の ACK を確認してから再びスリープ。
//  短押し(誤操作)の場合は画面も Wi-Fi も点けずに即スリープへ戻る。
//
//  ライブラリ: M5Unified (Arduino ライブラリマネージャから)
// =====================================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// ---------------- 設定 ----------------
static const char*    DEVICE_NAME = "寝室";            // Core2・スマホに表示される名前(全角10文字まで)
static const char*    WIFI_SSID   = "YOUR_SSID";
static const char*    WIFI_PASS   = "YOUR_PASSWORD";
static const IPAddress CORE2_IP(192, 168, 1, 50);      // Core2 の IP (待機画面に表示される)。不明なら 0,0,0,0
static const uint16_t UDP_PORT    = 50000;             // Core2 と合わせる

// 任意: StickC を固定IPにすると DHCP 待ちが無くなり接続が速くなる
static const bool      USE_STATIC_IP = false;
static const IPAddress STICK_IP(192, 168, 1, 51);
static const IPAddress GATEWAY(192, 168, 1, 1);
static const IPAddress SUBNET(255, 255, 255, 0);

static const uint32_t LONG_PRESS_MS  = 1000;  // この時間押し続けたら送信
static const uint32_t ACK_TIMEOUT_MS = 300;   // 1回送信ごとに Core2 の応答を待つ時間
static const int      SEND_RETRIES   = 10;
static const uint32_t RESULT_SHOW_MS = 1500;  // 送信成功表示の時間
// --------------------------------------

static const gpio_num_t BTN_PIN = GPIO_NUM_37;  // StickC 系のメインボタン (Low = 押下)

// ---- Core2 側と共通の電文 ----
#define NC_MAGIC 0x4E43414CUL  // "NCAL"
enum : uint8_t { NC_CALL = 1, NC_ACK = 2 };
struct __attribute__((packed)) NcMsg {
  uint32_t magic;
  uint8_t  type;
  uint8_t  battery;  // 0-100, 255 = 不明
  uint16_t seq;
  uint8_t  mac[6];   // 送信機の識別用
  char     name[32];
};

// ディープスリープ中も保持される変数 (前回つながった AP を覚えて高速接続に使う)
RTC_DATA_ATTR static uint8_t  rtcChannel = 0;  // 0 = 不明
RTC_DATA_ATTR static uint8_t  rtcBssid[6];
RTC_DATA_ATTR static uint16_t rtcSeq = 0;

static WiFiUDP udp;

// ボタンが離されてからディープスリープ (押しっぱなしだと即起床してしまうため)
[[noreturn]] static void enterDeepSleep() {
  while (digitalRead(BTN_PIN) == LOW) delay(10);
  delay(50);
  esp_sleep_enable_ext0_wakeup(BTN_PIN, 0);
  esp_deep_sleep_start();
  for (;;) {}
}

[[noreturn]] static void powerDownAndSleep() {
  udp.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  if (M5.getBoard() == m5::board_t::board_M5StickCPlus2) {
    // Plus2 は GPIO4 = HIGH で電源保持。スリープ中も保持させる
    gpio_hold_en(GPIO_NUM_4);
    gpio_deep_sleep_hold_en();
  }
  enterDeepSleep();
}

static void showMsg(const char* line1, const char* line2, uint16_t bg) {
  int cx = M5.Display.width() / 2;
  int cy = M5.Display.height() / 2;
  M5.Display.fillScreen(bg);
  M5.Display.setTextColor(TFT_WHITE, bg);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::lgfxJapanGothic_20);
  M5.Display.drawString(line1, cx, cy - 14);
  M5.Display.setFont(&fonts::lgfxJapanGothic_12);
  M5.Display.drawString(line2, cx, cy + 18);
}

static bool waitConnected(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(10);
  return WiFi.status() == WL_CONNECTED;
}

// 前回の AP(チャンネル+BSSID)指定でスキャン無しの高速接続 → ダメなら通常接続
static bool connectWiFi() {
  WiFi.persistent(false);  // 毎回フラッシュに書かない
  WiFi.mode(WIFI_STA);
  if (USE_STATIC_IP) WiFi.config(STICK_IP, GATEWAY, SUBNET, GATEWAY);

  if (rtcChannel) {
    WiFi.begin(WIFI_SSID, WIFI_PASS, rtcChannel, rtcBssid);
    if (waitConnected(3000)) return true;
    WiFi.disconnect();
    delay(100);
  }
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  if (!waitConnected(15000)) return false;
  rtcChannel = WiFi.channel();
  memcpy(rtcBssid, WiFi.BSSID(), 6);
  return true;
}

static void sendTo(const IPAddress& ip, const NcMsg& msg) {
  udp.beginPacket(ip, UDP_PORT);
  udp.write((const uint8_t*)&msg, sizeof(msg));
  udp.endPacket();
}

// Core2 の ACK が返るまで再送 (IP 指定 + ブロードキャストの両方に送る)
static bool sendCall(const NcMsg& msg) {
  udp.begin(UDP_PORT);
  for (int i = 0; i < SEND_RETRIES; i++) {
    if (CORE2_IP != IPAddress(0, 0, 0, 0)) sendTo(CORE2_IP, msg);
    sendTo(WiFi.broadcastIP(), msg);

    uint32_t t0 = millis();
    while (millis() - t0 < ACK_TIMEOUT_MS) {
      if (udp.parsePacket() == sizeof(NcMsg)) {
        NcMsg ack;
        udp.read((uint8_t*)&ack, sizeof(ack));
        if (ack.magic == NC_MAGIC && ack.type == NC_ACK && ack.seq == msg.seq) return true;
      }
      delay(5);
    }
  }
  return false;
}

void setup() {
  pinMode(BTN_PIN, INPUT);
  bool woke = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;

  if (woke) {
    // 長押し判定は M5.begin() より前に行い、短押しなら画面も点けずに寝る
    while (digitalRead(BTN_PIN) == LOW && millis() < LONG_PRESS_MS) delay(5);
    if (digitalRead(BTN_PIN) != LOW) enterDeepSleep();
  }

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(80);

  if (!woke) {
    // 電源投入直後: 案内と電池残量を出してスリープへ
    char buf[48];
    snprintf(buf, sizeof(buf), "電池 %d%%  長押しで呼出", M5.Power.getBatteryLevel());
    showMsg("ナースコール", buf, TFT_NAVY);
    delay(2000);
    powerDownAndSleep();
  }

  showMsg("接続中…", DEVICE_NAME, TFT_ORANGE);
  if (!connectWiFi()) {
    rtcChannel = 0;  // 次回は通常接続からやり直す
    showMsg("Wi-Fi接続失敗", "ルーターを確認", TFT_RED);
    delay(3000);
    powerDownAndSleep();
  }

  showMsg("送信中…", DEVICE_NAME, TFT_ORANGE);
  NcMsg msg = {};
  msg.magic = NC_MAGIC;
  msg.type  = NC_CALL;
  msg.seq   = ++rtcSeq;
  int bat   = M5.Power.getBatteryLevel();
  msg.battery = (bat < 0 || bat > 100) ? 255 : bat;
  WiFi.macAddress(msg.mac);
  strncpy(msg.name, DEVICE_NAME, sizeof(msg.name) - 1);

  if (sendCall(msg)) {
    showMsg("呼び出しました", DEVICE_NAME, TFT_DARKGREEN);
    delay(RESULT_SHOW_MS);
  } else {
    showMsg("届きません", "Core2の電源を確認", TFT_RED);
    delay(3000);
  }
  powerDownAndSleep();
}

void loop() {}
