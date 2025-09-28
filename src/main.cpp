#include <Arduino.h>

/*
  Acht entkoppelte Blaulichter mit Mehrfach-Pattern:
  - Mehrere LED-Pattern (Steps) in PATTERNS[]
  - Jeder Ausgang wählt per Index sein Pattern
  - Der letzte Step eines Patterns ist die Ruhepause und kann pro Ausgang übersteuert werden
  - PWM (LEDC) 5 kHz, 8 Bit, nicht-blockierend

  Hinweis: Der ESP32 bietet 16 LEDC-Kanäle (8 HS + 8 LS). Dieses Schema skaliert bis 16 Ausgänge,
  solange Pins als Output taugen (nicht: 34..39 input-only, 6..11 Flash, strapping pins beachten).
*/

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

/// -------------------- Pattern-Definitionen --------------------
struct LEDStep {
  uint16_t duration_ms;   // Länge des Schritts
  uint8_t  level;         // Zielhelligkeit 0..255
  bool     fade;          // weicher Übergang über die Dauer
};

struct LEDPattern {
  const LEDStep* steps;   // Zeiger auf Steps
  size_t         count;   // Anzahl Steps
  // Konvention: Der letzte Step (count-1) ist die Ruhepause
};

// === Pattern 0: Dein „Blaulicht“-Pattern (inkl. Ruhepause am Ende) ===
const LEDStep PAT0_steps[] = {
  {  30, 255, false }, // kurz AN (hart)
  {  30,   0, false }, // kurz AUS
  {  30, 255, false }, // kurz AN (hart)
  {  30,   0, false }, // kurz AUS
  { 150, 255, false }, // lang AN (hart)
  {  30,   0, true  }, // weich AUS
  {  30, 255, false }, // kurzer Nachblitz
  {   0,   0, false }, // sofort weiter
  { 250,   0, false }, // Ruhepause (LETZTER STEP)
};

// === Pattern 1: Doppel-Strobe weich ===
const LEDStep PAT1_steps[] = {
  {  60, 255, true  },
  {  40,   0, true  },
  {  60, 255, true  },
  {  40,   0, true  },
  { 180,   0, false }, // Ruhepause
};

// === Pattern 2: Langsamer Puls ===
const LEDStep PAT2_steps[] = {
  { 300, 255, true  }, // weich hoch
  { 300,   0, true  }, // weich runter
  { 200,   0, false }, // Ruhepause
};

// === Pattern 3: Blinker ===
const LEDStep PAT3_steps[] = {
  { 500, 255, false  }, // weich hoch
  { 500,   0, false  }, // weich runter
  { 0,   0, false }, // Ruhepause
};


// Array aller verfügbaren Pattern
const LEDPattern PATTERNS[] = {
  { PAT0_steps, sizeof(PAT0_steps)/sizeof(PAT0_steps[0]) },
  { PAT1_steps, sizeof(PAT1_steps)/sizeof(PAT1_steps[0]) },
  { PAT2_steps, sizeof(PAT2_steps)/sizeof(PAT2_steps[0]) },
  { PAT3_steps, sizeof(PAT3_steps)/sizeof(PAT3_steps[0]) },
};
constexpr size_t NUM_PATTERNS = sizeof(PATTERNS) / sizeof(PATTERNS[0]);

/// -------------------- Hardware & Kanal-Zuordnung (8 Ausgänge) --------------------
constexpr uint32_t LEDC_FREQ_HZ = 5000;
constexpr uint8_t  LEDC_RES_BITS = 8;   // Duty 0..255

struct LightHW {
  uint8_t pin;
  uint8_t ledcChannel;     // 0..15 (ESP32)
  uint8_t patternIndex;    // welches Pattern dieser Ausgang nutzt
  int     restOverrideMs;  // -1 = Pattern-Pause, sonst Dauer in ms für letzten Step
};

/*
  Pinvorschlag:
    - Vermeide GPIOs 6..11 (Flash), 34..39 (input-only), sowie strapping pins im Zweifel.
    - Hier: 21,22,18,19,23,25,26,27 (alle Output-tauglich).
*/
constexpr LightHW LIGHTS[] = {
  {21, 0, 0, -1   }, // L0: Pattern 0, Pause aus Pattern
  {22, 1, 0, 400  }, // L1: Pattern 0, Pause = 400 ms
  {18, 2, 1, 250  }, // L2: Pattern 1, Pause = 250 ms
  {19, 3, 2, 600  }, // L3: Pattern 2, Pause = 600 ms
  {23, 4, 0, 300  }, // L4: Pattern 0, Pause = 300 ms
  {25, 5, 1, -1   }, // L5: Pattern 1, Pause aus Pattern
  {26, 6, 3, 0  }, // L6: Pattern 2, Pause = 500 ms
  {27, 7, 3, 0  }, // L7: Pattern 0, Pause = 200 ms
};
constexpr size_t NUM_LIGHTS = sizeof(LIGHTS) / sizeof(LIGHTS[0]);

/// -------------------- Player für EINEN Ausgang --------------------
class PatternPlayer {
public:
  PatternPlayer(uint8_t ledcChannel, const LEDPattern* pat, int restOverrideMs = -1)
  : ch(ledcChannel), P(pat), restOverride(restOverrideMs) {}

