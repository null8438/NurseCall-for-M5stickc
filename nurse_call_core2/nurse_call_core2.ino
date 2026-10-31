// =====================================================================
//  ナースコール 受信機 (M5Stack Core2)
//
//  Wi-Fi(ルーター経由)で StickC からの UDP 呼び出しを受けて、
//  画面表示＋チャイム＋振動。画面(または下の3ボタン)をタッチするまで鳴り続ける。
//  NTFY_TOPIC を設定するとスマホ(ntfy アプリ)にもプッシュ通知する。
//
//  ライブラリ: M5Unified (Arduino ライブラリマネージャから)
//  ※ USB 給電で常時起動しておく前提
// =====================================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ---------------- 設定 ----------------
static const char*    WIFI_SSID  = "YOUR_SSID";
static const char*    WIFI_PASS  = "YOUR_PASSWORD";
static const uint16_t UDP_PORT   = 50000;           // StickC と合わせる
static const char*    NTFY_TOPIC = "";              // 例 "nursecall-k3x9q2"。空文字ならスマホ通知なし
static const uint8_t  VOLUME     = 200;             // 0-255
static const bool     VIBRATION  = true;

// 任意: Core2 を固定IPにする (ルーター側の DHCP 予約でも可)。
// StickC の CORE2_IP にはこのアドレスを設定する
static const bool      USE_STATIC_IP = false;
static const IPAddress CORE2_IP(192, 168, 1, 50);
static const IPAddress GATEWAY(192, 168, 1, 1);
static const IPAddress SUBNET(255, 255, 255, 0);
// --------------------------------------

// ---- StickC 側と共通の電文 ----
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

struct CallEvent {
  uint8_t  mac[6];
  uint16_t seq;
  uint8_t  battery;
  char     name[33];
};

struct CallEntry {
  char    name[33];
  uint8_t battery;
  char    when[12];
};

static WiFiUDP       udp;
static QueueHandle_t ntfyQueue;

static const int MAX_ACTIVE  = 5;
static const int MAX_HISTORY = 5;
static CallEntry active[MAX_ACTIVE];
static int       activeCount = 0;
static CallEntry history[MAX_HISTORY];
static int       historyCount = 0;

static bool     alarming     = false;
static uint32_t alarmStart   = 0;
static uint32_t lastCycle    = UINT32_MAX;
static int      stage        = 0;
static uint32_t lastIdleDraw = 0;
static bool     lastWifiOk   = false;

// ACK が StickC に届かず再送された場合や、IP指定とブロードキャストの二重受信を弾く
struct SeenSeq {
  uint8_t  mac[6];
  uint16_t seq;
  uint32_t at;
  bool     used;
};
static SeenSeq seen[8];

static bool isDuplicate(const CallEvent& ev) {
  uint32_t now = millis();
  for (auto& s : seen) {
    if (s.used && memcmp(s.mac, ev.mac, 6) == 0) {
      bool dup = (s.seq == ev.seq) && (now - s.at < 30000);
      s.seq = ev.seq;
      s.at  = now;
      return dup;
    }
  }
  SeenSeq* slot = &seen[0];
  for (auto& s : seen) {
    if (!s.used) { slot = &s; break; }
    if (s.at < slot->at) slot = &s;
  }
  memcpy(slot->mac, ev.mac, 6);
  slot->seq  = ev.seq;
  slot->at   = now;
  slot->used = true;
  return false;
}

// ---------------- スマホ通知 (ntfy) ----------------
static String jsonEscape(const char* s) {
  String out;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') out += '\\';
    out += *s;
  }
  return out;
}

static bool postNtfy(const CallEvent& ev) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, "https://ntfy.sh/")) return false;
  http.addHeader("Content-Type", "application/json");

  String msg = jsonEscape(ev.name) + " から呼び出しがありました";
  if (ev.battery <= 100) msg += " (送信機の電池 " + String(ev.battery) + "%)";
  String body = String("{\"topic\":\"") + NTFY_TOPIC + "\","
                "\"title\":\"ナースコール\","
                "\"message\":\"" + msg + "\","
                "\"priority\":5,"
                "\"tags\":[\"rotating_light\"]}";
  int code = http.POST(body);
  http.end();
  return code >= 200 && code < 300;
}

static void ntfyTask(void*) {
  CallEvent ev;
  for (;;) {
    if (xQueueReceive(ntfyQueue, &ev, portMAX_DELAY) != pdTRUE) continue;
    for (int i = 0; i < 3; i++) {
      if (WiFi.status() == WL_CONNECTED && postNtfy(ev)) break;
      vTaskDelay(pdMS_TO_TICKS(3000));
    }
  }
}

// ---------------- 表示 ----------------
static void nowStr(char* out, size_t n) {
  struct tm t;
  if (getLocalTime(&t, 0)) strftime(out, n, "%H:%M", &t);
  else snprintf(out, n, "+%lu分", (unsigned long)(millis() / 60000));
}

static void drawIdle() {
  auto& d = M5.Display;
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  d.setBrightness(wifiOk ? 60 : 160);
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(top_left);

  d.setFont(&fonts::lgfxJapanGothicP_28);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("ナースコール 待機中", 10, 8);

  char clock[12];
  nowStr(clock, sizeof(clock));
  d.setFont(&fonts::lgfxJapanGothic_16);
  if (wifiOk) {
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    String status = String(clock) + "  IP " + WiFi.localIP().toString();
    if (NTFY_TOPIC[0]) status += "  スマホ通知ON";
    d.drawString(status, 10, 46);
  } else {
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.drawString("Wi-Fi 未接続！ 呼び出しを受けられません", 10, 46);
  }

  d.drawFastHLine(10, 70, 300, TFT_DARKGREY);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("最近の呼び出し", 10, 78);
  d.setFont(&fonts::lgfxJapanGothic_20);
  if (historyCount == 0) {
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("なし", 20, 104);
  }
  for (int i = 0; i < historyCount; i++) {
    d.drawString(String(history[i].when) + "  " + history[i].name, 20, 104 + i * 26);
  }
  lastIdleDraw = millis();
  lastWifiOk   = wifiOk;
}

