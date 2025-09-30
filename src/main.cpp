#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <esp_bt.h>
#include <ESPmDNS.h>

/* =========================================================================
   Blaulicht-Controller (ESP32)
   - 8 Ausgänge, PWM (LEDC) 5 kHz / 8-bit, nicht-blockierend
   - Mehrere Patterns (letzter Step = Ruhepause), CRUD via Web-UI (AP/STA) ODER CLI
   - Lights: pin, channel, patternIndex, restOverrideMs, groupId, phase_ms
   - Gruppen an/aus (freeze der Zeit), Phasenverschiebung pro Ausgang
   - Persistenz: /config.json (LittleFS)  +  WLAN-Schalter wifi_enabled
   - WLAN: AP (eindeutige SSID) / STA (DHCP + mDNS Hostname)
   - JsonDocument-API (ArduinoJson v7)
   ========================================================================= */

/// -------------------- AP-/Netzwerk-Defaults --------------------
static const IPAddress AP_IP  (192,168,4,1);
static const IPAddress AP_GW  (192,168,4,1);
static const IPAddress AP_MASK(255,255,255,0);

/// -------------------- LEDC-Konfiguration --------------------
constexpr uint32_t LEDC_FREQ_HZ     = 5000;
constexpr uint8_t  LEDC_RES_BITS    = 8;      // Duty 0..255
constexpr uint8_t  MAX_LEDC_CHANNELS= 16;     // ESP32

/// -------------------- (Optionale) Gamma-Korrektur --------------------
constexpr bool GAMMA_CORRECTION = false;
static uint8_t gammaTable[256];
static inline uint8_t applyGamma(uint8_t v) {
  if (!GAMMA_CORRECTION) return v;
  return gammaTable[v];
}
static void buildGammaTable() {
  for (int i = 0; i < 256; ++i) {
    float norm = i / 255.0f;
    float gamma = powf(norm, 2.2f);
    gammaTable[i] = (uint8_t)roundf(gamma * 255.0f);
  }
}

/// -------------------- Datenmodelle --------------------
struct LEDStep {
  uint16_t duration_ms;   // Länge des Schritts
  uint8_t  level;         // Zielhelligkeit (0..255)
  bool     fade;          // weicher Übergang über die Dauer
};

struct Pattern {
  String               name;
  std::vector<LEDStep> steps;  // letzter Step = Ruhepause
};

struct GroupCfg {
  int     id;
  String  name;
  bool    enabled;
};

struct LightCfg {
  uint8_t pin;
  uint8_t channel;        // 0..15
  uint8_t patternIndex;   // Index in patterns[]
  int     restOverrideMs; // -1 => Pattern-Pause nutzen; sonst Dauer für letzten Step
  int     groupId;
  int     phase_ms;       // Phasenverschiebung relativ zum Pattern-Zyklus (>=0)
};

static std::vector<Pattern>  patterns;
static std::vector<GroupCfg> groups;
static std::vector<LightCfg> lights;

/// -------------------- WLAN-Config (persistiert) --------------------
static bool   wifiEnabled  = true;    // Hauptschalter
static String wifiModeCfg  = "ap";    // "ap" | "sta"
static String apSsidCfg    = "";      // leer => wird zu "Blaulicht-XXXX"
static String apPassCfg    = "";      // leer => offener AP
static String staSsidCfg   = "";
static String staPassCfg   = "";
static String hostName     = "";      // mDNS/Hostname (z. B. "blaulicht-xxxx")

static WebServer server(80);
static bool serverRunning = false;

/// -------------------- Helpers --------------------
static bool isGroupEnabled(int gid) {
  for (const auto &g : groups) if (g.id == gid) return g.enabled;
  return true; // unbekannte Gruppe => nicht blockieren
}

