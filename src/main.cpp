#include <Arduino.h>

/*
  ESP32 Blaulicht (abwechselnd links/rechts) – nicht-blockierend

  Steuerung:
    - Standard: 2.0 Hz (kompletter Links+Rechts-Zyklus = 500 ms)
    - Serielle Kommandos (115200 8N1):
        ms <zahl>      -> Intervall zwischen Start linke & rechte Seite (50..2000 ms)
        hz <zahl>      -> Blinktakt in Hz (Gesamtzyklus links+rechts), z.B. "hz 2.5"
        on <ms>        -> Pulsbreite (LED an) in Millisekunden (20..400 ms)
        info           -> aktuelle Einstellungen anzeigen
        help           -> Kurzhilfe
    Beispiele:
        ms 250
        hz 3.0
        on 80
*/

constexpr uint8_t PIN_LEFT  = 21;
constexpr uint8_t PIN_RIGHT = 22;

// Zeitparameter (werden zur Laufzeit änderbar)
uint32_t side_interval_ms = 250;  // Startabstand zwischen linker und rechter Seite
uint32_t pulse_ms         = 80;   // Einschaltzeit pro Blitz

// Grenzen
constexpr uint32_t SIDE_MIN_MS = 50;
constexpr uint32_t SIDE_MAX_MS = 2000;
constexpr uint32_t PULSE_MIN_MS = 20;
constexpr uint32_t PULSE_MAX_MS = 400;

// Zustandsmaschine
enum class State : uint8_t {
  LEFT_ON,
  LEFT_OFF_GAP,
  RIGHT_ON,
  RIGHT_OFF_GAP
};

State     state         = State::LEFT_ON;
uint32_t  stateStarted  = 0;

void setPins(bool left, bool right) {
  digitalWrite(PIN_LEFT,  left  ? HIGH : LOW);
  digitalWrite(PIN_RIGHT, right ? HIGH : LOW);
}

void printInfo() {
  // Gesamtzyklus: LEFT_ON + gap + RIGHT_ON + gap.
  // Wir interpretieren side_interval_ms als Abstand zwischen Beginn links und Beginn rechts.
  // Ein kompletter Zyklus (links->rechts->wieder links) dauert ~ 2 * side_interval_ms.
  const float hz = 1000.0f / (2.0f * (float)side_interval_ms);
  Serial.println(F("\nAktuelle Einstellungen:"));
  Serial.print(F("  Intervall (ms)  : ")); Serial.println(side_interval_ms);
  Serial.print(F("  Puls (ms)       : ")); Serial.println(pulse_ms);
  Serial.print(F("  Takt (Hz)       : ")); Serial.println(hz, 3);
  Serial.println();
}

void printHelp() {
  Serial.println(F(
    "Kommandos:\n"
    "  ms <zahl>   : Intervall zwischen Start links & rechts (50..2000 ms)\n"
    "  hz <zahl>   : Gesamt-Blinktakt in Hz (links+rechts)\n"
    "  on <ms>     : Pulsbreite (20..400 ms)\n"
    "  info        : Werte anzeigen\n"
    "  help        : diese Hilfe\n"
    "Beispiele:  ms 250   |   hz 3.0   |   on 80\n"
  ));
}

void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line.trim();
      if (line.length() > 0) {
        // Parsen
        if (line.equalsIgnoreCase("info")) {
          printInfo();
        } else if (line.equalsIgnoreCase("help")) {
          printHelp();
        } else if (line.startsWith("ms")) {
          long v = line.substring(2).toInt();
          if (v >= (long)SIDE_MIN_MS && v <= (long)SIDE_MAX_MS) {
            side_interval_ms = (uint32_t)v;
            Serial.print(F("OK: Intervall = ")); Serial.println(side_interval_ms);
          } else {
            Serial.println(F("Fehler: ms ausserhalb 50..2000"));
          }
        } else if (line.startsWith("on")) {
          long v = line.substring(2).toInt();
          if (v >= (long)PULSE_MIN_MS && v <= (long)PULSE_MAX_MS) {
            pulse_ms = (uint32_t)v;
            Serial.print(F("OK: Puls = ")); Serial.println(pulse_ms);
          } else {
            Serial.println(F("Fehler: on ausserhalb 20..400"));
          }
        } else if (line.startsWith("hz")) {
          // hz setzt den Gesamtzyklus (links+rechts). side_interval_ms = (1000 / (2*hz))
          float h = line.substring(2).toFloat();
          if (h > 0.05f && h < 20.0f) {
            uint32_t newSide = (uint32_t)roundf(1000.0f / (2.0f * h));
            newSide = constrain(newSide, SIDE_MIN_MS, SIDE_MAX_MS);
            side_interval_ms = newSide;
            Serial.print(F("OK: Takt = ")); Serial.print(h, 3);
            Serial.print(F(" Hz -> Intervall = ")); Serial.println(side_interval_ms);
          } else {
            Serial.println(F("Fehler: hz sinnvoller Bereich ~0.05..20"));
          }
        } else {
          Serial.println(F("Unbekannt. 'help' eingeben."));
        }
      }
      line = "";
    } else {
      line += c;
    }
  }
}

void advanceState(State next) {
  state = next;
  stateStarted = millis();
}

void setup() {
  pinMode(PIN_LEFT, OUTPUT);
  pinMode(PIN_RIGHT, OUTPUT);
  setPins(false, false);

  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nESP32 Blaulicht gestartet."));
  printHelp();
  printInfo();

  stateStarted = millis();
  state = State::LEFT_ON;
}

void loop() {
  handleSerial();

  const uint32_t now = millis();

  switch (state) {
    case State::LEFT_ON:
      setPins(true, false);
      if (now - stateStarted >= pulse_ms) {
        advanceState(State::LEFT_OFF_GAP);
      }
      break;

    case State::LEFT_OFF_GAP:
      setPins(false, false);
      // Gap = side_interval_ms - pulse_ms (aber mindestens 0)
      if (now - stateStarted >= (side_interval_ms > pulse_ms ? (side_interval_ms - pulse_ms) : 0)) {
        advanceState(State::RIGHT_ON);
      }
      break;

    case State::RIGHT_ON:
      setPins(false, true);
      if (now - stateStarted >= pulse_ms) {
        advanceState(State::RIGHT_OFF_GAP);
      }
      break;

    case State::RIGHT_OFF_GAP:
      setPins(false, false);
      if (now - stateStarted >= (side_interval_ms > pulse_ms ? (side_interval_ms - pulse_ms) : 0)) {
        advanceState(State::LEFT_ON);
      }
      break;
  }
}
