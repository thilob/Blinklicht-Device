
# ESP32 5×LED Web-PWM (PlatformIO)

- 5 LEDs an frei wählbaren PWM-Pins (LEDC), Helligkeit 0..255
- Mehrere editierbare Blinkmuster
- Weboberfläche zum Ein/Aus-Schalten, Muster wählen, Helligkeit setzen
- WLAN: STA oder AP (Fallback AP mit SSID `esp32-leds`, Passwort `12345678`)
- Dateien werden aus LittleFS bedient (`/data/index.html`)

## Pins anpassen

In `src/main.cpp`:
```cpp
static const int LED_PINS[5] = {2, 4, 16, 17, 5};
```

## Build & Flash

1. Öffne den Ordner in VS Code / PlatformIO.
2. Menü: **Build** → Projekt bauen.
3. **Upload** → Firmware flashen.
4. **Upload Filesystem Image** → LittleFS-Inhalte (Web-UI) hochladen.
5. Serielle Konsole (115200 Baud) prüfen für IP-Adresse.

## WLAN

- Beim ersten Start: AP-Modus `esp32-leds` / `12345678` → Weboberfläche öffnen: http://192.168.4.1
- Unter „WLAN-Einstellungen“ **Modus=sta** wählen, SSID/PW eintragen, **Speichern** → Neustart → IP erscheint in der UI.

## Blinkmuster anpassen

In `src/main.cpp` sind Muster definiert:
```cpp
const Step double_flash[] = {
  {70, 255, false},
  {120, 0,  false},
  {70, 255, false},
  {500, 0,  false},
};
```
- `duration_ms`: Schritt-Dauer
- `duty`: Ziel-PWM (0..255)
- `smooth`: `true` → linearer Übergang, `false` → Sprung

Neue Muster einfach als neue `Step[]` definieren und in `PATTERNS[]` eintragen.

## Erweiterungen

- Weitere Endpunkte hinzufügen (Preset speichern, globaler Sync, etc.).
- Captive Portal, mDNS, OTA.
- WebSocket für Push-Updates (WS ist bereits initialisiert, aber hier nicht genutzt).