static String uniqueMacSuffix() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char buf[7]; snprintf(buf, sizeof(buf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

static String makeDefaultApSsid() {
  return String("Blaulicht-") + uniqueMacSuffix();
}

static void sanitizeHostName(String &hn) {
  hn.toLowerCase();
  for (size_t i=0;i<hn.length();++i) {
    char c=hn[i];
    if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-')) hn.setCharAt(i,'-');
  }
  while (hn.startsWith("-")) hn.remove(0,1);
  while (hn.endsWith("-")) hn.remove(hn.length()-1);
  if (!hn.length()) hn = "esp32";
}

/// -------------------- Player für EINEN Ausgang --------------------
class PatternPlayer {
public:
  PatternPlayer() = default;

  void attach(uint8_t ledcChannel) { ch = ledcChannel; }
  void bind(int lightIndex) { boundLightIndex = lightIndex; }

  void begin() {
    prev = 0;
    idx = 0;
    started = millis();
    lastDt = 0;
    initialized = false;
  }

  void update() {
    if (!isValidBinding()) return;

    const LightCfg &L = lights[boundLightIndex];
    if ((size_t)L.patternIndex >= patterns.size()) { writeLevel(0); return; }
    const Pattern &P = patterns[L.patternIndex];
    if (P.steps.empty()) { writeLevel(0); return; }

    if (!initialized) {
      initWithPhase(L, P);
      initialized = true;
    }

    if (!isGroupEnabled(L.groupId)) {
      writeLevel(0);
      started = millis() - lastDt; // freeze
      return;
    }

    const LEDStep &s = P.steps[idx];
    const uint16_t dur = effectiveDuration(L, P, idx, s.duration_ms);

    const uint32_t now = millis();
    lastDt = now - started;

    if (lastDt >= dur) {
      writeLevel(target);
      nextStep(L, P);
      return;
    }

    const float t = dur > 0 ? (float)lastDt / (float)dur : 1.0f;
    uint8_t cur = s.fade
      ? (uint8_t)roundf(prev + (float)((int)target - (int)prev) * t)
      : target;
    writeLevel(cur);
  }

private:
  uint8_t ch = 0;
  int     boundLightIndex = -1;

  bool     initialized = false;
  size_t   idx = 0;
  uint32_t started = 0;
  uint32_t lastDt  = 0;
  uint8_t  prev = 0;
  uint8_t  target = 0;

  inline void writeLevel(uint8_t lvl) { ledcWrite(ch, applyGamma(lvl)); }

  bool isValidBinding() const {
    return boundLightIndex >= 0 && (size_t)boundLightIndex < lights.size();
  }

  static size_t restIndexOf(const Pattern &P) {
    return P.steps.empty() ? 0 : (P.steps.size() - 1);
  }

  static uint16_t effectiveDuration(const LightCfg &L, const Pattern &P, size_t i, uint16_t nominal) {
    if (i == restIndexOf(P) && L.restOverrideMs >= 0) return (uint16_t)L.restOverrideMs;
    return nominal;
  }

  static uint32_t patternTotalMs(const LightCfg &L, const Pattern &P) {
    uint32_t total = 0;
    for (size_t i = 0; i < P.steps.size(); ++i)
      total += effectiveDuration(L, P, i, P.steps[i].duration_ms);
    return total;
  }

  void initWithPhase(const LightCfg &L, const Pattern &P) {
    uint32_t total = patternTotalMs(L, P);
    uint32_t phase = (L.phase_ms >= 0 && total > 0) ? (uint32_t)L.phase_ms % total : 0;

    size_t step = 0; uint32_t acc = 0;
    while (step < P.steps.size()) {
      uint16_t dur = effectiveDuration(L, P, step, P.steps[step].duration_ms);
      if (phase < acc + dur) break;
      acc += dur; ++step;
    }
    if (step >= P.steps.size()) step = 0;

    idx = step;
    const LEDStep &s = P.steps[idx];
    prev = P.steps[ idx == 0 ? (P.steps.size()-1) : (idx-1) ].level;
    target = s.level;
    if (!s.fade) writeLevel(target);

    started = millis() - (phase - acc);
    lastDt  = (phase - acc);
  }

  void nextStep(const LightCfg &L, const Pattern &P) {
    prev = target;
    idx = (idx + 1) % P.steps.size();
    const LEDStep &s = P.steps[idx];
    target = s.level;
    if (!s.fade) writeLevel(target);
    started = millis();
    lastDt = 0;
  }
};

static PatternPlayer players[8];

/// -------------------- Persistenz --------------------
static bool saveConfig() {
  File f = LittleFS.open("/config.json", FILE_WRITE);
  if (!f) return false;

  JsonDocument doc;

  // wifi
  JsonObject jw = doc["wifi"].to<JsonObject>();
  jw["enabled"] = wifiEnabled;
  jw["mode"]    = wifiModeCfg; // "ap" | "sta"
  JsonObject jwa = jw["ap"].to<JsonObject>();
  jwa["ssid"]     = apSsidCfg.length() ? apSsidCfg : makeDefaultApSsid();
  jwa["password"] = apPassCfg;
  JsonObject jws = jw["sta"].to<JsonObject>();
  jws["ssid"]     = staSsidCfg;
  jws["password"] = staPassCfg;

  // patterns
  JsonArray jPatterns = doc["patterns"].to<JsonArray>();
  for (const auto &p : patterns) {
    JsonObject jp = jPatterns.add<JsonObject>();
    jp["name"] = p.name;
    JsonArray jSteps = jp["steps"].to<JsonArray>();
    for (const auto &st : p.steps) {
      JsonObject js = jSteps.add<JsonObject>();
      js["duration_ms"] = st.duration_ms;
      js["level"]       = st.level;
      js["fade"]        = st.fade;
    }
  }
  // groups
  JsonArray jGroups = doc["groups"].to<JsonArray>();
  for (const auto &g : groups) {
    JsonObject jg = jGroups.add<JsonObject>();
    jg["id"] = g.id; jg["name"] = g.name; jg["enabled"] = g.enabled;
  }
  // lights
  JsonArray jLights = doc["lights"].to<JsonArray>();
  for (const auto &L : lights) {
    JsonObject jl = jLights.add<JsonObject>();
    jl["pin"]            = L.pin;
    jl["channel"]        = L.channel;
    jl["patternIndex"]   = L.patternIndex;
    jl["restOverrideMs"] = L.restOverrideMs;
    jl["groupId"]        = L.groupId;
    jl["phase_ms"]       = L.phase_ms;
  }

  bool ok = serializeJsonPretty(doc, f) > 0;
  f.close();
  return ok;
}

static bool loadConfig() {
  if (!LittleFS.exists("/config.json")) return false;
  File f = LittleFS.open("/config.json", FILE_READ);
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  // WLAN
  wifiEnabled = (bool)(doc["wifi"]["enabled"] | true);
  wifiModeCfg = String(doc["wifi"]["mode"]    | "ap");
  if (doc["wifi"]["ap"].is<JsonObject>()) {
    apSsidCfg = String(doc["wifi"]["ap"]["ssid"]     | "");
    apPassCfg = String(doc["wifi"]["ap"]["password"] | "");
  }
  if (doc["wifi"]["sta"].is<JsonObject>()) {
    staSsidCfg= String(doc["wifi"]["sta"]["ssid"]     | "");
    staPassCfg= String(doc["wifi"]["sta"]["password"] | "");
  }

  patterns.clear(); groups.clear(); lights.clear();

  for (JsonObject jp : doc["patterns"].as<JsonArray>()) {
    Pattern p; p.name = jp["name"].as<const char*>();
    for (JsonObject js : jp["steps"].as<JsonArray>()) {
      LEDStep st;
      st.duration_ms = (uint16_t)(js["duration_ms"] | 0);
      st.level       = (uint8_t) (js["level"]       | 0);
      st.fade        = (bool)    (js["fade"]        | false);
      p.steps.push_back(st);
    }
    patterns.push_back(std::move(p));
  }

  for (JsonObject jg : doc["groups"].as<JsonArray>()) {
    GroupCfg g;
    g.id      = (int)(jg["id"]      | 0);
    g.name    = jg["name"].as<const char*>();
    g.enabled = (bool)(jg["enabled"] | true);
    groups.push_back(std::move(g));
  }

  for (JsonObject jl : doc["lights"].as<JsonArray>()) {
    LightCfg L;
    L.pin            = (uint8_t)(jl["pin"]            | 255);
    L.channel        = (uint8_t)(jl["channel"]        | 0);
    L.patternIndex   = (uint8_t)(jl["patternIndex"]   | 0);
    L.restOverrideMs = (int)     (jl["restOverrideMs"]| -1);
    L.groupId        = (int)     (jl["groupId"]       | 0);
    L.phase_ms       = (int)     (jl["phase_ms"]      | 0);
    lights.push_back(std::move(L));
  }
  return true;
}

static void makeDefaultConfig() {
  wifiEnabled = true;
  wifiModeCfg = "ap";
  apSsidCfg   = "";     // automatisch "Blaulicht-XXXX"
  apPassCfg   = "";     // offen
  staSsidCfg  = "";
  staPassCfg  = "";

  patterns.clear(); groups.clear(); lights.clear();

  // Pattern 0 (Blaulicht)
  {
    Pattern p; p.name = "Pat0-Blaulicht";
    LEDStep s[] = {
      {30,255,false},{30,0,false},{30,255,false},{30,0,false},
      {150,255,false},{30,0,true},{30,255,false},{0,0,false},{250,0,false}
    };
    p.steps.assign(std::begin(s), std::end(s));
    patterns.push_back(std::move(p));
  }
  // Pattern 1
  {
    Pattern p; p.name = "Pat1-DoppelStrobeWeich";
    LEDStep s[] = { {60,255,true},{40,0,true},{60,255,true},{40,0,true},{180,0,false} };
    p.steps.assign(std::begin(s), std::end(s));
    patterns.push_back(std::move(p));
  }

  groups.push_back({0,"Front",true});
  groups.push_back({1,"Heck", true});

  lights = {
    LightCfg{21,0,0,-1,   0,   0},
    LightCfg{22,1,0,400,  0,  50},
    LightCfg{18,2,1,250,  1, 100},
    LightCfg{19,3,0,600,  1, 150},
    LightCfg{23,4,0,300,  0, 200},
    LightCfg{25,5,1,-1,   0, 250},
    LightCfg{26,6,1,500,  1, 300},
    LightCfg{27,7,0,200,  1, 350}
  };
}

/// Hardware anwenden (LEDC setup/attach) + Player binden & (re)starten
static void applyHardware() {
  for (const auto &L : lights) {
    if (L.channel >= MAX_LEDC_CHANNELS) continue;
    ledcSetup(L.channel, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(L.pin, L.channel);
    ledcWrite(L.channel, 0);
  }
  size_t n = min<size_t>(8, lights.size());
  for (size_t i = 0; i < n; ++i) {
    players[i].attach(lights[i].channel);
    players[i].bind((int)i);
    players[i].begin();
  }
}

/// -------------------- Static Files --------------------
static String contentTypeFor(const String& path) {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".css"))  return "text/css; charset=utf-8";
  if (path.endsWith(".js"))   return "application/javascript; charset=utf-8";
  if (path.endsWith(".json")) return "application/json; charset=utf-8";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".svg"))  return "image/svg+xml; charset=utf-8";
  return "text/plain; charset=utf-8";
}
static bool handleFileRead(String path) {
  if (path.endsWith("/")) path += "index.html";
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  server.streamFile(f, contentTypeFor(path));
  f.close();
  return true;
}

