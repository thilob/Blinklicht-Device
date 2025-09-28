#include <Arduino.h>

/*
  Vier entkoppelte Blaulichter mit identischem Pattern, aber je eigener Schluss-Pause.
  - PWM (LEDC) 5 kHz, 8 Bit
  - Pattern definiert EIN LED-Ablauf. Der letzte Step ist die Ruhepause.
  - Pro Ausgang lässt sich die Pausenlänge (nur letzter Step) separat überschreiben.
*/

/// -------------------- Hardware & LEDC --------------------
struct LightHW {
  uint8_t pin;
  uint8_t ledcChannel;     // 0..15 auf ESP32
  int     restOverrideMs;  // -1 = keine Änderung, sonst Dauer in ms für letzten Step
};

// Vorschlag: vorhandene 21/22 + zusätzlich 18/19 (beliebig anpassbar)
constexpr LightHW LIGHTS[4] = {
  {21, 0, -1   }, // Light A  (z.B. links vorne)  -> nutzt Pattern-Pause
  {22, 1, 251  }, // Light B  (z.B. rechts vorne) -> 400 ms Pause
  {18, 2, 253  }, // Light C  (z.B. links hinten) -> 250 ms Pause
  {19, 3, 255  }, // Light D  (z.B. rechts hinten)-> 600 ms Pause
};

constexpr uint32_t LEDC_FREQ_HZ = 5000;
constexpr uint8_t  LEDC_RES_BITS = 8;   // Duty 0..255

/// -------------------- (Optionale) Gamma-Korrektur --------------------
constexpr bool GAMMA_CORRECTION = false;
uint8_t gammaTable[256];
void buildGammaTable() {
  for (int i = 0; i < 256; ++i) {
    float norm = i / 255.0f;
    float gamma = powf(norm, 2.2f);
    gammaTable[i] = (uint8_t)roundf(gamma * 255.0f);
  }
}
inline uint8_t applyGamma(uint8_t v) {
  return GAMMA_CORRECTION ? gammaTable[v] : v;
}

/// -------------------- Pattern für EINE LED --------------------
struct LEDStep {
  uint16_t duration_ms;  // Länge des Schritts
  uint8_t  level;        // Zielhelligkeit 0..255
  bool     fade;         // weicher Übergang über die Step-Dauer
};

// Pattern aus deiner Spezifikation (linkes Blaulicht) + explizite Ruhepause am Ende
const LEDStep LEDSteps[] = {
  {  30, 255, false }, // L kurz AN (hart)
  {  30,   0, false }, // kurz AUS
  {  30, 255, false }, // L kurz AN (hart)
  {  30,   0, false }, // kurz AUS
  { 150, 255, false }, // L kurz AN (hart)
  {  30,   0, true  }, // L weich AUS
  {  30, 255, false }, // L kurz AN (hart)
  {   0,   0, false }, // sofort weiter
  { 250,   0, false }, // Ruhepause (LETZTER STEP)
};
constexpr size_t NUM_LED_STEPS = sizeof(LEDSteps) / sizeof(LEDSteps[0]);
constexpr size_t REST_STEP_INDEX = NUM_LED_STEPS - 1;

/* -------------------- Player für EINEN Ausgang -------------------- */
class PatternPlayer {
public:
  PatternPlayer(uint8_t ledcChannel, const LEDStep* steps, size_t count, int restOverrideMs = -1)
  : ch(ledcChannel), steps(steps), count(count), restOverride(restOverrideMs) {}

  void begin() {
    prev = 0;
    idx = 0;
    startStep(idx);
  }

  void update() {
    const auto& s = steps[idx];
    const uint32_t now = millis();
    const uint32_t dt  = now - started;

    const uint16_t dur = effectiveDuration(idx, s.duration_ms);

    if (dt >= dur) {
      writeLevel(target); // Ziel sicherstellen
      nextStep();
      return;
    }

    const float t = dur > 0 ? (float)dt / (float)dur : 1.0f;
    uint8_t cur = s.fade
      ? (uint8_t)roundf(prev + (float)((int)target - (int)prev) * t)
      : target; // bei nicht-Fade halten wir direkt den Zielwert

    writeLevel(cur);
  }

private:
  uint8_t ch;
  const LEDStep* steps;
  size_t count;
  int restOverride;

  size_t   idx = 0;
  uint32_t started = 0;
  uint8_t  prev = 0;
  uint8_t  target = 0;

  inline void writeLevel(uint8_t lvl) {
    ledcWrite(ch, applyGamma(lvl));
  }

  uint16_t effectiveDuration(size_t i, uint16_t nominal) const {
    if (i == REST_STEP_INDEX && restOverride >= 0) {
      return (uint16_t)restOverride;
    }
    return nominal;
  }

  void startStep(size_t i) {
    const auto& s = steps[i];
    started = millis();
    target = s.level;
    if (!s.fade) {
      writeLevel(target); // harter Sprung
    }
    // bei Fade: Interpolation von prev -> target in update()
  }

  void nextStep() {
    prev = target;
    idx = (idx + 1) % count;
    startStep(idx);
  }
};

/// -------------------- Vier Player-Instanzen --------------------
PatternPlayer* players[4]; // zur bequemen Schleifensteuerung

void setup() {
  if (GAMMA_CORRECTION) buildGammaTable();

  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println(F("Vier entkoppelte Blaulichter gestartet."));

  // LEDC konfigurieren und Player erzeugen
  for (int i = 0; i < 4; ++i) {
    ledcSetup(LIGHTS[i].ledcChannel, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(LIGHTS[i].pin, LIGHTS[i].ledcChannel);
  }

  static PatternPlayer p0(LIGHTS[0].ledcChannel, LEDSteps, NUM_LED_STEPS, LIGHTS[0].restOverrideMs);
  static PatternPlayer p1(LIGHTS[1].ledcChannel, LEDSteps, NUM_LED_STEPS, LIGHTS[1].restOverrideMs);
  static PatternPlayer p2(LIGHTS[2].ledcChannel, LEDSteps, NUM_LED_STEPS, LIGHTS[2].restOverrideMs);
  static PatternPlayer p3(LIGHTS[3].ledcChannel, LEDSteps, NUM_LED_STEPS, LIGHTS[3].restOverrideMs);

  players[0] = &p0;
  players[1] = &p1;
  players[2] = &p2;
  players[3] = &p3;

  for (auto* pl : players) pl->begin();

  // Debug-Ausgabe der Pausen
  Serial.println(F("Rest (override) je Ausgang [ms]:"));
  for (int i = 0; i < 4; ++i) {
    Serial.print(F("  Light ")); Serial.print(i);
    Serial.print(F(": ")); Serial.println(LIGHTS[i].restOverrideMs);
  }
}

void loop() {
  for (auto* pl : players) pl->update();
}
