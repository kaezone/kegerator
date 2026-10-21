#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <HX711_ADC.h>

WiFiMulti wifiMulti;
HX711_ADC LoadCell(3,4);   //HX711 constructor (dout pin, sck pin)
long scaleTick, httpTick;

void setup() 
{
  Serial.begin(115200);

  Serial.println();
  Serial.println();
  Serial.println();

  // Give background processes a few seconds to complete
  for(uint8_t t = 4; t > 0; t--) 
  {
    Serial.printf("[SETUP] WAIT %d...\n", t);
    Serial.flush();
    delay(1000);
  }

  wifiMulti.addAP("ssid", "pass");

  // Set up the HX711 library
  LoadCell.begin();
  LoadCell.start(10); // Start up the HX711 library with 10ms delay
  LoadCell.setCalFactor(5910); // user set calibration factor (float)
  Serial.println("Startup + tare is complete");
}