/// -------------------- CORS / Captive / JSON helper --------------------
static void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Accept");
}
static void sendJSON(const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  addCORS();
  server.send(200, "application/json; charset=utf-8", json);
}
static bool isAPModeActive() {
  return (wifiEnabled && wifiModeCfg == "ap" && WiFi.getMode() == WIFI_AP);
}

// Captive-Portal: nur für HTML, nie für API
static bool handleCaptivePortalRedirect()
{
  const String uri = server.uri();
  if (uri.startsWith("/api/")) return false;
  const String accept = server.header("Accept");
  if (accept.indexOf("application/json") >= 0) return false;
  if (!isAPModeActive()) return false;

  IPAddress ip = WiFi.softAPIP();
  String host = server.hostHeader(); host.toLowerCase();
  if (host != ip.toString()) {
    server.sendHeader("Location", String("http://") + ip.toString() + "/");
    server.send(302, "text/plain", "");
    return true;
  }
  return false;
}

/// -------------------- REST Endpoints --------------------
static void handleGetConfig() {
  JsonDocument doc;

  // WLAN
  JsonObject jw = doc["wifi"].to<JsonObject>();
  jw["enabled"] = wifiEnabled;
  jw["mode"]    = wifiModeCfg;
  JsonObject jwa = jw["ap"].to<JsonObject>();
  jwa["ssid"]     = apSsidCfg.length()?apSsidCfg:makeDefaultApSsid();
  jwa["password"] = apPassCfg;
  JsonObject jws = jw["sta"].to<JsonObject>();
  jws["ssid"]     = staSsidCfg;
  jws["password"] = staPassCfg;

  // patterns
  JsonArray jPatterns = doc["patterns"].to<JsonArray>();
  for (const auto &p : patterns) {
    JsonObject jp = jPatterns.add<JsonObject>();
    jp["name"] = p.name;
    JsonArray jSteps = jp["steps"].to<JsonArray>();
    for (const auto &st : p.steps) {
      JsonObject js = jSteps.add<JsonObject>();
      js["duration_ms"] = st.duration_ms;
      js["level"]       = st.level;
      js["fade"]        = st.fade;
    }
  }
  // groups
  JsonArray jGroups = doc["groups"].to<JsonArray>();
  for (const auto &g : groups) {
    JsonObject jg = jGroups.add<JsonObject>();
    jg["id"] = g.id; jg["name"] = g.name; jg["enabled"] = g.enabled;
  }
  // lights
  JsonArray jLights = doc["lights"].to<JsonArray>();
  for (const auto &L : lights) {
    JsonObject jl = jLights.add<JsonObject>();
    jl["pin"]            = L.pin;
    jl["channel"]        = L.channel;
    jl["patternIndex"]   = L.patternIndex;
    jl["restOverrideMs"] = L.restOverrideMs;
    jl["groupId"]        = L.groupId;
    jl["phase_ms"]       = L.phase_ms;
  }

  String out; serializeJson(doc, out);
  sendJSON(out);
}