static void drawAlarm(bool bright) {
  auto& d = M5.Display;
  uint16_t bg = bright ? TFT_RED : TFT_MAROON;
  d.fillScreen(bg);
  d.setTextColor(TFT_WHITE, bg);
  d.setTextDatum(top_center);

  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.drawString("呼び出し", 160, 10);

  d.setFont(&fonts::lgfxJapanGothicP_32);
  for (int i = 0; i < activeCount; i++) {
    d.drawString(String(active[i].name) + "  " + active[i].when, 160, 64 + i * 38);
  }
  d.setFont(&fonts::lgfxJapanGothic_16);
  d.setTextDatum(bottom_center);
  d.drawString("画面タッチで停止", 160, 236);
}

// ---------------- 呼び出し管理 ----------------
static void addActive(const CallEvent& ev) {
  CallEntry e = {};
  strncpy(e.name, ev.name, sizeof(e.name) - 1);
  e.battery = ev.battery;
  nowStr(e.when, sizeof(e.when));

  for (int i = 0; i < activeCount; i++) {  // 同じ送信機からの再呼び出しは時刻だけ更新
    if (strcmp(active[i].name, e.name) == 0) { active[i] = e; return; }
  }
  if (activeCount < MAX_ACTIVE) active[activeCount++] = e;
  else active[MAX_ACTIVE - 1] = e;
}

static void onCall(const CallEvent& ev) {
  if (isDuplicate(ev)) return;
  addActive(ev);
  if (NTFY_TOPIC[0]) xQueueSend(ntfyQueue, &ev, 0);
  if (!alarming) {
    alarming   = true;
    alarmStart = millis();
    lastCycle  = UINT32_MAX;
    M5.Display.wakeup();
    M5.Display.setBrightness(255);
  } else {
    drawAlarm(true);  // 呼び出し元が増えたら即再描画
  }
}

static void acknowledge() {
  for (int i = 0; i < activeCount; i++) {
    memmove(&history[1], &history[0], sizeof(CallEntry) * (MAX_HISTORY - 1));
    history[0] = active[i];
    if (historyCount < MAX_HISTORY) historyCount++;
  }
  activeCount = 0;
  alarming    = false;
  M5.Speaker.stop();
  if (VIBRATION) M5.Power.setVibration(0);
  drawIdle();
}

static bool stopPressed() {
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) return true;
  return M5.Touch.getCount() > 0 && M5.Touch.getDetail().wasPressed();
}

// ---------------- UDP 受信 ----------------
static void pollUdp() {
  int len;
  while ((len = udp.parsePacket()) > 0) {
    if (len != sizeof(NcMsg)) continue;
    NcMsg m;
    udp.read((uint8_t*)&m, sizeof(m));
    if (m.magic != NC_MAGIC || m.type != NC_CALL) continue;

    // 先に ACK を返す (StickC はこれを受け取るまで再送してくる)
    NcMsg ack = m;
    ack.type  = NC_ACK;
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write((const uint8_t*)&ack, sizeof(ack));
    udp.endPacket();

    CallEvent ev = {};
    memcpy(ev.mac, m.mac, 6);
    ev.seq     = m.seq;
    ev.battery = m.battery;
    memcpy(ev.name, m.name, sizeof(m.name));
    ev.name[32] = '\0';
    onCall(ev);
  }
}

// ---------------- setup / loop ----------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.setVolume(VOLUME);

  ntfyQueue = xQueueCreate(8, sizeof(CallEvent));

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(&fonts::lgfxJapanGothic_20);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("Wi-Fi 接続中…", 10, 10);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // 省電力モードだと受信が遅れるため無効化
  WiFi.setAutoReconnect(true);
  if (USE_STATIC_IP) WiFi.config(CORE2_IP, GATEWAY, SUBNET, GATEWAY);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) delay(100);
  configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org");

  udp.begin(UDP_PORT);

  if (NTFY_TOPIC[0]) xTaskCreatePinnedToCore(ntfyTask, "ntfy", 10240, nullptr, 1, nullptr, 0);

  drawIdle();
}

void loop() {
  M5.update();
  pollUdp();

  if (alarming) {
    if (stopPressed()) {
      acknowledge();
      return;
    }
    // 2秒周期で「ピン・ポーン」＋振動＋画面点滅
    uint32_t el    = millis() - alarmStart;
    uint32_t cycle = el / 2000;
    uint32_t ph    = el % 2000;
    if (cycle != lastCycle) {
      lastCycle = cycle;
      stage     = 0;
    }
    if (stage == 0) {
      M5.Speaker.tone(1568, 350);  // G6
      if (VIBRATION) M5.Power.setVibration(200);
      drawAlarm(true);
      stage = 1;
    } else if (stage == 1 && ph >= 400) {
      M5.Speaker.tone(1175, 600);  // D6
      stage = 2;
    } else if (stage == 2 && ph >= 1000) {
      if (VIBRATION) M5.Power.setVibration(0);
      drawAlarm(false);
      stage = 3;
    }
  } else {
    bool wifiOk = WiFi.status() == WL_CONNECTED;
    if (wifiOk != lastWifiOk || millis() - lastIdleDraw > 30000) drawIdle();  // 時刻・Wi-Fi 状態の更新
  }

  delay(5);
}
