#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_sleep.h"

// RFID Pins
#define SS_PIN 5
#define RST_PIN 22
#define INT_PIN 4

MFRC522DriverPinSimple ss_pin(SS_PIN);
MFRC522DriverSPI driver{ss_pin};
MFRC522 mfrc522{driver};

bool cardPresent = false;
#define LED_CONNECTED_PIN 2

// BLE setup
BLECharacteristic* pNotifyCharacteristic;
bool deviceConnected = false;
#define SERVICE_UUID           "48aac3ec-923e-4844-ab3d-7dded9a4acc6"
#define CHARACTERISTIC_NOTIFY  "370e2728-ad13-4006-942f-275dcb00c580"
#define CHARACTERISTIC_WRITE   "62f4f090-82db-4409-9b0c-3ced7217d627"

unsigned long lastInterruptTime = 0;
const long debounceDelay = 200;  // debounce 200ms

// Callback untuk passkey dan autentikasi
class MySecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() {
    return 123456;  // Passkey statis untuk pairing
  }

  void onPassKeyNotify(uint32_t pass_key) {
    Serial.printf("Passkey yang diterima: %06u\n", pass_key);
  }

  bool onConfirmPIN(uint32_t pass_key) {
    Serial.printf("Menyetujui PIN: %06u\n", pass_key);
    return true;  // Mengkonfirmasi PIN secara otomatis
  }

  bool onSecurityRequest() {
    return true;  // Mengizinkan permintaan keamanan
  }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) {
    if (auth_cmpl.success) {
      Serial.println("✅ Autentikasi Berhasil");
    } else {
      Serial.println("❌ Autentikasi Gagal");
    }
  }
};

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    digitalWrite(LED_CONNECTED_PIN, HIGH);
    Serial.println("✅ BLE device connected");
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    digitalWrite(LED_CONNECTED_PIN, LOW);
    Serial.println("❌ BLE device disconnected");
    pServer->startAdvertising();
  }
};

void IRAM_ATTR rfidInterrupt() {
    unsigned long interruptTime = millis();
    if (interruptTime - lastInterruptTime > debounceDelay) {
        cardPresent = true;
        lastInterruptTime = interruptTime;
    }
}


String getUniqueBleName(const String& baseName) {
  uint64_t chipid = ESP.getEfuseMac(); // Ambil chip ID (64-bit)
  uint16_t shortID = (uint16_t)(chipid & 0xFFFF); // Ambil 2 byte terakhir
  char uniqueName[32];
  sprintf(uniqueName, "%s-%04X", baseName.c_str(), shortID); // Format: baseName-XXXX
  return String(uniqueName);
}




void setup() {
  Serial.begin(115200);
  pinMode(LED_CONNECTED_PIN, OUTPUT);

  String bleName = getUniqueBleName("AGRX");
  BLEDevice::init(bleName.c_str());
  Serial.print("📛 BLE Name: ");
  Serial.println(bleName);

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLESecurity *security = new BLESecurity();
  security->setAuthenticationMode(ESP_LE_AUTH_NO_BOND);    // Mode pairing
  security->setKeySize(16);                                  // Ukuran kunci 16 byte
  security->setStaticPIN(123456);



  BLEService *pService = pServer->createService(SERVICE_UUID);

  pNotifyCharacteristic = pService->createCharacteristic(
                            CHARACTERISTIC_NOTIFY,
                            BLECharacteristic::PROPERTY_NOTIFY);
  pNotifyCharacteristic->addDescriptor(new BLE2902());

  pService->start();
  pServer->getAdvertising()->start();
  Serial.println("🚀 BLE ready");

  // RFID
  mfrc522.PCD_Init();
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
  Serial.println("🔍 RFID ready. Tap your card...");

  // Konfigurasi pin INT untuk interrupt
  pinMode(INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), rfidInterrupt, FALLING);

  // Konfigurasi wakeup pin
  esp_sleep_enable_ext0_wakeup((gpio_num_t)INT_PIN, 0); // Wake on LOW
}

void loop() {
  if (!cardPresent && !deviceConnected) {
    Serial.println("😴 Tidur...");
    delay(100); // delay untuk BLE tetap stabil
    esp_light_sleep_start(); // masuk sleep
  }

  cardPresent = false;

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  // Ambil UID
  String uidStr = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uidStr += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidStr.toUpperCase();

  Serial.print("🎫 Kartu terdeteksi! UID: ");
  Serial.println(uidStr);

  if (deviceConnected) {
    pNotifyCharacteristic->setValue(uidStr.c_str());
    pNotifyCharacteristic->notify();
    Serial.println("📤 UID dikirim via BLE");
  }

  mfrc522.PICC_HaltA(); // Stop komunikasi dengan kartu
  delay(1000); // Debounce kartu
}