static void handlePutConfig() {
  if (!server.hasArg("plain")) { addCORS(); server.send(400, "text/plain", "Missing body"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) { addCORS(); server.send(400, "text/plain", String("JSON error: ")+err.c_str()); return; }

  // --- WLAN übernehmen (optional vorhanden) ---
  if (doc["wifi"].is<JsonObject>()) {
    wifiEnabled = (bool)(doc["wifi"]["enabled"] | wifiEnabled);
    String newMode = String( doc["wifi"]["mode"] | wifiModeCfg.c_str() );
    newMode.toLowerCase();
    if (newMode == "ap" || newMode == "sta") wifiModeCfg = newMode;

    if (doc["wifi"]["ap"].is<JsonObject>()) {
      apSsidCfg = String( doc["wifi"]["ap"]["ssid"]     | apSsidCfg.c_str() );
      apPassCfg = String( doc["wifi"]["ap"]["password"] | apPassCfg.c_str() );
    }
    if (doc["wifi"]["sta"].is<JsonObject>()) {
      staSsidCfg= String( doc["wifi"]["sta"]["ssid"]     | staSsidCfg.c_str() );
      staPassCfg= String( doc["wifi"]["sta"]["password"] | staPassCfg.c_str() );
    }
  }

  // --- Pflicht: patterns/groups/lights ---
  if (!doc["patterns"].is<JsonArray>() || !doc["lights"].is<JsonArray>() || !doc["groups"].is<JsonArray>()) {
    addCORS(); server.send(400,"text/plain","Invalid JSON structure"); return;
  }

  patterns.clear(); groups.clear(); lights.clear();

  for (JsonObject jp : doc["patterns"].as<JsonArray>()) {
    Pattern p; p.name = jp["name"].as<const char*>();
    for (JsonObject js : jp["steps"].as<JsonArray>()) {
      LEDStep st;
      st.duration_ms = (uint16_t)(js["duration_ms"] | 0);
      st.level       = (uint8_t) (js["level"]       | 0);
      st.fade        = (bool)    (js["fade"]        | false);
      p.steps.push_back(st);
    }
    patterns.push_back(std::move(p));
  }
  for (JsonObject jg : doc["groups"].as<JsonArray>()) {
    GroupCfg g;
    g.id      = (int)(jg["id"]      | 0);
    g.name    = jg["name"].as<const char*>();
    g.enabled = (bool)(jg["enabled"] | true);
    groups.push_back(std::move(g));
  }
  for (JsonObject jl : doc["lights"].as<JsonArray>()) {
    LightCfg L;
    L.pin            = (uint8_t)(jl["pin"]            | 255);
    L.channel        = (uint8_t)(jl["channel"]        | 0);
    L.patternIndex   = (uint8_t)(jl["patternIndex"]   | 0);
    L.restOverrideMs = (int)     (jl["restOverrideMs"]| -1);
    L.groupId        = (int)     (jl["groupId"]       | 0);
    L.phase_ms       = (int)     (jl["phase_ms"]      | 0);
    lights.push_back(std::move(L));
  }

  // Hardware & WLAN anwenden
  applyHardware();

  if (wifiEnabled) {
    // Re-konfigurieren
    if (serverRunning) { server.stop(); serverRunning=false; }
    // Neustart des WLANs je nach Modus
    // (startWiFi() setzt alles – s.u.)
  } else {
    if (serverRunning) { server.stop(); serverRunning=false; }
    WiFi.mode(WIFI_OFF);
  }

  bool ok = saveConfig();

  // WLAN neu starten (je nach cfg)
  if (wifiEnabled) {
    // bewusst nach saveConfig(), damit neue SSID/Passwörter gelten
    // (Start inkl. DHCP im STA-Modus)
    // Vorher hart stoppen, um Reste zu vermeiden:
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
  }

  // Antwort zuerst senden, danach neu starten, um den HTTP-Ablauf nicht zu unterbrechen
  addCORS(); server.send(ok?200:500, "text/plain", ok?"OK (saved)":"ERROR (save failed)");

  // Nach kurzer Verzögerung WLAN starten
  delay(50);
  if (wifiEnabled) {
    // startWiFi holt DHCP im STA
    // und registriert Routen + server.begin()
    // (Definition unten)
    extern void startWiFi();
    startWiFi();
  }
}

/// -------------------- WIFI Start/Stop --------------------
static void startServerRoutes() {
  // Favicon & Touch-Icons -> leere 204-Antwort
  auto noContent = [](){ addCORS(); server.send(204, "text/plain", ""); };
  server.on("/favicon.ico", HTTP_ANY, noContent);
  server.on("/apple-touch-icon.png", HTTP_ANY, noContent);
  server.on("/apple-touch-icon-precomposed.png", HTTP_ANY, noContent);

  // Root
  server.on("/", HTTP_GET, [](){
    if (handleCaptivePortalRedirect()) return;
    if (!handleFileRead("/index.html")) { addCORS(); server.send(404,"text/plain","index.html not found"); }
  });

  // APIs
  server.on("/api/config",  HTTP_GET, handleGetConfig);
  server.on("/api/config",  HTTP_PUT, handlePutConfig);

  server.on("/api/sysinfo", HTTP_GET, [](){
    JsonDocument doc;
    doc["max_ledc_channels"] = MAX_LEDC_CHANNELS;
    doc["ledc_freq_hz"]      = LEDC_FREQ_HZ;
    doc["ledc_res_bits"]     = LEDC_RES_BITS;
    doc["num_lights"]        = (uint32_t)lights.size();
    String out; serializeJson(doc, out);
    sendJSON(out);
  });

  // OPTIONS-Preflight & statische Dateien
  server.onNotFound([](){
    if (server.method() == HTTP_OPTIONS) { addCORS(); server.send(204); return; }
    if (handleCaptivePortalRedirect()) return;
    String path = server.uri();
    if (handleFileRead(path)) return;
    addCORS();
    server.send(404, "text/plain", "Not found");
  });
}

static void stopWiFi() {
  if (serverRunning) {
    server.stop();
    serverRunning = false;
  }
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  MDNS.end();
  Serial.println("WLAN/Server gestoppt.");
}

void startWiFi() {
  // Hostname bestimmen
  String base = apSsidCfg.length()?apSsidCfg:makeDefaultApSsid();
  // Hostname ohne Leerzeichen/Sonderzeichen und in Kleinbuchstaben
  hostName = base;
  hostName.replace(" ", "-");
  sanitizeHostName(hostName);

  if (!wifiEnabled) {
    stopWiFi();
    return;
  }

  // STA-Modus
  if (wifiModeCfg == "sta" && staSsidCfg.length()) {
    stopWiFi(); // sicherstellen, dass alles sauber startet
    WiFi.mode(WIFI_STA);

    // DHCP erzwingen (setzt IP/GW/Subnet auf 0)
    WiFi.config((uint32_t)0U, (uint32_t)0U, (uint32_t)0U);

    WiFi.setHostname(hostName.c_str());
    Serial.printf("STA connecting to SSID='%s' ...\n", staSsidCfg.c_str());
    WiFi.begin(staSsidCfg.c_str(), staPassCfg.c_str());

    uint32_t t0=millis();
    while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) delay(100);

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("STA connected: IP=%s  GW=%s  SN=%s  Host='%s.local'\n",
        WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str(),
        hostName.c_str());

      MDNS.begin(hostName.c_str());
      MDNS.addService("http", "tcp", 80);
    } else {
      Serial.println("STA connect failed. Fallback auf AP.");
      wifiModeCfg = "ap"; // AP fallback
    }
  }

  // AP-Modus (Fallback oder explizit)
  if (wifiModeCfg == "ap") {
    stopWiFi(); // sicherheitshalber
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
    String ssid = apSsidCfg.length()?apSsidCfg:makeDefaultApSsid();
    bool apok = WiFi.softAP(ssid.c_str(), apPassCfg.c_str()); // pass darf leer sein
    WiFi.setSleep(true);
    WiFi.setTxPower(WIFI_POWER_5dBm); // moderater TX-Pegel
    Serial.printf("AP gestartet: %s (%s), ok=%d\n", WiFi.SSID().c_str(), WiFi.softAPIP().toString().c_str(), apok);
    // mDNS im AP-Modus wird oft nicht unterstützt -> weglassen
  }

  startServerRoutes();
  server.begin();
  serverRunning = true;
}

