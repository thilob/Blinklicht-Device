#pragma once

/// ============================================================================
/// Board-spezifische Pin-Konfigurationen
/// Wird via build_flags in platformio.ini definiert
/// ============================================================================

// -------------------- ESP32 DevKit (Standard 30-Pin) --------------------
#ifdef BOARD_ESP32_DEVKIT
  #define BOARD_NAME "ESP32 DevKit"
  #define NUM_LED_OUTPUTS 10
  
  static const int LED_PINS[NUM_LED_OUTPUTS] = {
    16, 17, 18, 19, 21, 22, 23, 25, 26, 27
  };
  
  static const int RESET_WIFI_PIN = 34;  // Input only
  
// -------------------- ESP32-C3 Super Mini (13 GPIO) --------------------
#elif defined(BOARD_ESP32C3_MINI)
  #define BOARD_NAME "ESP32-C3 Super Mini"
  #define NUM_LED_OUTPUTS 8  // C3 hat weniger GPIOs
  
  static const int LED_PINS[NUM_LED_OUTPUTS] = {
    2, 3, 4, 5, 6, 7, 8, 9  // GPIO 0,1 für USB; 18,19 für andere
  };
  
  static const int RESET_WIFI_PIN = 10;

// -------------------- ESP32-C3 WiFiduino (21 GPIO) --------------------
#elif defined(BOARD_WIFIDUINO32C3)
  #define BOARD_NAME "WiFiduino32 C3"
  #define NUM_LED_OUTPUTS 10
  
  static const int LED_PINS[NUM_LED_OUTPUTS] = {
    2, 3, 4, 5, 6, 7, 8, 9, 10, 18
  };
  
  static const int RESET_WIFI_PIN = 19;

// -------------------- ESP32-S2 Mini --------------------
#elif defined(BOARD_ESP32S2_MINI)
  #define BOARD_NAME "ESP32-S2 Mini"
  #define NUM_LED_OUTPUTS 12
  
  static const int LED_PINS[NUM_LED_OUTPUTS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
  };
  
  static const int RESET_WIFI_PIN = 0;

// -------------------- ESP32-S3 DevKit --------------------
#elif defined(BOARD_ESP32S3_DEVKIT)
  #define BOARD_NAME "ESP32-S3 DevKit"
  #define NUM_LED_OUTPUTS 16  // S3 hat viele GPIOs
  
  static const int LED_PINS[NUM_LED_OUTPUTS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
  };
  
  static const int RESET_WIFI_PIN = 0;

// -------------------- Custom Board (Beispiel für eigene Definition) --------------------
#elif defined(BOARD_CUSTOM)
  #define BOARD_NAME "Custom Board"
  #define NUM_LED_OUTPUTS 6
  
  static const int LED_PINS[NUM_LED_OUTPUTS] = {
    13, 14, 15, 16, 17, 18  // Deine eigenen Pins
  };
  
  static const int RESET_WIFI_PIN = 12;

// -------------------- Fallback: Standard ESP32 --------------------
#else
  #warning "Kein Board definiert, verwende ESP32 DevKit als Standard"
  #define BOARD_NAME "ESP32 DevKit (Default)"
  #define NUM_LED_OUTPUTS 10
  
  static const int LED_PINS[NUM_LED_OUTPUTS] = {
    16, 17, 18, 19, 21, 22, 23, 25, 26, 27
  };
  
  static const int RESET_WIFI_PIN = 34;
#endif

// -------------------- Validierung --------------------
#if NUM_LED_OUTPUTS > 16
  #error "NUM_LED_OUTPUTS darf nicht größer als 16 sein (LEDC Limit)"
#endif

// -------------------- Gemeinsame Konstanten --------------------
constexpr uint8_t MAX_LEDC_CHANNELS = 16;  // Hardware-Limit
constexpr uint32_t LEDC_FREQ_HZ = 5000;
constexpr uint8_t LEDC_RES_BITS = 8;
