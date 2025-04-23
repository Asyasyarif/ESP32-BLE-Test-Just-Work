#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
// #include <WiFi.h>
#include "esp_sleep.h"
#include "esp_bt_main.h"
#include "esp_bt.h"
#include "esp_wifi.h"

// Debugging macro
#define DEBUG 1
#if DEBUG
  #define LOG(x) Serial.println(x)
  #define LOGV(x, v) Serial.printf(x, v)
#else
  #define LOG(x)
  #define LOGV(x, v)
#endif

// RFID Pins
#define SS_PIN 5
#define RST_PIN 22
#define INT_PIN 4
#define LED_CONNECTED_PIN 2

// Waktu dalam milidetik
#define WAKE_DURATION 30000        // 30 detik waktu aktif saat tidak terhubung
#define DEBOUNCE_DELAY 200         // 200ms debounce
#define HEARTBEAT_INTERVAL 60000   // 30 detik untuk heartbeat
#define SLEEP_DURATION 10000       // 10 detik light sleep
#define DEEP_SLEEP_TIMEOUT 60000   // 60 detik tanpa koneksi

MFRC522DriverPinSimple ss_pin(SS_PIN);
MFRC522DriverSPI driver{ss_pin};
MFRC522 mfrc522{driver};

// Status variabel
volatile bool cardPresent = false;
bool deviceConnected = false;
BLEServer *pServer = nullptr;
unsigned long lastWakeTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastConnectedTime = 0;
bool isAdvertising = false;
bool rfidInitialized = false; // Flag untuk status inisialisasi RFID

bool sleepAfterNotify = false;
unsigned long sleepRequestTime = 0;

// BLE setup
BLECharacteristic* pNotifyCharacteristic;
BLECharacteristic* pWriteCharacteristic;
#define SERVICE_UUID           "48aac3ec-923e-4844-ab3d-7dded9a4acc6"
#define CHARACTERISTIC_NOTIFY  "370e2728-ad13-4006-942f-275dcb00c580"
#define CHARACTERISTIC_WRITE   "62f4f090-82db-4409-9b0c-3ced7217d627"

// Status LED
bool ledStatus = false;

// Deklarasi fungsi goToSleep
void goToSleep();
void startAdvertising();
void stopAdvertising();

// Callback untuk karakteristik WRITE
class MyCharacteristicCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue();
    if (value.length() > 0) {
      LOG("📥 Data diterima dari client");
      lastWakeTime = millis();
    }
  }
};

// Callback untuk server BLE
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    lastConnectedTime = millis();
    lastWakeTime = millis();
    digitalWrite(LED_CONNECTED_PIN, HIGH);
    ledStatus = true;
    LOG("✅ BLE device connected");
    // stopAdvertising(); // Pastikan advertising berhenti saat terhubung
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    digitalWrite(LED_CONNECTED_PIN, LOW);
    ledStatus = false;
    lastConnectedTime = millis();
    LOG("❌ BLE device disconnected");
    startAdvertising();
  }
};

static MyServerCallbacks serverCallbacks;
static MyCharacteristicCallbacks characteristicCallbacks;

// Interrupt handler untuk RFID
void IRAM_ATTR rfidInterrupt() {
  cardPresent = true;
}

// Generate nama BLE unik
String getUniqueBleName(const String& baseName) {
  uint64_t chipid = ESP.getEfuseMac();
  uint16_t shortID = (uint16_t)(chipid & 0xFFFF);
  char uniqueName[32];
  sprintf(uniqueName, "%s-%04X", baseName.c_str(), shortID);
  return String(uniqueName);
}

// Fungsi untuk mengatur advertising
void startAdvertising() {
  if (pServer && !isAdvertising && !deviceConnected) {
    BLEAdvertising *pAdvertising = pServer->getAdvertising();
    pAdvertising->setMinInterval(500);
    pAdvertising->setMaxInterval(1000);
    pAdvertising->start();
    isAdvertising = true;
    LOG("📢 Memulai BLE advertising");
  }
}

void stopAdvertising() {
  if (pServer && isAdvertising) {
    pServer->getAdvertising()->stop();
    isAdvertising = false;
    LOG("🛑 Menghentikan BLE advertising");
  }
}

// Inisialisasi BLE
void initBLE() {
  String bleName = getUniqueBleName("AGRX0");
  BLEDevice::init(bleName.c_str());
  BLEDevice::setPower(ESP_PWR_LVL_P3);
  LOG("📛 BLE Name: " + bleName);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pNotifyCharacteristic = pService->createCharacteristic(
                          CHARACTERISTIC_NOTIFY,
                          BLECharacteristic::PROPERTY_NOTIFY);
  pNotifyCharacteristic->addDescriptor(new BLE2902());

  pWriteCharacteristic = pService->createCharacteristic(
                          CHARACTERISTIC_WRITE,
                          BLECharacteristic::PROPERTY_WRITE);
  pWriteCharacteristic->setCallbacks(&characteristicCallbacks);

  pService->start();
  startAdvertising();
  LOG("🚀 BLE ready");
}

