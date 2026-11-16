#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // Set Wi-Fi to station mode
  WiFi.mode(WIFI_STA);
  
  Serial.print("ESP32-S3 MAC Address: ");
  Serial.println(WiFi.macAddress());
}

void loop() {
  // Nothing here
}