/// -------------------- CLI (Seriell) --------------------
static String cliLine;

static void cliHelp() {
  Serial.println(F(
    "\n=== CLI Befehle ===\n"
    "help                              : diese Hilfe\n"
    "status                            : Kurzstatus anzeigen\n"
    "save                              : Konfiguration speichern\n"
    "load                              : Konfiguration neu laden\n"
    "reboot                            : Neustart\n"
    "wifi on|off                       : WLAN/Webserver einschalten/abschalten (persistent)\n"
    "mode ap|sta                       : WLAN-Modus setzen (persistent)\n"
    "sta <ssid> <pass>                 : STA-Zugangsdaten setzen (persistent)\n"
    "ap  <ssid> [pass]                 : AP-SSID/PW setzen (pass optional)\n"
    "dhcp                              : im STA-Modus DHCP neu beziehen (reconnect)\n"
    "\n-- Gruppen --\n"
    "group list                        : Gruppen anzeigen\n"
    "group add <id> <name>             : Gruppe anlegen (enabled=1)\n"
    "group set <id> enabled <0|1>      : Gruppe schalten\n"
    "group set <id> name <text>        : Namen ändern\n"
    "group del <id>                    : Gruppe löschen\n"
    "\n-- Patterns --\n"
    "pat list                          : Patterns auflisten\n"
    "pat add <name>                    : Leeres Pattern anlegen\n"
    "pat del <idx>                     : Pattern löschen\n"
    "pat step add <pidx> <dur> <lvl> <fade0|1>\n"
    "pat step set <pidx> <sidx> <dur> <lvl> <fade0|1>\n"
    "pat step del <pidx> <sidx>\n"
    "\n-- Lights --\n"
    "light list                        : Lichter auflisten\n"
    "light add <pin> <ch> <pidx> <rest> <gid> <phase>\n"
    "light set <idx> <field> <value>   : field=pin|ch|pidx|rest|gid|phase\n"
    "light del <idx>                   : Ausgang löschen\n"
  ));
}

