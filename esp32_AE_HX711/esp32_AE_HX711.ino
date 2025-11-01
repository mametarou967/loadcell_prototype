/***** ESP32 DevKitC : Weight MEASUREMENT Unit (HX711 x2 + ESP-NOW)
 * 変更点:
 *  - CMD: MEASURE_ITEM(=1) 受信時に再計測せず、直近の合計重量(lastGsum)をそのまま itemWeight に採用
 *****/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// -------- Pins (DevKitC) --------
#define DOUT1 34
#define SCK1  25
#define DOUT2 35
#define SCK2  26

// -------- Loadcell specs (例) --------
// CH1: SC133 20kg
#define OUT_VOL1  0.002f
#define LOAD1     20000.0f
// CH2: SC133 20kg
#define OUT_VOL2  0.002f
#define LOAD2     20000.0f

// -------- HX711 constants --------
#define HX711_AVDD      4.2987f
#define HX711_ADC1bit   (HX711_AVDD / 16777216.0f)
#define HX711_PGA       128.0f

// -------- ESP-NOW peer (DISPLAY unit MACを入れる) --------
static uint8_t DISPLAY_PEER_MAC[6] = {0x98,0xF4,0xAB,0x6B,0x8A,0x48};

// -------- Packets --------
typedef struct __attribute__((packed)) {
  float g1;
  float g2;
  float gsum;
  float itemWeight;
  uint16_t itemCount;
  uint32_t elapsedSec;
  uint32_t seq;
} MeasPacket;

typedef struct __attribute__((packed)) {
  uint8_t cmd;   // 0=TARE, 1=MEASURE_ITEM, 2=SET_ITEM_WEIGHT
  float   value; // SET_ITEM_WEIGHTのみ使用
} CmdPacket;

// -------- Globals --------
float offset1 = 0.0f, offset2 = 0.0f;
float itemWeight = 0.0f;     // 1部材あたり重量 [g]
uint32_t tareStartMs = 0;
uint32_t seq = 0;

// CHANGE: 直近の合計重量を保持（表示中の値に相当）
volatile float lastGsum = 0.0f;

// -------- Forward --------
void hx711_init(int dout, int sck);
void hx711_reset(int sck);
long hx711_read(int dout, int sck);
float hx711_getGram(int dout, int sck, float OUT_VOL, float LOAD, char avg);
uint16_t itemsFromTotal(float total, float perItem);
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len);

// =========================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[MEAS] ESP32 DevKitC Weight Unit");

  // Show own MAC
  WiFi.mode(WIFI_STA);
  Serial.print("[MEAS] My MAC: ");
  Serial.println(WiFi.macAddress());

  // ESP-NOW init
  if (esp_now_init() != ESP_OK) {
    Serial.println("[MEAS] ESP-NOW init failed");
    while (1) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);

  // Add peer (DISPLAY)
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, DISPLAY_PEER_MAC, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 1;    // 双方で合わせる
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[MEAS] add_peer failed");
  }

  // HX711 init
  hx711_init(DOUT1, SCK1);
  hx711_init(DOUT2, SCK2);
  hx711_reset(SCK1);
  hx711_reset(SCK2);
  delay(300);

  // Tare
  offset1 = hx711_getGram(DOUT1, SCK1, OUT_VOL1, LOAD1, 20);
  offset2 = hx711_getGram(DOUT2, SCK2, OUT_VOL2, LOAD2, 20);
  tareStartMs = millis();
  Serial.printf("[MEAS] Tare done: off1=%.3f, off2=%.3f\n", offset1, offset2);
}