// Inisialisasi RFID
void initRFID() {
  if (!rfidInitialized) {
    mfrc522.PCD_Init();
    rfidInitialized = true;
    LOG("🔍 RFID ready. Tap your card...");
  }
}

// Matikan RFID
void disableRFID() {
  mfrc522.PCD_AntennaOff();
  rfidInitialized = false;
  LOG("🔌 RFID dimatikan untuk sleep");
}

// Kirim heartbeat
void sendHeartbeat() {
  if (deviceConnected) {

    lastHeartbeatTime = millis();
    lastWakeTime = millis();

    const char* heartbeat = "PING";
    LOG("📡 Heartbeat PING dikirim");


    pNotifyCharacteristic->setValue(heartbeat);
    pNotifyCharacteristic->notify();

    // Langsung sleep jika tidak ada kartu RFID
    if (!cardPresent) {
      sleepAfterNotify = true;
      sleepRequestTime = millis(); // tunggu delay sebelum tidur
    }
  }
}

// Proses kartu RFID
void handleRFIDCard() {
  static unsigned long lastCardTime = 0;
  if (cardPresent && (millis() - lastCardTime > DEBOUNCE_DELAY)) {
    cardPresent = false;
    lastCardTime = millis();

    if (!rfidInitialized) {
      initRFID();
    }

    if (!mfrc522.PICC_IsNewCardPresent()) return;
    if (!mfrc522.PICC_ReadCardSerial()) return;

    char uidStr[17];
    int pos = 0;
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) {
        uidStr[pos++] = '0';
      }
      sprintf(&uidStr[pos], "%02X", mfrc522.uid.uidByte[i]);
      pos += 2;
    }
    uidStr[pos] = '\0';
    for (int i = 0; uidStr[i]; i++) {
      if (uidStr[i] >= 'a' && uidStr[i] <= 'f') {
        uidStr[i] -= 32;
      }
    }

    if (strlen(uidStr) >= 8) {
      LOG("🎫 Kartu terdeteksi! UID: ");
      if (deviceConnected) {
        pNotifyCharacteristic->setValue(uidStr);
        pNotifyCharacteristic->notify();
        LOG("📤 UID dikirim via BLE");
        lastWakeTime = millis();
      }
    }

    mfrc522.PICC_HaltA();
  }
}

// Fungsi untuk masuk ke light sleep
void goToSleep() {

  if (deviceConnected) {
    LOG("⚠️ Skip sleep karena BLE masih aktif");
    return;
  }
  
  LOG("😴 Memasuki mode light sleep...");
  disableRFID();

  delay(100); // Jaga-jaga
  esp_light_sleep_start();

  LOG("🌞 Terbangun dari light sleep");
  delay(200); // Beri waktu stabilisasi lebih lama
  LOGV("deviceConnected after wakeup: %d\n", deviceConnected);
  initRFID();
  if (!deviceConnected) {
    startAdvertising();
  }
  lastWakeTime = millis();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    cardPresent = true;
    LOG("📡 Wakeup disebabkan oleh RFID interrupt");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    LOG("⏰ Wakeup disebabkan oleh timer");
  }
}

// Fungsi untuk masuk ke deep sleep
void goToDeepSleep() {
  LOG("💤 Memasuki mode deep sleep...");
  stopAdvertising();
  disableRFID();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)INT_PIN, 0);
  esp_bt_controller_disable();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  setCpuFrequencyMhz(80);

  // WiFi.mode(WIFI_OFF);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  LOG("📴 Wi-Fi dinonaktifkan");

  pinMode(LED_CONNECTED_PIN, OUTPUT);
  digitalWrite(LED_CONNECTED_PIN, LOW);

  initBLE();
  initRFID();

  pinMode(INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), rfidInterrupt, FALLING);

  // esp_sleep_enable_ext0_wakeup((gpio_num_t)INT_PIN, 0);
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION * 1000ULL * 1000);


  lastWakeTime = millis();
  lastHeartbeatTime = millis();
  lastConnectedTime = millis();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    cardPresent = true;
    LOG("📡 Boot disebabkan oleh RFID interrupt");
  }
}

void loop() {
  if (cardPresent) {
    handleRFIDCard();
  }

  if (deviceConnected) {
    lastConnectedTime = millis();
  }

  if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
  }

  if (millis() - lastConnectedTime > DEEP_SLEEP_TIMEOUT) {
    goToDeepSleep();
  }

  if (sleepAfterNotify && millis() - sleepRequestTime >= 200) {
    goToSleep();
    sleepAfterNotify = false;
  }

  if (!deviceConnected && millis() - lastWakeTime > WAKE_DURATION && !cardPresent) {
    goToSleep();
  }

  if (!isAdvertising && !deviceConnected) {
    startAdvertising();
    delay(100);
  }

  delay(10);
}