static long asLong(const String& s, long def=0){ char* e=nullptr; long v=strtol(s.c_str(), &e, 10); return (e && *e==0) ? v : def; }

static void cliStatus() {
  Serial.printf("wifi_enabled: %s, mode: %s, serverRunning: %d, host: %s\n",
    wifiEnabled?"true":"false", wifiModeCfg.c_str(), (int)serverRunning, hostName.c_str());
  if (WiFi.getMode()==WIFI_STA && WiFi.status()==WL_CONNECTED) {
    Serial.printf("STA IP=%s GW=%s SN=%s\n",
      WiFi.localIP().toString().c_str(),
      WiFi.gatewayIP().toString().c_str(),
      WiFi.subnetMask().toString().c_str());
  } else if (WiFi.getMode()==WIFI_AP) {
    Serial.printf("AP  IP=%s SSID=%s\n",
      WiFi.softAPIP().toString().c_str(), WiFi.SSID().c_str());
  }
  Serial.printf("patterns: %u, groups: %u, lights: %u\n", (unsigned)patterns.size(), (unsigned)groups.size(), (unsigned)lights.size());
}

static void cliGroupList() {
  for (auto &g: groups) Serial.printf("id=%d name='%s' enabled=%d\n", g.id, g.name.c_str(), g.enabled);
}
static void cliPatList() {
  for (size_t i=0;i<patterns.size();++i)
    Serial.printf("[%u] '%s' steps=%u\n", (unsigned)i, patterns[i].name.c_str(), (unsigned)patterns[i].steps.size());
}
static void cliLightList() {
  for (size_t i=0;i<lights.size();++i) {
    auto &L = lights[i];
    Serial.printf("[%u] pin=%u ch=%u pidx=%u rest=%d gid=%d phase=%d\n",
      (unsigned)i, L.pin, L.channel, L.patternIndex, L.restOverrideMs, L.groupId, L.phase_ms);
  }
}