// =========================================================
void loop() {
  // 計測（表示に使う直近値を更新）
  float g1 = hx711_getGram(DOUT1, SCK1, OUT_VOL1, LOAD1, 5) - offset1;
  float g2 = hx711_getGram(DOUT2, SCK2, OUT_VOL2, LOAD2, 5) - offset2;
  float gsum = g1 + g2;
  if (fabs(gsum) < 0.01f) gsum = 0.0f; // 微小ノイズ丸め

  // CHANGE: lastGsum を更新（表示中の合計重量）
  lastGsum = gsum;

  // 個数算出（0.5個閾値）
  uint16_t cnt = itemsFromTotal(gsum, itemWeight);
  uint32_t elapsedSec = (millis() - tareStartMs) / 1000UL;

  // 送信
  MeasPacket pkt{};
  pkt.g1 = g1; pkt.g2 = g2; pkt.gsum = gsum;
  pkt.itemWeight = itemWeight;
  pkt.itemCount = cnt;
  pkt.elapsedSec = elapsedSec;
  pkt.seq = ++seq;

  esp_now_send(DISPLAY_PEER_MAC, (uint8_t*)&pkt, sizeof(pkt));

  // デバッグ表示
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    Serial.printf("[MEAS] g1=%7.3f g2=%7.3f sum=%7.3f  item=%.3f cnt=%u t=%lus\n",
                  g1, g2, gsum, itemWeight, cnt, (unsigned long)elapsedSec);
  }

  delay(200); // 5Hz
}

// ================== ESP-NOW RX (commands) =================
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len != sizeof(CmdPacket)) return;
  CmdPacket cmd{};
  memcpy(&cmd, incomingData, sizeof(cmd));

  if (cmd.cmd == 0) { // TARE
    offset1 = hx711_getGram(DOUT1, SCK1, OUT_VOL1, LOAD1, 20);
    offset2 = hx711_getGram(DOUT2, SCK2, OUT_VOL2, LOAD2, 20);
    tareStartMs = millis();
    Serial.println("[MEAS] CMD: TARE");
  } else if (cmd.cmd == 1) { // MEASURE_ITEM（再計測しない）
    // CHANGE: 直近表示重量をそのまま採用
    float gsum_now = lastGsum;               // 直近ループで更新済み
    if (gsum_now < 0) gsum_now = -gsum_now;  // 念のため絶対値
    itemWeight = max(0.0f, gsum_now);
    Serial.printf("[MEAS] CMD: MEASURE_ITEM -> itemWeight=%.3f g (from lastGsum)\n", itemWeight);
  } else if (cmd.cmd == 2) { // SET_ITEM_WEIGHT
    itemWeight = max(0.0f, cmd.value);
    Serial.printf("[MEAS] CMD: SET_ITEM_WEIGHT -> itemWeight=%.3f g\n", itemWeight);
  }
}

// ================== HX711 helpers ========================
void hx711_init(int dout, int sck) {
  pinMode(sck, OUTPUT);
  pinMode(dout, INPUT);
  digitalWrite(sck, LOW);
}
void hx711_reset(int sck) {
  digitalWrite(sck, HIGH); delayMicroseconds(120);
  digitalWrite(sck, LOW);  delayMicroseconds(120);
}
long hx711_read(int dout, int sck) {
  unsigned long data = 0;
  uint32_t start = micros();
  while (digitalRead(dout) != LOW) {
    if (micros() - start > 500000) return 0; // timeout 0.5s
  }
  for (int i=0;i<24;i++){
    digitalWrite(sck,HIGH); delayMicroseconds(1);
    data = (data<<1) | (digitalRead(dout)?1:0);
    digitalWrite(sck,LOW);  delayMicroseconds(1);
  }
  digitalWrite(sck,HIGH); delayMicroseconds(1);
  digitalWrite(sck,LOW);

  if (data & 0x800000UL) data |= 0xFF000000UL; // sign extend
  return (long)data;
}
float hx711_getGram(int dout, int sck, float OUT_VOL, float LOAD, char avgN) {
  long sum=0; for(int i=0;i<avgN;i++) sum += hx711_read(dout,sck);
  long avg = sum / max(1, (int)avgN);
  float v = avg * HX711_ADC1bit;
  float scale = (OUT_VOL * HX711_AVDD / LOAD * HX711_PGA);
  return ( v / scale ) * 2.0;
}

// 0.5個閾値の丸め規則
uint16_t itemsFromTotal(float total, float perItem) {
  if (perItem <= 0.0f) return 0;
  float n = (total + 0.5f * perItem) / perItem; // 0.5閾値
  if (n < 0) n = 0;
  return (uint16_t)floorf(n + 1e-6f);
}
