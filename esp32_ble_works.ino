#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

bool deviceConnected = false;
BLECharacteristic* pNotifyCharacteristic;

// UUIDs untuk karakteristik
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_NOTIFY  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_WRITE   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
int limit = 50;
// Callback saat menerima data dari client
class WriteCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String value = pCharacteristic->getValue().c_str();
    if (value.length() > 0) {
      Serial.print("📥 Data masuk dari HP: ");
      Serial.println(value);
      pNotifyCharacteristic->setValue(value); 
      pNotifyCharacteristic->notify();
    }
  }
};

// Callback koneksi
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("✅ Perangkat terhubung!");
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("❌ Perangkat terputus!");
    pServer->startAdvertising();
  }
};

// Fungsi kirim data biasa dengan batasan karakter
void sendDataWithLimit(BLECharacteristic* pChar, String data, int limit) {
  if (data.length() > limit) {
    data = data.substring(0, limit);  // Memotong data jika lebih dari batas
  }
  pChar->setValue(data);  // Set nilai data
  pChar->notify();        // Kirim notifikasi
  Serial.print("Kirim data: ");
  Serial.println(data);   // Log data yang dikirim
}

void setup() {
  Serial.begin(115200);
  BLEDevice::init("ESP32-BLE-2Way");

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Notify
  pNotifyCharacteristic = pService->createCharacteristic(
                            CHARACTERISTIC_NOTIFY,
                            BLECharacteristic::PROPERTY_NOTIFY
                          );
  pNotifyCharacteristic->addDescriptor(new BLE2902());

  // Write
  BLECharacteristic *pWriteCharacteristic = pService->createCharacteristic(
                                              CHARACTERISTIC_WRITE,
                                              BLECharacteristic::PROPERTY_WRITE
                                            );
  pWriteCharacteristic->setCallbacks(new WriteCallback());

  pService->start();
  pServer->getAdvertising()->start();

  Serial.println("🚀 BLE Siap: ESP32-BLE-2Way");
}

void loop() {
  if (deviceConnected) {
    String data = "Halo dari ESP32 ke HP!;Halo dari ESP32 ke HP!";  // Data untuk dikirim
    sendDataWithLimit(pNotifyCharacteristic, data, limit);  // Kirim data dengan batasan
    delay(1000);  // Jeda kecil antara pengiriman data
  }
}