static void cliApplyAndMaybeStartWifi() {
  applyHardware();
  if (wifiEnabled && !serverRunning) startWiFi();
  if (!wifiEnabled && serverRunning) stopWiFi();
}

static void cliDhcpRenew() {
  if (wifiModeCfg != "sta") { Serial.println("Nicht im STA-Modus."); return; }
  if (!staSsidCfg.length()) { Serial.println("STA SSID nicht gesetzt."); return; }
  Serial.println("DHCP erneuern...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false,true);
  WiFi.config((uint32_t)0U, (uint32_t)0U, (uint32_t)0U);
  WiFi.begin(staSsidCfg.c_str(), staPassCfg.c_str());
  uint32_t t0=millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 10000) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Neue STA IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("DHCP fehlgeschlagen.");
  }
}

static void handleCliLine(const String& line) {
  std::vector<String> t; String cur;
  for (size_t i=0;i<line.length();++i) {
    char c=line[i];
    if (c==' '||c=='\t'){ if(cur.length()){t.push_back(cur); cur="";} }
    else cur+=c;
  }
  if(cur.length()) t.push_back(cur);
  if (t.empty()) return;
  String cmd = t[0]; cmd.toLowerCase();

  if (cmd=="help") { cliHelp(); return; }
  if (cmd=="status") { cliStatus(); return; }
  if (cmd=="save") { bool ok=saveConfig(); Serial.println(ok?"OK saved":"ERR save"); return; }
  if (cmd=="load") { bool ok=loadConfig(); Serial.println(ok?"OK loaded":"ERR load"); cliApplyAndMaybeStartWifi(); return; }
  if (cmd=="reboot") { Serial.println("Rebooting..."); delay(100); ESP.restart(); }

  if (cmd=="wifi" && t.size()>=2) {
    String v=t[1]; v.toLowerCase();
    if (v=="on")  { wifiEnabled=true;  saveConfig(); startWiFi(); Serial.println("WiFi ON."); }
    else if (v=="off"){ wifiEnabled=false; saveConfig(); stopWiFi(); Serial.println("WiFi OFF."); }
    else Serial.println("Usage: wifi on|off");
    return;
  }
  if (cmd=="mode" && t.size()>=2) {
    String m=t[1]; m.toLowerCase();
    if (m=="ap"||m=="sta") { wifiModeCfg=m; saveConfig(); startWiFi(); Serial.println("OK"); }
    else Serial.println("mode ap|sta");
    return;
  }
  if (cmd=="sta" && t.size()>=3) {
    staSsidCfg=t[1]; staPassCfg=t[2]; saveConfig(); startWiFi(); Serial.println("OK");
    return;
  }
  if (cmd=="ap" && t.size()>=2) {
    apSsidCfg=t[1]; apPassCfg = (t.size()>=3)?t[2]:"";
    saveConfig(); startWiFi(); Serial.println("OK");
    return;
  }
  if (cmd=="dhcp") { cliDhcpRenew(); return; }

  // Gruppen
  if (cmd=="group" && t.size()>=2) {
    String sub=t[1]; sub.toLowerCase();
    if (sub=="list") { cliGroupList(); return; }
    if (sub=="add" && t.size()>=4) {
      int id=asLong(t[2]); String name=t[3];
      groups.push_back({id,name,true}); saveConfig(); Serial.println("OK"); return;
    }
    if (sub=="set" && t.size()>=5) {
      int id=asLong(t[2]); String field=t[3]; field.toLowerCase(); String val=t[4];
      for (auto &g: groups) if (g.id==id) {
        if (field=="enabled") g.enabled = (asLong(val)!=0);
        else if (field=="name") g.name = val;
        else { Serial.println("Unknown field"); return; }
        saveConfig(); Serial.println("OK"); return;
      }
      Serial.println("Group not found"); return;
    }
    if (sub=="del" && t.size()>=3) {
      int id=asLong(t[2]);
      for (size_t i=0;i<groups.size();++i) if (groups[i].id==id){ groups.erase(groups.begin()+i); saveConfig(); Serial.println("OK"); return; }
      Serial.println("Group not found"); return;
    }
  }

  // Patterns
  if (cmd=="pat" && t.size()>=2) {
    String sub=t[1]; sub.toLowerCase();
    if (sub=="list") { cliPatList(); return; }
    if (sub=="add" && t.size()>=3) { Pattern p; p.name=t[2]; patterns.push_back(std::move(p)); saveConfig(); cliPatList(); return; }
    if (sub=="del" && t.size()>=3) {
      int idx=asLong(t[2],-1); if (idx<0||idx>=(int)patterns.size()) {Serial.println("Index!"); return;}
      patterns.erase(patterns.begin()+idx); saveConfig(); cliPatList(); return;
    }
    if (sub=="step" && t.size()>=3) {
      String ssub=t[2]; ssub.toLowerCase();
      if (ssub=="add" && t.size()>=7) {
        int pidx=asLong(t[3],-1); uint16_t dur=asLong(t[4]); uint8_t lvl=asLong(t[5]); bool fade=asLong(t[6])!=0;
        if (pidx<0||pidx>=(int)patterns.size()) {Serial.println("pidx!"); return;}
        patterns[pidx].steps.push_back(LEDStep{dur,lvl,fade}); saveConfig(); Serial.println("OK"); return;
      }
      if (ssub=="set" && t.size()>=8) {
        int pidx=asLong(t[3],-1); int sidx=asLong(t[4],-1); uint16_t dur=asLong(t[5]); uint8_t lvl=asLong(t[6]); bool fade=asLong(t[7])!=0;
        if (pidx<0||pidx>=(int)patterns.size()) {Serial.println("pidx!"); return;}
        if (sidx<0||sidx>=(int)patterns[pidx].steps.size()) {Serial.println("sidx!"); return;}
        patterns[pidx].steps[sidx] = LEDStep{dur,lvl,fade}; saveConfig(); Serial.println("OK"); return;
      }
      if (ssub=="del" && t.size()>=5) {
        int pidx=asLong(t[3],-1); int sidx=asLong(t[4],-1);
        if (pidx<0||pidx>=(int)patterns.size()) {Serial.println("pidx!"); return;}
        if (sidx<0||sidx>=(int)patterns[pidx].steps.size()) {Serial.println("sidx!"); return;}
        patterns[pidx].steps.erase(patterns[pidx].steps.begin()+sidx); saveConfig(); Serial.println("OK"); return;
      }
    }
  }

  // Lichter
  if (cmd=="light" && t.size()>=2) {
    String sub=t[1]; sub.toLowerCase();
    if (sub=="list") { cliLightList(); return; }
    if (sub=="add" && t.size()>=8) {
      LightCfg L;
      L.pin=asLong(t[2]); L.channel=asLong(t[3]); L.patternIndex=asLong(t[4]);
      L.restOverrideMs=asLong(t[5]); L.groupId=asLong(t[6]); L.phase_ms=asLong(t[7]);
      lights.push_back(L); saveConfig(); cliApplyAndMaybeStartWifi(); Serial.println("OK"); return;
    }
    if (sub=="set" && t.size()>=5) {
      int idx=asLong(t[2],-1); String field=t[3]; field.toLowerCase(); if (idx<0||idx>=(int)lights.size()) {Serial.println("idx!"); return;}
      long val=asLong(t[4]);
      auto &L = lights[idx];
      if (field=="pin") L.pin=val;
      else if (field=="ch") L.channel=val;
      else if (field=="pidx") L.patternIndex=val;
      else if (field=="rest") L.restOverrideMs=val;
      else if (field=="gid") L.groupId=val;
      else if (field=="phase") L.phase_ms=val;
      else { Serial.println("Unknown field"); return; }
      saveConfig(); cliApplyAndMaybeStartWifi(); Serial.println("OK"); return;
    }
    if (sub=="del" && t.size()>=3) {
      int idx=asLong(t[2],-1); if (idx<0||idx>=(int)lights.size()) {Serial.println("idx!"); return;}
      lights.erase(lights.begin()+idx); saveConfig(); cliApplyAndMaybeStartWifi(); Serial.println("OK"); return;
    }
  }

  Serial.println("Unknown or bad command. 'help' für Hilfe.");
}