  void begin() {
    prev = 0;
    idx = 0;
    startStep(idx);
  }

  void update() {
    const auto& s = P->steps[idx];
    const uint16_t dur = effectiveDuration(idx, s.duration_ms);
    const uint32_t now = millis();
    const uint32_t dt  = now - started;

    if (dt >= dur) {
      writeLevel(target); // Ziel sicherstellen
      nextStep();
      return;
    }

    const float t = dur > 0 ? (float)dt / (float)dur : 1.0f;
    uint8_t cur = s.fade
      ? (uint8_t)roundf(prev + (float)((int)target - (int)prev) * t)
      : target; // harter Step: Ziel sofort, dann halten

    writeLevel(cur);
  }

private:
  uint8_t ch;
  const LEDPattern* P;
  int restOverride;

  size_t   idx = 0;
  uint32_t started = 0;
  uint8_t  prev = 0;
  uint8_t  target = 0;

  inline void writeLevel(uint8_t lvl) {
    ledcWrite(ch, applyGamma(lvl));
  }

  inline size_t restIndex() const {
    return P->count ? (P->count - 1) : 0;
  }

  uint16_t effectiveDuration(size_t i, uint16_t nominal) const {
    if (i == restIndex() && restOverride >= 0) {
      return (uint16_t)restOverride;
    }
    return nominal;
  }

  void startStep(size_t i) {
    const auto& s = P->steps[i];
    started = millis();
    target = s.level;
    if (!s.fade) {
      writeLevel(target); // sofortiger Sprung
    }
    // bei Fade: Interpolation von prev -> target in update()
  }

  void nextStep() {
    prev = target;
    idx = (idx + 1) % P->count;
    startStep(idx);
  }
};

/// -------------------- Instanzen --------------------
PatternPlayer* players[NUM_LIGHTS];

void setup() {
  if (GAMMA_CORRECTION) buildGammaTable();

  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println(F("Acht entkoppelte Blaulichter mit Mehrfach-Pattern."));

  // LEDC vorbereiten & Player erzeugen
  for (size_t i = 0; i < NUM_LIGHTS; ++i) {
    ledcSetup(LIGHTS[i].ledcChannel, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(LIGHTS[i].pin, LIGHTS[i].ledcChannel);
  }

  // Statische Player (stabile Adressen für players[])
  static PatternPlayer p0(LIGHTS[0].ledcChannel, &PATTERNS[ LIGHTS[0].patternIndex ], LIGHTS[0].restOverrideMs);
  static PatternPlayer p1(LIGHTS[1].ledcChannel, &PATTERNS[ LIGHTS[1].patternIndex ], LIGHTS[1].restOverrideMs);
  static PatternPlayer p2(LIGHTS[2].ledcChannel, &PATTERNS[ LIGHTS[2].patternIndex ], LIGHTS[2].restOverrideMs);
  static PatternPlayer p3(LIGHTS[3].ledcChannel, &PATTERNS[ LIGHTS[3].patternIndex ], LIGHTS[3].restOverrideMs);
  static PatternPlayer p4(LIGHTS[4].ledcChannel, &PATTERNS[ LIGHTS[4].patternIndex ], LIGHTS[4].restOverrideMs);
  static PatternPlayer p5(LIGHTS[5].ledcChannel, &PATTERNS[ LIGHTS[5].patternIndex ], LIGHTS[5].restOverrideMs);
  static PatternPlayer p6(LIGHTS[6].ledcChannel, &PATTERNS[ LIGHTS[6].patternIndex ], LIGHTS[6].restOverrideMs);
  static PatternPlayer p7(LIGHTS[7].ledcChannel, &PATTERNS[ LIGHTS[7].patternIndex ], LIGHTS[7].restOverrideMs);

  players[0] = &p0; players[1] = &p1; players[2] = &p2; players[3] = &p3;
  players[4] = &p4; players[5] = &p5; players[6] = &p6; players[7] = &p7;

  for (auto* pl : players) pl->begin();

  // Debug & Info
  Serial.println(F("Konfiguration je Ausgang: pin, channel, patternIndex, restOverrideMs"));
  for (size_t i = 0; i < NUM_LIGHTS; ++i) {
    Serial.print(F("  L")); Serial.print(i);
    Serial.print(F(": ")); Serial.print(LIGHTS[i].pin);
    Serial.print(F(", ch=")); Serial.print(LIGHTS[i].ledcChannel);
    Serial.print(F(", pat=")); Serial.print(LIGHTS[i].patternIndex);
    Serial.print(F(", rest=")); Serial.println(LIGHTS[i].restOverrideMs);
  }

  // Wie viele PWM-Ausgänge sind prinzipiell möglich?
  const uint8_t MAX_LEDC_CHANNELS = 16; // ESP32 gesamt (8 HS + 8 LS)
  Serial.println();
  Serial.print(F("PWM-Kapazität ESP32 (LEDC-Kanäle): "));
  Serial.println(MAX_LEDC_CHANNELS);
  Serial.print(F("In diesem Sketch genutzt: "));
  Serial.println(NUM_LIGHTS);
}

void loop() {
  for (auto* pl : players) pl->update();
}
