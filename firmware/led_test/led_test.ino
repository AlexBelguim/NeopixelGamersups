/*
 * Minimal NeoPixel Test - NO BLE
 * Use this to confirm your LED strip works on Pin 13
 */

#include <Adafruit_NeoPixel.h>

#define LED_PIN     13
#define LED_COUNT   7

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
    Serial.begin(115200);
    Serial.println("Minimal NeoPixel Test Starting...");
    
    strip.begin();
    strip.clear();
    strip.show();
    strip.setBrightness(255);
    
    Serial.println("Setting first 7 LEDs to WHITE...");
    for (int i = 0; i < LED_COUNT; i++) {
        strip.setPixelColor(i, strip.Color(255, 255, 255));
    }
    strip.show();
    
    Serial.println("LEDs should be WHITE now!");
    Serial.println("If not, check your wiring and pin number.");
}

void loop() {
    // Blink between white and off every second
    static unsigned long lastBlink = 0;
    static bool isOn = true;
    
    if (millis() - lastBlink > 1000) {
        lastBlink = millis();
        isOn = !isOn;
        
        if (isOn) {
            for (int i = 0; i < LED_COUNT; i++) {
                strip.setPixelColor(i, strip.Color(255, 0, 255)); // Magenta
            }
            Serial.println("LEDs: MAGENTA");
        } else {
            strip.clear();
            Serial.println("LEDs: OFF");
        }
        strip.show();
    }
}
