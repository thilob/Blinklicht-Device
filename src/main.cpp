#include <Arduino.h>

/*
  Vier entkoppelte Blaulichter mit Mehrfach-Pattern:
  - Mehrere LED-Pattern (Steps) in einem Pattern-Array
  - Jeder Ausgang wählt per Index ein Pattern
  - Der letzte Step eines Patterns gilt als Ruhepause (Rest) und kann pro Ausgang übersteuert werden
  - PWM (LEDC) 5 kHz, 8 Bit, nicht-blockierend
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

// === Pattern 0: Dein gewünschtes „Blaulicht“-Pattern ===
// (aus deiner linken Seite übernommen; letzte Zeile = Ruhepause)
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

// === Pattern 1: Doppel-Strobe mit weichen Übergängen ===
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

// Array aller verfügbaren Pattern
const LEDPattern PATTERNS[] = {
  { PAT0_steps, sizeof(PAT0_steps)/sizeof(PAT0_steps[0]) },
  { PAT1_steps, sizeof(PAT1_steps)/sizeof(PAT1_steps[0]) },
  { PAT2_steps, sizeof(PAT2_steps)/sizeof(PAT2_steps[0]) },
};
constexpr size_t NUM_PATTERNS = sizeof(PATTERNS) / sizeof(PATTERNS[0]);

/// -------------------- Hardware & Kanal-Zuordnung --------------------
constexpr uint32_t LEDC_FREQ_HZ = 5000;
constexpr uint8_t  LEDC_RES_BITS = 8;   // Duty 0..255

struct LightHW {
  uint8_t pin;
  uint8_t ledcChannel;     // 0..15 (ESP32)
  uint8_t patternIndex;    // welches Pattern dieser Ausgang nutzt
  int     restOverrideMs;  // -1 = Pattern-Pause verwenden, sonst Dauer in ms für letzten Step
};

/*
  Beispiellayout:
    - GPIO 21/22 (wie bei dir), plus 18/19 als zusätzliche Ausgänge
    - Jeder Ausgang wählt ein Pattern (patternIndex)
    - restOverrideMs steuert NUR die Dauer des letzten Steps seines Patterns
*/
constexpr LightHW LIGHTS[4] = {
  {21, 0, 0, -1   }, // A: Pattern 0, Pause wie im Pattern
  {22, 1, 0, 400  }, // B: Pattern 0, Pause = 400 ms
  {18, 2, 1, 250  }, // C: Pattern 1, Pause = 250 ms
  {19, 3, 2, 600  }, // D: Pattern 2, Pause = 600 ms
};

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
PatternPlayer* players[4];

void setup() {
  if (GAMMA_CORRECTION) buildGammaTable();

  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println(F("Vier entkoppelte Blaulichter mit Mehrfach-Pattern."));

  // LEDC vorbereiten & Player erzeugen
  for (int i = 0; i < 4; ++i) {
    ledcSetup(LIGHTS[i].ledcChannel, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(LIGHTS[i].pin, LIGHTS[i].ledcChannel);
  }

  // Statische Player (damit gültige Adressen für players[])
  static PatternPlayer p0(LIGHTS[0].ledcChannel, &PATTERNS[ LIGHTS[0].patternIndex ], LIGHTS[0].restOverrideMs);
  static PatternPlayer p1(LIGHTS[1].ledcChannel, &PATTERNS[ LIGHTS[1].patternIndex ], LIGHTS[1].restOverrideMs);
  static PatternPlayer p2(LIGHTS[2].ledcChannel, &PATTERNS[ LIGHTS[2].patternIndex ], LIGHTS[2].restOverrideMs);
  static PatternPlayer p3(LIGHTS[3].ledcChannel, &PATTERNS[ LIGHTS[3].patternIndex ], LIGHTS[3].restOverrideMs);

  players[0] = &p0;
  players[1] = &p1;
  players[2] = &p2;
  players[3] = &p3;

  for (auto* pl : players) pl->begin();

  // Debug
  Serial.println(F("Konfiguration je Ausgang: pin, channel, patternIndex, restOverrideMs"));
  for (int i = 0; i < 4; ++i) {
    Serial.print(F("  L")); Serial.print(i);
    Serial.print(F(": ")); Serial.print(LIGHTS[i].pin);
    Serial.print(F(", ch=")); Serial.print(LIGHTS[i].ledcChannel);
    Serial.print(F(", pat=")); Serial.print(LIGHTS[i].patternIndex);
    Serial.print(F(", rest=")); Serial.println(LIGHTS[i].restOverrideMs);
  }
}

void loop() {
  for (auto* pl : players) pl->update();
}
