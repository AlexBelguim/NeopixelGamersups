# GamerSup Controller

A Progressive Web App (PWA) and ESP32 BLE firmware for controlling RGB cup lights.

## 🎮 Features

- **Web Bluetooth Control** - Connect to ESP32 controllers from any browser
- **Cup Light Management** - Control individual cups with 7 NeoPixel LEDs each
- **Image Upload** - Upload images and automatically extract colors for LEDs
- **Effects Engine** - Spotlight, Wave, Fade, Rainbow, Pulse, Strobe
- **Timer System** - Auto-off timer that runs independently on ESP32
- **Offline Support** - PWA works offline, effects continue when app disconnects
- **GitHub Pages Hosting** - No server required, host for free

## 📁 Project Structure

```
neopixel/
├── sketch_may15a.ino     # Original WiFi-based code (unchanged)
├── pwa/                  # Progressive Web App
│   ├── index.html        # Main app
│   ├── styles.css        # Dark theme with glassmorphism
│   ├── app.js            # Web Bluetooth integration
│   ├── manifest.json     # PWA manifest
│   ├── sw.js             # Service worker
│   └── icons/            # App icons
└── firmware/             # ESP32 BLE firmware
    └── gamersup_ble.ino  # BLE-based controller
```

## 🚀 Getting Started

### PWA (Web App)

1. **Local Testing:**
   ```bash
   cd pwa
   npx serve .
   ```
   Open `http://localhost:3000` in Chrome

2. **GitHub Pages Deployment:**
   - Push the `pwa/` folder to GitHub
   - Enable Pages in Settings → Pages
   - Select source folder

### ESP32 Firmware

1. **Requirements:**
   - Arduino IDE with ESP32 board package
   - Libraries: `Adafruit_NeoPixel`, `ESP32 BLE Arduino`

2. **Upload:**
   - Open `firmware/gamersup_ble.ino` in Arduino IDE
   - Select your ESP32 board
   - Configure `LED_PIN` and `LED_COUNT`
   - Upload

## 📡 BLE Protocol

| Command | Code | Format |
|---------|------|--------|
| Set Cup Color | `0x01` | `[cmd, cupId, r, g, b]` |
| All On | `0x03` | `[cmd]` |
| All Off | `0x04` | `[cmd]` |
| Start Effect | `0x05` | `[cmd, type, speedHi, speedLo]` |
| Stop Effect | `0x06` | `[cmd]` |
| Set Timer | `0x07` | `[cmd, secondsHi, secondsLo]` |
| Upload Image | `0x08` | `[cmd, cupId, r0,g0,b0, ... r6,g6,b6]` |

## 🎨 Effects

- **Spotlight** - Cycle through cups one at a time
- **Wave** - Brightness wave across cups
- **Fade** - All cups fade in/out together
- **Rainbow** - Color wheel cycling
- **Pulse** - Heartbeat pulsing effect
- **Strobe** - Flash effect

## 📱 Browser Support

Web Bluetooth requires:
- Chrome (Android, Windows, macOS, Linux)
- Edge (Windows)
- Opera

Note: Safari and Firefox do not support Web Bluetooth.

## 📄 License

MIT
