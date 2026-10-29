#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "driver/i2s.h"

// ---------- Wi-Fi and hosted bridge ----------
const char* WIFI_SSID = "Redmi12";
const char* WIFI_PASSWORD = "Homam123";
const char* BRIDGE_HOST = "esp32-gemini-live.onrender.com";// host only; no https://
const uint16_t BRIDGE_PORT = 443;
const char* BRIDGE_PATH = "/";

// ---------- Pin map based on the supplied wiring image ----------
// INMP441: SCK/BCLK=GPIO6, WS=GPIO5, SD/DOUT=GPIO4
static const int MIC_BCLK = 6;
static const int MIC_WS   = 5;
static const int MIC_DOUT = 4;
// MAX98357A: BCLK=GPIO16, LRC/WS=GPIO15, DIN=GPIO7.
// SD/EN should be tied HIGH to 3V3 as shown/verified on your module.
static const int SPK_BCLK = 16;
static const int SPK_WS   = 15;
static const int SPK_DIN  = 7;
// OLED: SDA=GPIO41, SCL=GPIO42 (verify the two wires against module labels).
static const int OLED_SDA = 41;
static const int OLED_SCL = 42;

static const int INPUT_RATE = 16000;
static const int OUTPUT_RATE = 24000;
static const size_t FRAMES_PER_CHUNK = 1600; // 100 ms at 16 kHz

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
WebSocketsClient webSocket;

int32_t mic32[FRAMES_PER_CHUNK];
int16_t mic16[FRAMES_PER_CHUNK];
volatile bool bridgeConnected = false;
unsigned long lastStatusDraw = 0;

void showStatus(const char* line1, const char* line2 = "") {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 16, line1);
  oled.drawStr(0, 34, line2);
  oled.sendBuffer();
}

bool installI2S() {
  i2s_config_t rx = {};
  rx.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  rx.sample_rate = INPUT_RATE;
  rx.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  rx.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  rx.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  rx.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  rx.dma_buf_count = 8;
  rx.dma_buf_len = 256;
  rx.use_apll = false;
  rx.tx_desc_auto_clear = false;
  rx.fixed_mclk = 0;

  i2s_pin_config_t rxPins = {};
  rxPins.bck_io_num = MIC_BCLK;
  rxPins.ws_io_num = MIC_WS;
  rxPins.data_out_num = I2S_PIN_NO_CHANGE;
  rxPins.data_in_num = MIC_DOUT;

  esp_err_t e = i2s_driver_install(I2S_NUM_0, &rx, 0, nullptr);
  if (e != ESP_OK) {
    Serial.printf("I2S RX install error: %d\n", (int)e);
    return false;
  }
  e = i2s_set_pin(I2S_NUM_0, &rxPins);
  if (e != ESP_OK) {
    Serial.printf("I2S RX pin error: %d\n", (int)e);
    return false;
  }

  i2s_config_t tx = {};
  tx.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  tx.sample_rate = OUTPUT_RATE;
  tx.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  tx.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  tx.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  tx.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  tx.dma_buf_count = 8;
  tx.dma_buf_len = 256;
  tx.use_apll = false;
  tx.tx_desc_auto_clear = true;
  tx.fixed_mclk = 0;

  i2s_pin_config_t txPins = {};
  txPins.bck_io_num = SPK_BCLK;
  txPins.ws_io_num = SPK_WS;
  txPins.data_out_num = SPK_DIN;
  txPins.data_in_num = I2S_PIN_NO_CHANGE;

  e = i2s_driver_install(I2S_NUM_1, &tx, 0, nullptr);
  if (e != ESP_OK) {
    Serial.printf("I2S TX install error: %d\n", (int)e);
    return false;
  }
  e = i2s_set_pin(I2S_NUM_1, &txPins);
  if (e != ESP_OK) {
    Serial.printf("I2S TX pin error: %d\n", (int)e);
    return false;
  }
  i2s_zero_dma_buffer(I2S_NUM_1);
  return true;
}

void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      bridgeConnected = true;
      Serial.println("Connected to Gemini bridge");
      showStatus("Gemini baglandi", "Konusabilirsiniz");
      break;
    case WStype_DISCONNECTED:
      bridgeConnected = false;
      Serial.println("Bridge disconnected");
      showStatus("Sunucu baglantisi", "kesildi / tekrar deniyor");
      break;
    case WStype_BIN: {
      // Gemini Live audio output: signed 16-bit little-endian PCM at 24 kHz.
      if (length > 0) {
        size_t written = 0;
        const esp_err_t e = i2s_write(I2S_NUM_1, payload, length, &written, pdMS_TO_TICKS(100));
        if (e != ESP_OK) Serial.printf("I2S playback error: %d\n", (int)e);
      }
      break;
    }
    case WStype_ERROR:
      Serial.println("WebSocket error");
      break;
    default:
      break;
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  showStatus("Wi-Fi baglaniyor", "Bekleyin...");
  Serial.print("Connecting Wi-Fi");
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi IP: ");
    Serial.println(WiFi.localIP());
    showStatus("Wi-Fi baglandi", "Sunucuya baglaniyor");
  } else {
    showStatus("Wi-Fi hatasi", "SSID/sifreyi kontrol et");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin();
  oled.setContrast(255);
  showStatus("ESP32-S3 Gemini", "Baslatiliyor...");

  if (!installI2S()) {
    showStatus("I2S baslatilamadi", "Pinleri kontrol et");
    while (true) delay(1000);
  }

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    webSocket.beginSSL(BRIDGE_HOST, BRIDGE_PORT, BRIDGE_PATH);
    // Prototype convenience only: disables server certificate verification.
    // For production, install/validate the correct root CA instead.
    webSocket.setInsecure();
    webSocket.onEvent(onWebSocketEvent);
    webSocket.setReconnectInterval(5000);
    webSocket.enableHeartbeat(15000, 3000, 2);
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    delay(10);
    return;
  }
  webSocket.loop();

  // Read 32-bit I2S slots from INMP441 and convert to mono signed 16-bit PCM.
  size_t bytesRead = 0;
  const size_t bytesWanted = sizeof(mic32);
  esp_err_t e = i2s_read(I2S_NUM_0, mic32, bytesWanted, &bytesRead, pdMS_TO_TICKS(20));
  if (e != ESP_OK || bytesRead < sizeof(int32_t) || !bridgeConnected) return;

  const size_t sampleCount = bytesRead / sizeof(int32_t);
  for (size_t i = 0; i < sampleCount; ++i) {
    // INMP441 commonly presents 24-bit samples in a 32-bit I2S slot.
    int32_t sample = mic32[i] >> 14;
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    mic16[i] = (int16_t)sample;
  }

  // Each frame is raw PCM, not WAV. The bridge forwards it to Gemini Live.
  const size_t pcmBytes = sampleCount * sizeof(int16_t);
  if (pcmBytes > 0) {
    webSocket.sendBIN((uint8_t*)mic16, pcmBytes);
  }
}
