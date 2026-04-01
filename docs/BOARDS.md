# Board-Konfigurationssystem

## Übersicht

Dieses Projekt unterstützt verschiedene ESP32-Boards mit unterschiedlichen Pin-Konfigurationen. Die Board-Konfiguration erfolgt automatisch basierend auf dem gewählten PlatformIO Environment. Die maximal gleichzeitig nutzbaren PWM-Ausgaenge richten sich nach den LEDC-Kanaelen der jeweiligen ESP32-Zielhardware.

## Unterstützte Boards

### ESP32 DevKit (Standard)
- **Environment:** `esp32dev`
- **Alias:** `devkit-v1`
- **Vorkonfigurierte Ausgänge:** 10 LEDs
- **Max. gleichzeitige PWM-Kanaele:** 16
- **Pins:** GPIO 16, 17, 18, 19, 21, 22, 23, 25, 26, 27
- **Reset Pin:** GPIO 34

### ESP32-C3 DevKit M1
- **Environment:** `esp32-c3-devkitm-1`
- **Vorkonfigurierte Ausgänge:** 8 LEDs
- **Max. gleichzeitige PWM-Kanaele:** 8
- **Pins:** GPIO 2, 3, 4, 5, 6, 7, 8, 9
- **Reset Pin:** GPIO 10
- **Hinweis:** GPIO 0,1 für USB reserviert

### ESP32-C3 Super Mini
- **Environment:** `esp32c3-super-mini`
- **Vorkonfigurierte Ausgänge:** 8 LEDs
- **Max. gleichzeitige PWM-Kanaele:** 8
- **Pins:** GPIO 2, 3, 4, 5, 6, 7, 8, 9
- **Reset Pin:** GPIO 10
- **Hinweis:** Sehr kompaktes Board

### WiFiduino32 C3
- **Environment:** `wifiduino32c3`
- **Vorkonfigurierte Ausgänge:** 8 LEDs
- **Hinweis:** 10 Pins sind hinterlegt, aber der ESP32-C3 stellt gleichzeitig nur 8 LEDC-PWM-Kanaele bereit.
- **Pins:** GPIO 2, 3, 4, 5, 6, 7, 8, 9, 10, 18
- **Reset Pin:** GPIO 19

### ESP32-S2 Mini
- **Environment:** `esp32s2-mini`
- **Vorkonfigurierte Ausgänge:** 8 LEDs
- **Hinweis:** 12 Pins sind hinterlegt, gleichzeitig nutzbar sind aber 8 LEDC-PWM-Kanaele.
- **Pins:** GPIO 1-12
- **Reset Pin:** GPIO 0

### ESP32-S3 DevKit
- **Environment:** `esp32s3-devkit`
- **Vorkonfigurierte Ausgänge:** 8 LEDs
- **Hinweis:** 16 Pins sind hinterlegt, gleichzeitig nutzbar sind aber 8 LEDC-PWM-Kanaele.
- **Pins:** GPIO 1-16
- **Reset Pin:** GPIO 0
- **Hinweis:** Mehr GPIOs verfügbar

## Verwendung

### 1. Board auswählen in VS Code

In VS Code unten in der Statusleiste auf das PlatformIO-Symbol klicken und das gewünschte Environment auswählen, z.B. `esp32dev` oder `devkit-v1`.

### 2. Mit PlatformIO CLI

```bash
# ESP32 DevKit kompilieren
pio run -e esp32dev

# ESP32 DevKit V1 (ESP32-WROOM) kompilieren
pio run -e devkit-v1

# ESP32-C3 kompilieren und uploaden
pio run -e esp32-c3-devkitm-1 -t upload

# ESP32-S3 kompilieren und Monitor starten
pio run -e esp32s3-devkit -t upload -t monitor
```

### 3. Board-Info beim Start

Beim Booten zeigt der Serial Monitor die aktuelle Board-Konfiguration:

```
========================================
Blaulicht-Controller (Multi-Board)
========================================
Board:         ESP32 DevKit
LED Ausgänge:  10
LEDC Kanäle:   16
Reset Pin:     GPIO 34
========================================
```

## Eigenes Board hinzufügen

### Schritt 1: Board-Definition in `board_config.h`

```cpp
#elif defined(BOARD_MEIN_BOARD)
  #define BOARD_NAME "Mein Custom Board"

  static const int LED_PINS[] = {
    13, 14, 15, 16, 17, 18  // Deine Pins
  };
  
  static const int RESET_WIFI_PIN = 12;
```

### Schritt 2: Environment in `platformio.ini`

```ini
[env:mein-board]
platform = espressif32
board = esp32dev  ; oder passende Board-ID
framework = arduino
monitor_speed = 115200
board_build.filesystem = littlefs

lib_deps = ${common.lib_deps}

build_flags =
  ${common.base_flags}
  -DBOARD_MEIN_BOARD  ; <-- Wichtig!

lib_ldf_mode = deep+
```

### Schritt 3: Kompilieren

```bash
pio run -e mein-board -t upload
```

## Pin-Auswahl Tipps

### ESP32 (Original)
- ✅ Sicher: GPIO 2, 4, 5, 12-19, 21-23, 25-27, 32-33
- ⚠️ Input-Only: GPIO 34-39 (nur als Eingänge)
- ❌ Vermeiden: GPIO 0, 1 (Boot/Flash), 6-11 (Flash)

### ESP32-C3
- ✅ Sicher: GPIO 2-10, 18-21
- ❌ Vermeiden: GPIO 0, 1 (USB), 18, 19 wenn USB benötigt

### ESP32-S2
- ✅ Sicher: GPIO 1-14, 15-17, 33-42
- ❌ Vermeiden: GPIO 0 (Boot), 19-20 (USB)

### ESP32-S3
- ✅ Viele GPIOs verfügbar
- ❌ Vermeiden: GPIO 19-20 (USB), 26-32 (PSRAM wenn genutzt)

## Beispiel: Migration von einem Board zum anderen

Wenn du von ESP32 DevKit auf ESP32-C3 wechseln möchtest:

1. **Umstecken der Hardware:** Verbinde deine LEDs mit den Pins aus `board_config.h`
2. **Environment wählen:** `esp32-c3-devkitm-1` in PlatformIO
3. **Upload:** Der Code kompiliert automatisch mit den richtigen Pins!
4. **Config prüfen:** Falls du bereits eine `config.json` hast, prüfe ob die Pins stimmen

## Fehlerbehebung

### "Kein Board definiert" Warning
- Stelle sicher dass in `platformio.ini` der richtige `build_flag` gesetzt ist
- Beispiel: `-DBOARD_ESP32_DEVKIT`

### Falscher Pin wird verwendet
- Prüfe die Pin-Definition in `board_config.h`
- Verwende CLI-Befehl `light list` um aktuelle Pins zu sehen

### Brownout-Resets
- Zu viele LEDs für die Stromversorgung
- Verwende externes Netzteil
- Reduziere Anzahl der Ausgänge in `board_config.h`

## Board-IDs Referenz

| Board | PlatformIO board= | Environment Name |
|-------|------------------|------------------|
| ESP32 DevKit | `esp32dev` | `esp32dev`, `devkit-v1` |
| ESP32-C3 | `esp32-c3-devkitm-1` | `esp32-c3-devkitm-1` |
| ESP32-S2 | `lolin_s2_mini` | `esp32s2-mini` |
| ESP32-S3 | `esp32-s3-devkitc-1` | `esp32s3-devkit` |

Weitere Board-IDs: https://registry.platformio.org/platforms/platformio/espressif32/boards
