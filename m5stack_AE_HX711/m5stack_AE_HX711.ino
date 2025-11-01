/***** M5Stack Core : DISPLAY/SETTING Unit (ESP-NOW) *****/
#include <M5Stack.h>
#include <WiFi.h>
#include <esp_now.h>

// -------- ESP-NOW peer (MEASUREMENT unit MACを入れる) --------
static uint8_t MEAS_PEER_MAC[6] = {0x08,0x3A,0xF2,0x2D,0x41,0x54};

// -------- Packets (MEASと一致させる) --------
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
  float   value; // SET_ITEM_WEIGHTのみ
} CmdPacket;

// -------- Globals --------
volatile bool hasNew = false;
MeasPacket latest{};
float lastItemWeight = 0.0f;

// UI state
enum UiMode { VIEW, INPUT_ITEM };
UiMode ui = VIEW;
float inputWeight = 10.00f; // 初期候補

// -------- Forward --------
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len);
void sendCmd(uint8_t cmd, float value=0.0f);
void drawView();
void drawInput();
void drawHeader();
void drawFooter(const char* txt);
void clearCanvas();

// =========================================================
void setup() {
  M5.begin();
  M5.Lcd.setRotation(1); // 横向き
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE, BLACK);

  WiFi.mode(WIFI_STA);
  M5.Lcd.setTextSize(2);
  M5.Lcd.printf("DISPLAY Unit\nMy MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("DISPLAY Unit\nMy MAC: %s\n", WiFi.macAddress().c_str());

  if (esp_now_init() != ESP_OK) {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.println("ESP-NOW init failed");
    while(1) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);

  // Peer (MEAS)
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, MEAS_PEER_MAC, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 1;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.println("add_peer failed");
    while(1) delay(1000);
  }

  // 初期描画
  drawView();
}

// =========================================================
void loop() {
  M5.update();

  if (ui == VIEW) {
    if (hasNew) { hasNew = false; drawView(); }

    if (M5.BtnA.wasPressed()) {        // 初期化=タレ
      sendCmd(0, 0.0f);
      drawFooter("TARE sent");
    }
    if (M5.BtnB.wasPressed()) {        // 単品重量=現在合計
      sendCmd(1, 0.0f);
      drawFooter("MEASURE_ITEM sent");
    }
    if (M5.BtnC.wasPressed()) {        // g数入力画面へ
      inputWeight = (latest.itemWeight > 0) ? latest.itemWeight : inputWeight;
      ui = INPUT_ITEM;
      drawInput();
    }
  } else if (ui == INPUT_ITEM) {
    bool changed = false;
    if (M5.BtnA.wasPressed() && M5.BtnB.wasPressed()) {
      // キャンセル（A+B同時）
      ui = VIEW; drawView();
    } else if (M5.BtnA.wasPressed()) {
      inputWeight = max(0.0f, inputWeight - 0.1f);
      changed = true;
    } else if (M5.BtnB.wasPressed()) {
      inputWeight += 0.1f;
      changed = true;
    } else if (M5.BtnC.wasPressed()) {
      // 決定→送信
      sendCmd(2, inputWeight);
      ui = VIEW; drawFooter("SET_ITEM_WEIGHT sent");
    }
    if (changed) drawInput();
  }

  delay(10);
}

// ================== ESP-NOW RX ===========================
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len != sizeof(MeasPacket)) return;
  memcpy((void*)&latest, incomingData, sizeof(MeasPacket));
  hasNew = true;
}

// ================== Send helper ==========================
void sendCmd(uint8_t cmd, float value) {
  CmdPacket pkt{cmd, value};
  esp_now_send(MEAS_PEER_MAC, (uint8_t*)&pkt, sizeof(pkt));
}

// ================== UI drawing ===========================
void drawHeader() {
  M5.Lcd.fillRect(0,0,320,18,0x0841);
  M5.Lcd.setTextColor(WHITE, 0x0841);
  M5.Lcd.setCursor(4,2);
  M5.Lcd.print("Weight Viewer/Setter");
  M5.Lcd.setTextColor(WHITE, BLACK);
}

void drawFooter(const char* txt) {
  M5.Lcd.fillRect(0,220,320,20,0x2965);
  M5.Lcd.setTextColor(WHITE, 0x2965);
  M5.Lcd.setCursor(4,224);
  M5.Lcd.print(txt);
  M5.Lcd.setTextColor(WHITE, BLACK);
}

void clearCanvas() {
  M5.Lcd.fillRect(0,18,320,202,BLACK);
}

void drawView() {
  drawHeader();
  clearCanvas();

  // 大：個数 & 合計
  M5.Lcd.setTextSize(4);
  M5.Lcd.setCursor(6, 24);
  M5.Lcd.printf("Count: %u", latest.itemCount);

  M5.Lcd.setCursor(6, 72);
  M5.Lcd.printf("Sum: %0.2f g", latest.gsum);

  // 中：各CH & 単品重量
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(6, 120);
  M5.Lcd.printf("CH1: %0.2f g", latest.g1);
  M5.Lcd.setCursor(6, 150);
  M5.Lcd.printf("CH2: %0.2f g", latest.g2);
  M5.Lcd.setCursor(6, 180);
  M5.Lcd.printf("Item: %0.2f g", latest.itemWeight);

  // 小：経過時間
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(200, 180);
  M5.Lcd.printf("t= %lus", (unsigned long)latest.elapsedSec);

  // ボタンラベル
  M5.Lcd.drawRect(0,202,106,18,0x7BEF);   M5.Lcd.setCursor(8,206);  M5.Lcd.print("[A] Tare");
  M5.Lcd.drawRect(107,202,106,18,0x7BEF); M5.Lcd.setCursor(112,206);M5.Lcd.print("[B] Measure");
  M5.Lcd.drawRect(214,202,106,18,0x7BEF); M5.Lcd.setCursor(218,206);M5.Lcd.print("[C] Set g");

  drawFooter("Ready");
}

void drawInput() {
  drawHeader();
  clearCanvas();

  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(6, 40);
  M5.Lcd.print("Input item weight:");

  M5.Lcd.setTextSize(6);
  M5.Lcd.setCursor(6, 90);
  M5.Lcd.printf("%0.2f g", inputWeight);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(6, 170);
  M5.Lcd.print("A:-0.1g   B:+0.1g");
  M5.Lcd.setCursor(6, 195);
  M5.Lcd.print("C:Confirm   A+B:Cancel");

  drawFooter("Editing...");
}
