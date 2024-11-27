#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <NTPClient.h>
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>

// Thông tin mạng WiFi
const char* ssid = "___";
const char* password = "____";

// Thông tin Adafruit IO
#define AIO_USERNAME    "___"
#define AIO_KEY         "___"
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883

// Cấu hình OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Cấu hình MAX30105
MAX30105 particleSensor;
#define SDA_PIN 8
#define SCL_PIN 9

uint32_t irBuffer[100];
uint32_t redBuffer[100];
int32_t bufferLength = 100;
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

unsigned long lastDisplayTime = 0;
const unsigned long displayInterval = 5000; // Hiển thị và gửi mỗi 5 giây
WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// Tạo các đối tượng publish cho các feeds
Adafruit_MQTT_Publish temp = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/temp");
Adafruit_MQTT_Publish humi = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/humi");

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);  // Adjust timezone (e.g., GMT+7)

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  // Kết nối WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi"); 
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" WiFi connected");

  // Kết nối đến MQTT
  while (mqtt.connected() == false) {
    Serial.print("Connecting to MQTT...");
    if (mqtt.connect()) {
      Serial.println("MQTT Connected");
    } else {
      Serial.print("Failed to connect to MQTT, status code = ");
      Serial.print(mqtt.connectErrorString(mqtt.connect()));
      delay(5000);
    }
  }

  // Khởi tạo màn hình OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED không tìm thấy."));
    while(1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("Khoi tao OLED...");
  display.display();
  delay(2000);
  display.clearDisplay();

  // Khởi tạo cảm biến MAX30105
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("Không tìm thấy MAX30105."));
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("MAX30105 not found!");
    display.display();
    while (1);
  }

  // Cấu hình cho MAX30105
  byte ledBrightness = 60;
  byte sampleAverage = 4;
  byte ledMode = 3;
  byte sampleRate = 100;
  int pulseWidth = 411;
  int adcRange = 8192;
  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
  display.clearDisplay();
}

void loop() {
  mqtt.processPackets(10000);  // Xử lý MQTT mỗi 10 giây
  mqtt.ping();  // Gửi gói ping để duy trì kết nối MQTT

  // Kiểm tra và gửi dữ liệu nếu đã qua 5 giây
  if (millis() - lastDisplayTime >= displayInterval) {
    lastDisplayTime = millis();

    // Đọc dữ liệu từ cảm biến
    for (byte i = 0; i < bufferLength; i++) {
      while (!particleSensor.available()) {
        particleSensor.check();
      }
      redBuffer[i] = particleSensor.getRed();
      irBuffer[i] = particleSensor.getIR();
      particleSensor.nextSample();
    }

    // Kiểm tra dữ liệu và hiển thị kết quả
    uint32_t irSum = 0;
    for (int i = 0; i < bufferLength; i++) {
      irSum += irBuffer[i];
    }
    long irAverage = irSum / bufferLength;

    // Cập nhật màn hình OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    
    // Hiển thị thời gian
    String currentTime = getCurrentTime();
    display.println(currentTime);

    // Nếu không phát hiện ngón tay
    if (irAverage < 50000) {
      Serial.println("No finger detected.");
      display.println("No finger detected.");
      sendDataAdafruit(0, 0);
    } else {
      maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);

      if (validHeartRate && validSPO2) {
        // Hiển thị dữ liệu nhịp tim và oxy lên OLED
        display.println("Heart Rate: " + String(heartRate) + " bpm");
        display.println("SPO2: " + String(spo2) + " %");
        display.display();

        Serial.print("HR=");
        Serial.print(heartRate);
        Serial.print(", SPO2=");
        Serial.println(spo2);

        // Gửi dữ liệu lên Adafruit IO
        sendData(heartRate, spo2);

        sendDataAdafruit(heartRate, spo2);
      } else {
        display.println("Invalid readings.");
        Serial.println("Invalid readings.");
      }
    }
    display.display();
  }
}

String getCurrentTime() {
    timeClient.update();
    unsigned long epochTime = timeClient.getEpochTime();
    
    int currentHour = (epochTime % 86400L) / 3600; // Hours
    int currentMinute = (epochTime % 3600) / 60;    // Minutes
    int currentSecond = epochTime % 60;             // Seconds

    char formattedTime[20];
    snprintf(formattedTime, sizeof(formattedTime), "2024-11-11 %02d:%02d:%02d", currentHour, currentMinute, currentSecond);

    return String(formattedTime);
}

void sendDataAdafruit(int heart, float oxygen) {
  if (WiFi.status() == WL_CONNECTED) {
    // Chuyển đổi heartRate và oxygen (SPO2) sang uint32_t
    uint32_t heartRateUint32 = static_cast<uint32_t>(heart);
    uint32_t oxygenUint32 = static_cast<uint32_t>(oxygen);

    // Gửi nhịp tim và oxy lên Adafruit IO
    temp.publish(heartRateUint32);    // Gửi nhịp tim dưới dạng uint32_t
    humi.publish(oxygenUint32);       // Gửi nồng độ oxy trong máu dưới dạng uint32_t

    Serial.print("Heart Rate Sent: ");
    Serial.println(heartRateUint32);
    Serial.print("Oxygen Sent: ");
    Serial.println(oxygenUint32);
  } else {
    Serial.println("WiFi not connected.");
  }
}

// Hàm gửi dữ liệu lên server
void sendData(int heart, float oxygen) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("https://smarthomecmn.vercel.app/feed/heart_beat_oxygen");
    http.addHeader("Content-Type", "application/json");

    String currentDateTime = getCurrentTime();

    // Dữ liệu JSON để gửi
    String payload = "{";
    payload += "\"name\": \"Watch\",";
    payload += "\"description\": \"Heart Beat and Oxygen\",";
    payload += "\"heart\": " + String(heart) + ",";
    payload += "\"oxygen\": " + String(oxygen) + ",";
    payload += "\"updated_time\": \"" + currentDateTime + "\"";
    payload += "}";

    // Gửi yêu cầu POST
    int httpResponseCode = http.POST(payload);

    // Kiểm tra phản hồi
    if (httpResponseCode > 0) {
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
    } else {
      Serial.print("Error on sending POST: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("WiFi not connected.");
  }
}