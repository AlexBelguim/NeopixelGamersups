/*
 * GamerSup Controller - ESP32 BLE Firmware
 * 
 * Features:
 * - BLE GATT server for PWA communication
 * - NeoPixel driver for cup lights (7 LEDs per cup)
 * - Independent effect engine (continues when app disconnects)
 * - Independent timer system
 * - Persistent settings storage
 * - Image-to-LED color mapping
 * 
 * Hardware:
 * - ESP32 (any variant with BLE)
 * - NeoPixel LED strip connected to LED_PIN
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
// FastLED with RMT - no I2S (I2S causes crashes)
#include <FastLED.h>
#include <Preferences.h>
#include <SPIFFS.h>

// ========================================
// Configuration
// ========================================
#define LED_PIN         13
#define LED_COUNT       1400    // Total LEDs (multiple of 7)
#define LEDS_PER_CUP    7
#define MAX_CUPS        (LED_COUNT / LEDS_PER_CUP)

// BLE UUIDs
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHAR_COMMAND_UUID   "12345678-1234-1234-1234-123456789001"
#define CHAR_STATE_UUID     "12345678-1234-1234-1234-123456789002"
#define CHAR_IMAGE_UUID     "12345678-1234-1234-1234-123456789003"
#define CHAR_CONFIG_UUID    "12345678-1234-1234-1234-123456789004"

// Command codes (must match PWA)
#define CMD_SET_COLOR       0x01
#define CMD_SET_ALL         0x02
#define CMD_ALL_ON          0x03
#define CMD_ALL_OFF         0x04
#define CMD_SET_EFFECT      0x05
#define CMD_STOP_EFFECT     0x06
#define CMD_SET_TIMER       0x07
#define CMD_UPLOAD_IMG      0x08
#define CMD_GET_STATE       0x09
#define CMD_SET_CUP_COUNT   0x0A
#define CMD_SAVE_SETTINGS   0x0B

// State notification types
#define STATE_TIMER         0x01
#define STATE_EFFECT        0x02
#define STATE_CUP           0x03
#define STATE_FULL          0x04

// ========================================
// Globals
// ========================================
CRGB leds[LED_COUNT];
Preferences prefs;

// BLE
BLEServer* pServer = nullptr;
BLECharacteristic* pCommandChar = nullptr;
BLECharacteristic* pStateChar = nullptr;
BLECharacteristic* pImageChar = nullptr;
BLECharacteristic* pConfigChar = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Cup state
struct Cup {
    uint8_t r, g, b;
    uint8_t leds[7][3];  // Individual LED colors
    bool on;
    bool hasImage;
};

Cup cups[MAX_CUPS];
uint8_t numCups = 3;

// Effects
bool effectActive = false;
uint8_t currentEffect = 0;
uint16_t effectSpeed = 1000;
unsigned long effectNextStep = 0;
int effectStep = 0;

// Timer
unsigned long autoOffTime = 0;
int lastTimerSent = -1;

// Forward declarations
void handleCommand(uint8_t* data, size_t len);
void handleImageUpload(uint8_t* data, size_t len);
void updateCupLeds(uint8_t cupId);
void updateAllLeds();
void notifyCupState(uint8_t cupId);
void notifyFullState();
void notifyEffectState();
void notifyTimer(uint16_t seconds);
void saveSettings();
void saveCupImage(uint8_t cupId);

// ========================================
// BLE Callbacks
// ========================================
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("Client connected");
    }

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("Client disconnected");
        // Note: effects and timer continue running!
    }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        String value = pCharacteristic->getValue();
        if (value.length() > 0) {
            handleCommand((uint8_t*)value.c_str(), value.length());
        }
    }
};

class ImageCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        String value = pCharacteristic->getValue();
        if (value.length() > 0) {
            handleImageUpload((uint8_t*)value.c_str(), value.length());
        }
    }
};

// ========================================
// Command Handler
// ========================================
void handleCommand(uint8_t* data, size_t len) {
    if (len < 1) return;
    
    uint8_t cmd = data[0];
    Serial.printf("CMD: 0x%02X, len: %d\n", cmd, len);
    
    switch (cmd) {
        case CMD_SET_COLOR:
            if (len >= 5) {
                uint8_t cupId = data[1];
                if (cupId < numCups) {
                    cups[cupId].r = data[2];
                    cups[cupId].g = data[3];
                    cups[cupId].b = data[4];
                    cups[cupId].on = (data[2] > 0 || data[3] > 0 || data[4] > 0);
                    
                    // Apply solid color to all LEDs in cup
                    for (int i = 0; i < LEDS_PER_CUP; i++) {
                        cups[cupId].leds[i][0] = data[2];
                        cups[cupId].leds[i][1] = data[3];
                        cups[cupId].leds[i][2] = data[4];
                    }
                    cups[cupId].hasImage = false;
                    
                    if (!effectActive) {
                        updateCupLeds(cupId);
                    }
                    notifyCupState(cupId);
                }
            }
            break;
            
        case CMD_SET_ALL:
            if (len >= 4) {
                for (int c = 0; c < numCups; c++) {
                    cups[c].r = data[1];
                    cups[c].g = data[2];
                    cups[c].b = data[3];
                    cups[c].on = (data[1] > 0 || data[2] > 0 || data[3] > 0);
                    for (int i = 0; i < LEDS_PER_CUP; i++) {
                        cups[c].leds[i][0] = data[1];
                        cups[c].leds[i][1] = data[2];
                        cups[c].leds[i][2] = data[3];
                    }
                }
                if (!effectActive) updateAllLeds();
            }
            break;
            
        case CMD_ALL_ON:
            for (int c = 0; c < numCups; c++) {
                cups[c].on = true;
                // If color is black, set to default white
                if (cups[c].r == 0 && cups[c].g == 0 && cups[c].b == 0) {
                    cups[c].r = 255;
                    cups[c].g = 255;
                    cups[c].b = 255;
                }
                // Always sync leds array with cup color
                for (int i = 0; i < LEDS_PER_CUP; i++) {
                    cups[c].leds[i][0] = cups[c].r;
                    cups[c].leds[i][1] = cups[c].g;
                    cups[c].leds[i][2] = cups[c].b;
                }
                Serial.printf("Cup %d: on=%d, r=%d g=%d b=%d, leds[0]=%d,%d,%d\n", 
                    c, cups[c].on, cups[c].r, cups[c].g, cups[c].b,
                    cups[c].leds[0][0], cups[c].leds[0][1], cups[c].leds[0][2]);
            }
            effectActive = false;
            autoOffTime = 0;
            Serial.printf("Calling updateAllLeds, numCups=%d\n", numCups);
            updateAllLeds();
            notifyFullState();
            Serial.println("ALL_ON complete");
            break;
            
        case CMD_ALL_OFF:
            for (int c = 0; c < numCups; c++) {
                cups[c].on = false;
            }
            effectActive = false;
            autoOffTime = 0;
            fill_solid(leds, LED_COUNT, CRGB::Black);
            FastLED.show();
            notifyFullState();
            break;
            
        case CMD_SET_EFFECT:
            if (len >= 4) {
                currentEffect = data[1];
                effectSpeed = (data[2] << 8) | data[3];
                effectActive = true;
                effectStep = 0;
                effectNextStep = millis();
                Serial.printf("Effect %d started, speed %d\n", currentEffect, effectSpeed);
                notifyEffectState();
            }
            break;
            
        case CMD_STOP_EFFECT:
            effectActive = false;
            updateAllLeds();  // Restore cup colors
            notifyEffectState();
            break;
            
        case CMD_SET_TIMER:
            if (len >= 3) {
                uint16_t seconds = (data[1] << 8) | data[2];
                if (seconds > 0) {
                    autoOffTime = millis() + (seconds * 1000UL);
                } else {
                    autoOffTime = 0;
                }
                lastTimerSent = -1;
                Serial.printf("Timer set: %d seconds\n", seconds);
            }
            break;
            
        case CMD_GET_STATE:
            notifyFullState();
            break;
            
        case CMD_SET_CUP_COUNT:
            if (len >= 2) {
                numCups = constrain(data[1], 1, MAX_CUPS);
                saveSettings();
                Serial.printf("Cup count set: %d\n", numCups);
            }
            break;
            
        case CMD_SAVE_SETTINGS:
            saveSettings();
            break;
    }
}

void handleImageUpload(uint8_t* data, size_t len) {
    // Format: [CMD_UPLOAD_IMG, cupId, r0,g0,b0, r1,g1,b1, ... r6,g6,b6]
    if (len < 23) return;  // 1 + 1 + (7 * 3) = 23 bytes minimum
    
    uint8_t cupId = data[1];
    if (cupId >= numCups) return;
    
    cups[cupId].hasImage = true;
    cups[cupId].on = true;
    
    for (int i = 0; i < LEDS_PER_CUP; i++) {
        int offset = 2 + (i * 3);
        cups[cupId].leds[i][0] = data[offset];
        cups[cupId].leds[i][1] = data[offset + 1];
        cups[cupId].leds[i][2] = data[offset + 2];
    }
    
    // Use first LED color as main cup color
    cups[cupId].r = cups[cupId].leds[0][0];
    cups[cupId].g = cups[cupId].leds[0][1];
    cups[cupId].b = cups[cupId].leds[0][2];
    
    if (!effectActive) {
        updateCupLeds(cupId);
    }
    
    saveCupImage(cupId);
    Serial.printf("Image uploaded for cup %d\n", cupId);
}

// ========================================
// LED Control
// ========================================
void updateCupLeds(uint8_t cupId) {
    if (cupId >= numCups) return;
    
    int startLed = cupId * LEDS_PER_CUP;
    
    for (int i = 0; i < LEDS_PER_CUP; i++) {
        if (cups[cupId].on) {
            leds[startLed + i] = CRGB(cups[cupId].leds[i][0], 
                                      cups[cupId].leds[i][1], 
                                      cups[cupId].leds[i][2]);
        } else {
            leds[startLed + i] = CRGB::Black;
        }
    }
    FastLED.show();
}

void updateAllLeds() {
    Serial.printf("updateAllLeds: numCups=%d\n", numCups);
    for (int c = 0; c < numCups; c++) {
        int startLed = c * LEDS_PER_CUP;
        Serial.printf("  Cup %d: on=%d, startLed=%d, color=%d,%d,%d\n", 
            c, cups[c].on, startLed, cups[c].leds[0][0], cups[c].leds[0][1], cups[c].leds[0][2]);
        for (int i = 0; i < LEDS_PER_CUP; i++) {
            if (cups[c].on) {
                leds[startLed + i] = CRGB(cups[c].leds[i][0],
                                          cups[c].leds[i][1],
                                          cups[c].leds[i][2]);
            } else {
                leds[startLed + i] = CRGB::Black;
            }
        }
    }
    FastLED.show();
    Serial.println("updateAllLeds: FastLED.show() called");
}

// ========================================
// Effects Engine (runs independently!)
// ========================================
void runEffect() {
    unsigned long now = millis();
    if (now < effectNextStep) return;
    
    effectNextStep = now + (effectSpeed / 20);  // Smooth animation
    
    switch (currentEffect) {
        case 0: // Spotlight - one cup at a time
            runSpotlight();
            break;
        case 1: // Wave - brightness wave
            runWave();
            break;
        case 2: // Fade - all fade in/out
            runFade();
            break;
        case 3: // Rainbow - color cycling
            runRainbow();
            break;
        case 4: // Pulse - heartbeat effect
            runPulse();
            break;
        case 5: // Strobe - flash effect
            runStrobe();
            break;
    }
    
    effectStep++;
}

void runSpotlight() {
    static int currentCup = 0;
    static unsigned long lastSwitch = 0;
    
    if (millis() - lastSwitch > effectSpeed) {
        currentCup = (currentCup + 1) % numCups;
        lastSwitch = millis();
    }
    
    fill_solid(leds, LED_COUNT, CRGB::Black);
    if (cups[currentCup].on) {
        int startLed = currentCup * LEDS_PER_CUP;
        for (int i = 0; i < LEDS_PER_CUP; i++) {
            leds[startLed + i] = CRGB(cups[currentCup].leds[i][0],
                                      cups[currentCup].leds[i][1],
                                      cups[currentCup].leds[i][2]);
        }
    }
    FastLED.show();
}

void runWave() {
    for (int c = 0; c < numCups; c++) {
        if (!cups[c].on) continue;
        
        float phase = (effectStep + c * 10) % 100;
        float brightness = (sin(phase * 0.0628) + 1.0) * 0.5;
        
        int startLed = c * LEDS_PER_CUP;
        for (int i = 0; i < LEDS_PER_CUP; i++) {
            leds[startLed + i] = CRGB(cups[c].leds[i][0] * brightness,
                                      cups[c].leds[i][1] * brightness,
                                      cups[c].leds[i][2] * brightness);
        }
    }
    FastLED.show();
}

void runFade() {
    float brightness = (sin(effectStep * 0.05) + 1.0) * 0.5;
    
    for (int c = 0; c < numCups; c++) {
        if (!cups[c].on) continue;
        
        int startLed = c * LEDS_PER_CUP;
        for (int i = 0; i < LEDS_PER_CUP; i++) {
            leds[startLed + i] = CRGB(cups[c].leds[i][0] * brightness,
                                      cups[c].leds[i][1] * brightness,
                                      cups[c].leds[i][2] * brightness);
        }
    }
    FastLED.show();
}

CRGB wheelColor(byte pos) {
    pos = 255 - pos;
    if (pos < 85) {
        return CRGB(255 - pos * 3, 0, pos * 3);
    }
    if (pos < 170) {
        pos -= 85;
        return CRGB(0, pos * 3, 255 - pos * 3);
    }
    pos -= 170;
    return CRGB(pos * 3, 255 - pos * 3, 0);
}

void runRainbow() {
    for (int c = 0; c < numCups; c++) {
        if (!cups[c].on) continue;
        
        int startLed = c * LEDS_PER_CUP;
        CRGB color = wheelColor((effectStep + c * 20) & 255);
        
        for (int i = 0; i < LEDS_PER_CUP; i++) {
            leds[startLed + i] = color;
        }
    }
    FastLED.show();
}

void runPulse() {
    float phase = (effectStep % 100) / 100.0;
    float brightness;
    
    if (phase < 0.15) {
        brightness = phase / 0.15;
    } else if (phase < 0.3) {
        brightness = 1.0 - ((phase - 0.15) / 0.15) * 0.5;
    } else if (phase < 0.45) {
        brightness = 0.5 + ((phase - 0.3) / 0.15) * 0.5;
    } else {
        brightness = 1.0 - ((phase - 0.45) / 0.55);
    }
    
    for (int c = 0; c < numCups; c++) {
        if (!cups[c].on) continue;
        
        int startLed = c * LEDS_PER_CUP;
        for (int i = 0; i < LEDS_PER_CUP; i++) {
            leds[startLed + i] = CRGB(cups[c].leds[i][0] * brightness,
                                      cups[c].leds[i][1] * brightness,
                                      cups[c].leds[i][2] * brightness);
        }
    }
    FastLED.show();
}

void runStrobe() {
    bool on = (effectStep % 10) < 3;
    
    if (on) {
        for (int c = 0; c < numCups; c++) {
            if (!cups[c].on) continue;
            int startLed = c * LEDS_PER_CUP;
            for (int i = 0; i < LEDS_PER_CUP; i++) {
                leds[startLed + i] = CRGB(cups[c].leds[i][0],
                                          cups[c].leds[i][1],
                                          cups[c].leds[i][2]);
            }
        }
    } else {
        fill_solid(leds, LED_COUNT, CRGB::Black);
    }
    FastLED.show();
}

// ========================================
// Timer Engine (runs independently!)
// ========================================
void runTimer() {
    if (autoOffTime == 0) return;
    
    unsigned long now = millis();
    
    if (now >= autoOffTime) {
        // Timer expired - turn everything off
        for (int c = 0; c < numCups; c++) {
            cups[c].on = false;
        }
        effectActive = false;
        fill_solid(leds, LED_COUNT, CRGB::Black);
        FastLED.show();
        autoOffTime = 0;
        
        // Notify app
        notifyTimer(0);
        Serial.println("Timer expired - all off");
    } else {
        // Send remaining time (throttled)
        int remaining = (autoOffTime - now) / 1000;
        if (remaining != lastTimerSent) {
            lastTimerSent = remaining;
            notifyTimer(remaining);
        }
    }
}

// ========================================
// BLE Notifications
// ========================================
void notifyTimer(uint16_t seconds) {
    if (!deviceConnected || !pStateChar) return;
    
    uint8_t data[3] = { STATE_TIMER, (uint8_t)(seconds >> 8), (uint8_t)(seconds & 0xFF) };
    pStateChar->setValue(data, 3);
    pStateChar->notify();
}

void notifyEffectState() {
    if (!deviceConnected || !pStateChar) return;
    
    uint8_t data[2] = { STATE_EFFECT, effectActive ? currentEffect : 0xFF };
    pStateChar->setValue(data, 2);
    pStateChar->notify();
}

void notifyCupState(uint8_t cupId) {
    if (!deviceConnected || !pStateChar) return;
    
    uint8_t data[3] = { STATE_CUP, cupId, cups[cupId].on ? 1 : 0 };
    pStateChar->setValue(data, 3);
    pStateChar->notify();
}

void notifyFullState() {
    if (!deviceConnected || !pStateChar) return;
    
    // Build state packet: [TYPE, numCups, cup0_on, r, g, b, cup1_on, r, g, b, ...]
    size_t len = 2 + (numCups * 4);
    uint8_t* data = new uint8_t[len];
    
    data[0] = STATE_FULL;
    data[1] = numCups;
    
    for (int c = 0; c < numCups; c++) {
        int offset = 2 + (c * 4);
        data[offset] = cups[c].on ? 1 : 0;
        data[offset + 1] = cups[c].r;
        data[offset + 2] = cups[c].g;
        data[offset + 3] = cups[c].b;
    }
    
    pStateChar->setValue(data, len);
    pStateChar->notify();
    
    delete[] data;
}

// ========================================
// Persistence
// ========================================
void saveSettings() {
    prefs.begin("gamersup", false);
    prefs.putUChar("numCups", numCups);
    
    for (int c = 0; c < numCups; c++) {
        String key = "cup" + String(c);
        uint8_t cupData[5] = { cups[c].r, cups[c].g, cups[c].b, cups[c].on ? 1 : 0, cups[c].hasImage ? 1 : 0 };
        prefs.putBytes(key.c_str(), cupData, 5);
    }
    
    prefs.end();
    Serial.println("Settings saved");
}

void loadSettings() {
    prefs.begin("gamersup", true);
    numCups = prefs.getUChar("numCups", 3);
    
    for (int c = 0; c < MAX_CUPS; c++) {
        String key = "cup" + String(c);
        uint8_t cupData[5] = { 255, 0, 255, 0, 0 };  // Default: magenta, off
        prefs.getBytes(key.c_str(), cupData, 5);
        
        cups[c].r = cupData[0];
        cups[c].g = cupData[1];
        cups[c].b = cupData[2];
        cups[c].on = cupData[3] == 1;
        cups[c].hasImage = cupData[4] == 1;
        
        // Initialize all LEDs to cup color
        for (int i = 0; i < LEDS_PER_CUP; i++) {
            cups[c].leds[i][0] = cups[c].r;
            cups[c].leds[i][1] = cups[c].g;
            cups[c].leds[i][2] = cups[c].b;
        }
    }
    
    prefs.end();
    
    // Load cup images from SPIFFS
    loadCupImages();
    
    Serial.printf("Settings loaded: %d cups\n", numCups);
}

void saveCupImage(uint8_t cupId) {
    if (!SPIFFS.begin(true)) return;
    
    String filename = "/cup" + String(cupId) + ".dat";
    File file = SPIFFS.open(filename, "w");
    if (file) {
        for (int i = 0; i < LEDS_PER_CUP; i++) {
            file.write(cups[cupId].leds[i], 3);
        }
        file.close();
    }
}

void loadCupImages() {
    if (!SPIFFS.begin(true)) return;
    
    for (int c = 0; c < MAX_CUPS; c++) {
        if (!cups[c].hasImage) continue;
        
        String filename = "/cup" + String(c) + ".dat";
        File file = SPIFFS.open(filename, "r");
        if (file) {
            for (int i = 0; i < LEDS_PER_CUP && file.available() >= 3; i++) {
                file.read(cups[c].leds[i], 3);
            }
            file.close();
        }
    }
}

// ========================================
// Setup
// ========================================
void setup() {
    Serial.begin(115200);
    Serial.println("GamerSup Controller Starting...");
    
    // Load saved settings FIRST (before NeoPixel, doesn't use strip)
    loadSettings();
    
    // Initialize BLE FIRST (before NeoPixel to avoid RMT conflict)
    Serial.println("Initializing BLE...");
    String deviceName = "GamerSup-";
    deviceName += String((uint16_t)(ESP.getEfuseMac() >> 32), HEX);
    
    BLEDevice::init(deviceName.c_str());
    
    // Disable BLE sleep to prevent RMT conflict with NeoPixel
    esp_bt_sleep_disable();
    
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    
    // Create service
    BLEService* pService = pServer->createService(SERVICE_UUID);
    
    // Command characteristic (write)
    pCommandChar = pService->createCharacteristic(
        CHAR_COMMAND_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pCommandChar->setCallbacks(new CommandCallbacks());
    
    // State characteristic (read + notify)
    pStateChar = pService->createCharacteristic(
        CHAR_STATE_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pStateChar->addDescriptor(new BLE2902());
    
    // Image characteristic (write)
    pImageChar = pService->createCharacteristic(
        CHAR_IMAGE_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pImageChar->setCallbacks(new ImageCallbacks());
    
    // Config characteristic (read + write)
    pConfigChar = pService->createCharacteristic(
        CHAR_CONFIG_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    
    // Start service
    pService->start();
    
    // Start advertising
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    
    Serial.printf("BLE advertising as: %s\n", deviceName.c_str());
    
    // Small delay to let BLE fully stabilize
    delay(100);
    
    // NOW initialize NeoPixels (AFTER BLE is fully set up)
    Serial.println("Initializing NeoPixels with FastLED...");
    FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, LED_COUNT);
    FastLED.setBrightness(255);
    fill_solid(leds, LED_COUNT, CRGB::Black);
    FastLED.show();
    
    // STARTUP LED TEST - Flash first 7 LEDs white to confirm strip works
    Serial.println("LED Startup Test - flashing first 7 LEDs white...");
    for (int i = 0; i < 7; i++) {
        leds[i] = CRGB::White;
    }
    FastLED.show();
    delay(1000);
    fill_solid(leds, LED_COUNT, CRGB::Black);
    FastLED.show();
    Serial.println("LED Startup Test complete");
    
    Serial.println("Ready!");
    
    // Show startup animation
    for (int i = 0; i < numCups; i++) {
        int startLed = i * LEDS_PER_CUP;
        for (int j = 0; j < LEDS_PER_CUP; j++) {
            leds[startLed + j] = CRGB(50, 0, 100);
        }
        FastLED.show();
        delay(50);
    }
    delay(500);
    fill_solid(leds, LED_COUNT, CRGB::Black);
    FastLED.show();
}

// ========================================
// Main Loop
// ========================================
void loop() {
    // Handle BLE reconnection
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        pServer->startAdvertising();
        Serial.println("Restarted advertising");
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }
    
    // Run effect engine (independent of connection!)
    if (effectActive) {
        runEffect();
    }
    
    // Run timer engine (independent of connection!)
    runTimer();
    
    delay(1);
}