static void cliPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c=='\r') continue;

    // Echo inkl. Backspace
    if (c=='\b' || c==127) {
      if (cliLine.length()>0) {
        cliLine.remove(cliLine.length()-1);
        Serial.print("\b \b"); // visuelles Löschen
      }
      continue;
    }

    Serial.print(c); // Echo

    if (c=='\n') {
      Serial.print("\r"); // CR für hübsche Zeile
      String line = cliLine; cliLine = "";
      line.trim();
      if (line.length()) handleCliLine(line);
      Serial.print("> ");
    } else {
      cliLine += c;
    }
  }
}

/// -------------------- Setup & Loop --------------------
void setup() {
  if (GAMMA_CORRECTION) buildGammaTable();

  Serial.begin(115200);
  delay(200);
  setCpuFrequencyMhz(80); // Brownout-freundlich
  btStop();               // Bluetooth aus

  Serial.println("\nBlaulicht-Controller (Web/CLI + LittleFS)");
  Serial.printf("LEDC-Kanaele gesamt: %u\n", MAX_LEDC_CHANNELS);

  if (!LittleFS.begin(true)) Serial.println("LittleFS init fehlgeschlagen.");

  if (!loadConfig()) {
    Serial.println("Keine config.json gefunden – erstelle Default.");
    makeDefaultConfig();
    saveConfig();
  }

  applyHardware();

  if (wifiEnabled) startWiFi();
  else {
    WiFi.mode(WIFI_OFF);
    Serial.println("WLAN ist laut config.json deaktiviert (wifi_enabled=false).");
  }

  Serial.println("\nCLI bereit. 'help' eingeben.");
  Serial.print("> ");
}

void loop() {
  if (serverRunning) server.handleClient();

  size_t n = min<size_t>(8, lights.size());
  for (size_t i = 0; i < n; ++i) players[i].update();

  cliPoll();
}
