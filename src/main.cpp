#include <Arduino.h>

/*
  Zwei entkoppelte Blaulichter (links/rechts) mit gleichem LED-Pattern.
  - PWM (LEDC), 5 kHz, 8 Bit
  - Steps bestehen aus: Dauer(ms), Zielhelligkeit(0..255), Fade(true/false)
  - Beide Seiten laufen unabhängig (eigene Zeit- und Step-Indizes)
  - Rechtes Blaulicht kann eine abweichende Ruhepause (letzter Step) haben

  Anpassung:
    - Das Pattern unten (LEDSteps[]) beschreibt EINE LED.
    - Der letzte Step wird als "Ruhepause" interpretiert.
    - LEFT_TAIL_REST_OVERRIDE_MS = -1  -> benutze Dauer aus Pattern
    - RIGHT_TAIL_REST_OVERRIDE_MS = z.B. 400 -> überschreibe letzte Dauer nur rechts
*/

/// -------------------- Hardware-Pins & LEDC-Konfig --------------------
constexpr uint8_t PIN_LEFT   = 21;
constexpr uint8_t PIN_RIGHT  = 22;

constexpr uint8_t LEDC_CH_LEFT  = 0;
constexpr uint8_t LEDC_CH_RIGHT = 1;
constexpr uint32_t LEDC_FREQ_HZ = 5000;
constexpr uint8_t LEDC_RES_BITS = 8;   // Duty 0..255

/// -------------------- Gamma-Korrektur (optional) --------------------
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

/// -------------------- Step-Definition für EINE LED --------------------
struct LEDStep {
  uint16_t duration_ms;  // Länge des Schritts
  uint8_t  level;        // Zielhelligkeit 0..255
  bool     fade;         // weicher Übergang von vorher zu level über duration_ms
};

/*
  Vom Nutzer gewünschtes Pattern (aus der linken Seite extrahiert) + Ruhepause:

  Original (linke LED Abschnitte):
    {  30, 255, false }
    {  30,   0, false }
    {  30, 255, false }
    {  30,   0, false }
    { 150, 255, false }
    {  30,   0, true  }
    {  30, 255, false }
    {   0,   0, false }
    (gesamte Ruhepause am Ende des Zyklus im Original war 250 ms)

  Wir übernehmen das als einzelnes LED-Pattern und hängen eine explizite Ruhepause (250 ms) an.
*/
const LEDStep LEDSteps[] = {
  {  30, 255, false }, // kurz AN (hart)
  {  30,   0, false }, // kurz AUS
  {  30, 255, false }, // kurz AN (hart)
  {  30,   0, false }, // kurz AUS
  { 150, 255, false }, // kurz AN (hart)
  {  30,   0, true  }, // weich AUS
  {  30, 255, false }, // kurz AN (hart)
  {   0,   0, false }, // 0-ms-Schritt (sofort weiter)
  { 250,   0, false }, // Ruhepause (LETZTER STEP)
};
constexpr size_t NUM_LED_STEPS = sizeof(LEDSteps) / sizeof(LEDSteps[0]);

// Index des Ruhe-Schritts (hier der letzte Eintrag):
constexpr size_t REST_STEP_INDEX = NUM_LED_STEPS - 1;

// Per-Seite Override für die Ruhepause (letzter Step).
// -1 => keine Änderung, benutze Dauer aus Pattern
constexpr int LEFT_TAIL_REST_OVERRIDE_MS  = -1;   // z.B. -1 (Pattern-Dauer) oder 250
constexpr int RIGHT_TAIL_REST_OVERRIDE_MS = 251;  // z.B. 400 ms (rechte Seite separat)

/* -------------------- PatternPlayer: spielt EIN LED-Pattern -------------------- */
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
      // vor Wechsel sicherstellen, dass Zielpegel gesetzt ist
      writeLevel(target);
      nextStep();
      return;
    }

    const float t = dur > 0 ? (float)dt / (float)dur : 1.0f;
    uint8_t cur = s.fade
      ? (uint8_t)roundf(prev + (float)((int)target - (int)prev) * t)
      : target; // bei nicht-Fade halten wir das Ziel über die Dauer

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
    // Start-/Zielwerte
    // Bei fade=false springen wir sofort auf target und halten; bei fade=true interpolieren wir
    target = s.level;
    if (!s.fade) {
      writeLevel(target); // sofortiger Sprung
      // prev bleibt der vorherige Endwert; für den nächsten Step ist das ok
    }
    // Bei fade=true wird in update() von prev -> target interpoliert
  }

  void nextStep() {
    // Schritt fertig: Endwert übernehmen
    prev = target;
    idx = (idx + 1) % count;
    startStep(idx);
  }
};

/// -------------------- Globale Instanzen --------------------
PatternPlayer leftPlayer (LEDC_CH_LEFT,  LEDSteps, NUM_LED_STEPS, LEFT_TAIL_REST_OVERRIDE_MS);
PatternPlayer rightPlayer(LEDC_CH_RIGHT, LEDSteps, NUM_LED_STEPS, RIGHT_TAIL_REST_OVERRIDE_MS);

/// -------------------- Setup & Loop --------------------
void setup() {
  if (GAMMA_CORRECTION) buildGammaTable();

  ledcSetup(LEDC_CH_LEFT,  LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcSetup(LEDC_CH_RIGHT, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttachPin(PIN_LEFT,  LEDC_CH_LEFT);
  ledcAttachPin(PIN_RIGHT, LEDC_CH_RIGHT);

  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println(F("Entkoppelte Blaulichter gestartet."));
  Serial.print(F("Right tail rest override (ms): "));
  Serial.println(RIGHT_TAIL_REST_OVERRIDE_MS);

  leftPlayer.begin();
  rightPlayer.begin();
}

void loop() {
  leftPlayer.update();
  rightPlayer.update();
}
