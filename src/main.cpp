#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <esp_bt.h>

/* =========================================================================
   Blaulicht-Controller (ESP32)
   - 8 Ausgänge, PWM (LEDC) 5 kHz / 8-bit, nicht-blockierend
   - Mehrere Patterns (letzter Step = Ruhepause), CRUD via Web-UI (AP/STA) ODER CLI
   - Lights: pin, channel, patternIndex, restOverrideMs, groupId, phase_ms
   - Gruppen an/aus (freeze der Zeit), Phasenverschiebung pro Ausgang
   - WLAN: AP/STA, DHCP in STA (Fallback statisch), mDNS (.local), Captive-Portal im AP
   - Persistenz: /config.json (LittleFS)  +  WLAN-Hauptschalter wifi_enabled
   - JsonDocument-API (ArduinoJson v7)
   ========================================================================= */

// -------------------- Globale WLAN-/Netz-Config --------------------
static WebServer server(80);
static DNSServer dnsServer;

static bool   wifiEnabled   = true;      // Hauptschalter
static String wifiModeCfg   = "ap";      // "ap" oder "sta"
static String apSsidCfg;                 // AP SSID (leer => Default generieren)
static String apPassCfg;                 // AP Passwort (leer => offener AP)
static String staSsidCfg;                // STA SSID
static String staPassCfg;                // STA Passwort
static IPAddress staLastIP(0,0,0,0);
static IPAddress staLastGW(0,0,0,0);
static IPAddress staLastMask(0,0,0,0);

static bool serverRunning = false;

const IPAddress AP_IP  (192,168,4,1);
const IPAddress AP_GW  (192,168,4,1);
const IPAddress AP_MASK(255,255,255,0);

#define AP_SSID_DEFAULT "Blaulicht-AP"
#define AP_PASS_DEFAULT "12345678" // min. 8 Zeichen für WPA2

// -------------------- LEDC-Konfiguration --------------------
constexpr uint32_t LEDC_FREQ_HZ = 5000;
constexpr uint8_t  LEDC_RES_BITS = 8;      // Duty 0..255
constexpr uint8_t  MAX_LEDC_CHANNELS = 16; // ESP32

// -------------------- (Optionale) Gamma-Korrektur --------------------
constexpr bool GAMMA_CORRECTION = false;
uint8_t gammaTable[256];
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

// -------------------- Hilfsfunktionen für Namen/Hostname --------------------
static String uniqueMacSuffix() {
  uint64_t mac = ESP.getEfuseMac();   // 48-bit MAC
  uint32_t l = (uint32_t)(mac & 0xFFFFFF);
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", l);
  return String(buf);
}
static String makeDefaultApSsid() {
  return String("Blaulicht-") + uniqueMacSuffix();
}
static String sanitizeHostname(const String& in) {
  String out; out.reserve(in.length());
  for (size_t i=0;i<in.length();++i) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c>='a' && c<='z') || (c>='0' && c<='9')) out += c;
    else if (c=='-' || c=='_') out += '-';
  }
  if (out.length()==0) out = "blaulicht";
  if (out.length()>32) out.remove(32);
  return out;
}
static String deviceName; // aus apSsidCfg abgeleitet
static void refreshDeviceNameFromApSsid() {
  String base = apSsidCfg.length() ? apSsidCfg : makeDefaultApSsid();
  deviceName = sanitizeHostname(base);
}

// -------------------- Datenmodelle --------------------
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

std::vector<Pattern>  patterns;
std::vector<GroupCfg> groups;
std::vector<LightCfg> lights;

static bool isGroupEnabled(int gid) {
  for (const auto &g : groups) if (g.id == gid) return g.enabled;
  return true; // unbekannte Gruppe => nicht blockieren
}

// -------------------- Pattern-Player --------------------
class PatternPlayer {
public:
  PatternPlayer() = default;
  void attach(uint8_t ledcChannel) { ch = ledcChannel; }
  void bind(int lightIndex) { boundLightIndex = lightIndex; }
  void begin() {
    prev = 0; idx = 0; started = millis(); lastDt = 0; initialized = false;
  }
  void update() {
    if (!isValidBinding()) return;
    const LightCfg &L = lights[boundLightIndex];
    if ((size_t)L.patternIndex >= patterns.size()) { writeLevel(0); return; }
    const Pattern &P = patterns[L.patternIndex];
    if (P.steps.empty()) { writeLevel(0); return; }

    if (!initialized) { initWithPhase(L, P); initialized = true; }

    if (!isGroupEnabled(L.groupId)) {
      writeLevel(0);
      started = millis() - lastDt; // freeze
      return;
    }
    const LEDStep &s = P.steps[idx];
    const uint16_t dur = effectiveDuration(L, P, idx, s.duration_ms);
    lastDt = millis() - started;
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
  bool isValidBinding() const { return boundLightIndex >= 0 && (size_t)boundLightIndex < lights.size(); }
  static size_t restIndexOf(const Pattern &P) { return P.steps.empty() ? 0 : (P.steps.size() - 1); }
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
    prev = endLevelOfStep(P, idx == 0 ? P.steps.size()-1 : idx-1);
    target = s.level;
    if (!s.fade) writeLevel(target);
    started = millis() - (phase - acc);
    lastDt  = (phase - acc);
  }
  static uint8_t endLevelOfStep(const Pattern &P, size_t i) {
    if (i >= P.steps.size()) return 0;
    return P.steps[i].level;
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

// -------------------- Persistenz --------------------
static bool saveConfig() {
  File f = LittleFS.open("/config.json", FILE_WRITE);
  if (!f) return false;

  JsonDocument doc;
  doc["wifi_enabled"] = wifiEnabled;
  doc["wifi"]["mode"] = wifiModeCfg;
  doc["wifi"]["ap"]["ssid"] = apSsidCfg;
  doc["wifi"]["ap"]["password"] = apPassCfg;
  doc["wifi"]["sta"]["ssid"] = staSsidCfg;
  doc["wifi"]["sta"]["password"] = staPassCfg;
  doc["wifi"]["sta"]["last_ip"]   = staLastIP.toString();
  doc["wifi"]["sta"]["last_gw"]   = staLastGW.toString();
  doc["wifi"]["sta"]["last_mask"] = staLastMask.toString();

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

  auto n = serializeJsonPretty(doc, f);
  f.close();
  return n > 0;
}

static bool parseIP(const String& s, IPAddress& out) {
  return out.fromString(s);
}

static bool loadConfig() {
  if (!LittleFS.exists("/config.json")) return false;
  File f = LittleFS.open("/config.json", FILE_READ);
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  wifiEnabled = (bool)(doc["wifi_enabled"] | true);
  wifiModeCfg = (doc["wifi"]["mode"] | "ap");
  apSsidCfg   = (doc["wifi"]["ap"]["ssid"] | AP_SSID_DEFAULT);
  apPassCfg   = (doc["wifi"]["ap"]["password"] | AP_PASS_DEFAULT);
  staSsidCfg  = (doc["wifi"]["sta"]["ssid"] | "");
  staPassCfg  = (doc["wifi"]["sta"]["password"] | "");

  // Letzte STA-IP-Werte
  IPAddress tmp;
  if (doc["wifi"]["sta"]["last_ip"].is<const char*>() && parseIP(doc["wifi"]["sta"]["last_ip"].as<const char*>(), tmp)) staLastIP = tmp;
  if (doc["wifi"]["sta"]["last_gw"].is<const char*>() && parseIP(doc["wifi"]["sta"]["last_gw"].as<const char*>(), tmp)) staLastGW = tmp;
  if (doc["wifi"]["sta"]["last_mask"].is<const char*>() && parseIP(doc["wifi"]["sta"]["last_mask"].as<const char*>(), tmp)) staLastMask = tmp;

  // Alt/leer -> eindeutige SSID erzeugen
  if (apSsidCfg.length() == 0 || apSsidCfg == AP_SSID_DEFAULT) {
    apSsidCfg = makeDefaultApSsid();
    saveConfig();
  }
  refreshDeviceNameFromApSsid();

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
  apSsidCfg   = makeDefaultApSsid();
  apPassCfg   = AP_PASS_DEFAULT;
  staSsidCfg  = "";
  staPassCfg  = "";
  refreshDeviceNameFromApSsid();

  patterns.clear(); groups.clear(); lights.clear();

  // Pattern 0 (dein Blaulicht)
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

// Hardware anwenden (LEDC setup/attach) + Player binden & (re)starten
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

// -------------------- Static Files --------------------
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

// -------------------- Captive-Portal (AP) --------------------
static bool isAPModeActive() { return wifiEnabled && wifiModeCfg == "ap"; }

// Redirecte alle fremden Hostnamen im AP auf unsere Portal-IP
static bool handleCaptivePortalRedirect() {
  if (!isAPModeActive()) return false;
  String host = server.hostHeader();
  if (!host.length()) return false;

  String apIpStr = AP_IP.toString();
  // Wenn der Hostname NICHT unsere AP-IP ist -> Redirect
  if (host != apIpStr) {
    String url = "http://" + apIpStr + "/";
    server.sendHeader("Location", url, true);
    server.send(302, "text/plain", "Redirect to captive portal");
    return true;
  }
  return false;
}

// Bekannte Captive-Check URLs (Apple/Android/Windows)
static void registerCaptiveHelpers() {
  server.on("/hotspot-detect.html", HTTP_GET, [](){ // Apple
    if (handleCaptivePortalRedirect()) return;
    server.send(200, "text/html", "<html><meta http-equiv='refresh' content='0;url=/'></html>");
  });
  server.on("/generate_204", HTTP_GET, [](){        // Android
    if (handleCaptivePortalRedirect()) return;
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/ncsi.txt", HTTP_GET, [](){            // Windows
    if (handleCaptivePortalRedirect()) return;
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Microsoft NCSI");
  });
}

// -------------------- REST Endpoints --------------------
static void sendJSON(const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

static void handleGetConfig() {
  JsonDocument doc;
  doc["wifi_enabled"] = wifiEnabled;
  doc["wifi"]["mode"] = wifiModeCfg;
  doc["wifi"]["ap"]["ssid"] = apSsidCfg;
  doc["wifi"]["ap"]["password"] = apPassCfg;
  doc["wifi"]["sta"]["ssid"] = staSsidCfg;
  doc["wifi"]["sta"]["password"] = staPassCfg;
  doc["wifi"]["device_name"] = deviceName;

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
  JsonArray jGroups = doc["groups"].to<JsonArray>();
  for (const auto &g : groups) {
    JsonObject jg = jGroups.add<JsonObject>();
    jg["id"] = g.id; jg["name"] = g.name; jg["enabled"] = g.enabled;
  }
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

static void handleSysInfo() {
  JsonDocument doc;
  doc["max_ledc_channels"] = MAX_LEDC_CHANNELS;
  doc["ledc_freq_hz"]      = LEDC_FREQ_HZ;
  doc["ledc_res_bits"]     = LEDC_RES_BITS;
  doc["num_lights"]        = (uint32_t)lights.size();
  doc["ap_ip"]             = AP_IP.toString();
  doc["device_name"]       = deviceName;
  String out; serializeJson(doc, out);
  sendJSON(out);
}

static bool waitForStaConnect(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(100);
  }
  return false;
}

static void startServerRoutes(); // Vorwärtsdeklaration
static void stopWiFi();          // Vorwärtsdeklaration
static void startWiFi();         // Vorwärtsdeklaration

// /api/wifi: GET -> Status; PUT -> Einstellungen setzen (inkl. Mode/SSID/PW), optional Neustart
static void handleWifiEndpoint() {
  if (server.method() == HTTP_GET) {
    JsonDocument d;
    d["enabled"]      = wifiEnabled;
    d["mode"]         = wifiModeCfg;         // "ap"|"sta"
    d["ap"]["ssid"]   = apSsidCfg;
    d["ap"]["password"]= apPassCfg;
    d["sta"]["ssid"]  = staSsidCfg;
    d["sta"]["password"]= staPassCfg;
    d["device_name"]  = deviceName;
    d["sta"]["ip"]    = WiFi.localIP().toString();
    d["sta"]["gw"]    = WiFi.gatewayIP().toString();
    d["sta"]["mask"]  = WiFi.subnetMask().toString();
    String out; serializeJson(d, out);
    sendJSON(out);
    return;
  }

  if (server.method() == HTTP_PUT || server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "Missing body"); return; }
    JsonDocument d;
    auto err = deserializeJson(d, server.arg("plain"));
    if (err) { server.send(400, "text/plain", String("JSON error: ")+err.c_str()); return; }

    if (d["enabled"].is<bool>()) wifiEnabled = (bool)d["enabled"];
    if (d["mode"].is<const char*>()) {
      String m = d["mode"].as<const char*>(); m.toLowerCase();
      if (m=="ap"||m=="sta") wifiModeCfg = m;
    }
    if (d["ap"]["ssid"].is<const char*>())      apSsidCfg = d["ap"]["ssid"].as<const char*>();
    if (d["ap"]["password"].is<const char*>())  apPassCfg = d["ap"]["password"].as<const char*>();
    if (d["sta"]["ssid"].is<const char*>())     staSsidCfg = d["sta"]["ssid"].as<const char*>();
    if (d["sta"]["password"].is<const char*>()) staPassCfg = d["sta"]["password"].as<const char*>();

    // AP-SSID leer/legacy -> eindeutige setzen
    if (apSsidCfg.length()==0 || apSsidCfg==AP_SSID_DEFAULT) apSsidCfg = makeDefaultApSsid();
    refreshDeviceNameFromApSsid();

    bool ok = saveConfig();
    if (!ok) { server.send(500, "text/plain", "save failed"); return; }

    // Optionaler Sofortwechsel
    if (d["apply_now"].is<bool>() && (bool)d["apply_now"]) {
      stopWiFi();
      startWiFi();
    }

    server.send(200, "text/plain", "OK");
    return;
  }

  server.send(405, "text/plain", "Method Not Allowed");
}

static void handlePutConfig() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Missing body"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) { server.send(400, "text/plain", String("JSON error: ")+err.c_str()); return; }

  if (!doc["patterns"].is<JsonArray>() || !doc["lights"].is<JsonArray>() || !doc["groups"].is<JsonArray>()) {
    server.send(400,"text/plain","Invalid JSON structure"); return;
  }
  // RAM übernehmen
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

  applyHardware();
  bool ok = saveConfig();
  server.send(ok?200:500, "text/plain", ok?"OK (saved)":"ERROR (save failed)");
}

// -------------------- WLAN Start/Stop + Server Start/Stop --------------------
static void startServerRoutes() {
  // Captive Portal Helfer (AP)
  registerCaptiveHelpers();

  server.on("/", HTTP_GET, [](){
    if (isAPModeActive() && handleCaptivePortalRedirect()) return;
    if (!handleFileRead("/index.html")) server.send(404,"text/plain","index.html not found");
  });

  server.on("/api/config",  HTTP_GET, handleGetConfig);
  server.on("/api/config",  HTTP_PUT, handlePutConfig);
  server.on("/api/sysinfo", HTTP_GET, handleSysInfo);
  server.on("/api/wifi",    HTTP_ANY, handleWifiEndpoint);

  // Statische Dateien & Captive Redirect
  server.onNotFound([](){
    if (isAPModeActive() && handleCaptivePortalRedirect()) return;
    String path = server.uri();
    if (handleFileRead(path)) return;
    server.send(404, "text/plain", "Not found");
  });
}

static void stopWiFi() {
  if (serverRunning) {
    server.stop();
    serverRunning = false;
  }
  MDNS.end();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WLAN/Server gestoppt.");
}

static bool waitDnsReady(uint32_t ms=50) {
  // kleine Verzögerung, damit DNSServer sauber starten kann
  delay(ms);
  return true;
}

static bool waitApReady(uint32_t ms=100) { delay(ms); return true; }

static void startWiFi() {
  // Sicherheits-Stopp (idempotent)
  if (serverRunning) {
    server.stop();
    serverRunning = false;
  }
  MDNS.end();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(50);

  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_5dBm); // moderat reduziert

  if (wifiModeCfg == "sta") {
    Serial.println("WLAN: wechsle in STA-Modus, versuche DHCP…");
    WiFi.mode(WIFI_STA);

    // Hostname aus AP-SSID ableiten
    refreshDeviceNameFromApSsid();
    WiFi.setHostname(deviceName.c_str());

    // DHCP aktivieren und verbinden
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.begin(staSsidCfg.c_str(), staPassCfg.c_str());

    bool ok = waitForStaConnect(10000); // 10 s Timeout
    if (!ok) {
      Serial.println("DHCP/Connect fehlgeschlagen – versuche statisch mit letzten Werten…");
      if (staLastIP != IPAddress(0,0,0,0) && staLastGW != IPAddress(0,0,0,0) && staLastMask != IPAddress(0,0,0,0)) {
        WiFi.disconnect();
        delay(100);
        WiFi.config(staLastIP, staLastGW, staLastMask);
        WiFi.begin(staSsidCfg.c_str(), staPassCfg.c_str());
        ok = waitForStaConnect(6000);
      }
    }

    if (ok) {
      Serial.printf("STA verbunden: IP=%s, GW=%s, MASK=%s, RSSI=%d dBm\n",
        WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str(),
        WiFi.RSSI());

      // letzte erfolgreichen Werte speichern
      staLastIP   = WiFi.localIP();
      staLastGW   = WiFi.gatewayIP();
      staLastMask = WiFi.subnetMask();
      saveConfig();

      // mDNS aktivieren -> http://<deviceName>.local/
      if (MDNS.begin(deviceName.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS aktiv: http://%s.local/\n", deviceName.c_str());
      } else {
        Serial.println("mDNS Start fehlgeschlagen.");
      }
    } else {
      Serial.println("STA-Verbindung fehlgeschlagen.");
    }

    startServerRoutes();
    server.begin();
    serverRunning = true;

  } else {
    // AP-Modus
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);

    // Fallback: leere/legacy SSID -> eindeutige SSID erzeugen + persistieren
    String ssid = apSsidCfg.length() ? apSsidCfg : makeDefaultApSsid();
    if (apSsidCfg != ssid) { apSsidCfg = ssid; saveConfig(); }
    refreshDeviceNameFromApSsid();

    String pass = apPassCfg; // darf leer sein (offener AP)
    bool apok = false;
    if (pass.length() == 0) {
      apok = WiFi.softAP(ssid.c_str()); // offener AP
    } else if (pass.length() >= 8) {
      apok = WiFi.softAP(ssid.c_str(), pass.c_str()); // WPA2
    } else {
      Serial.println("Warnung: Passwort < 8 Zeichen -> starte offenen AP.");
      apok = WiFi.softAP(ssid.c_str());
    }

    waitApReady();

    // DNS-Captive: alle Domains auf AP-IP umbiegen
    dnsServer.setTTL(60);
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", AP_IP);
    waitDnsReady();

    Serial.printf("AP gestartet: SSID='%s' (%s), ok=%d\n",
                  ssid.c_str(), WiFi.softAPIP().toString().c_str(), apok);

    // Optional mDNS auch im AP (nicht jedes OS nutzt das im AP, schadet aber nicht)
    if (MDNS.begin(deviceName.c_str())) {
      MDNS.addService("http", "tcp", 80);
    }

    startServerRoutes();
    server.begin();
    serverRunning = true;
  }
}

// -------------------- CLI (Serielle Konsole) --------------------
static String cliLine;

static void cliPrintPrompt() {
  Serial.print("\r\n> ");
  Serial.flush();
}
static long asLong(const String& s, long def=0){ char* e=nullptr; long v=strtol(s.c_str(), &e, 10); return e && *e==0 ? v : def; }

static void cliStatus() {
  Serial.printf("wifi_enabled: %s, mode: %s, serverRunning: %d, device: %s\n",
                wifiEnabled?"true":"false", wifiModeCfg.c_str(), (int)serverRunning, deviceName.c_str());
  Serial.printf("AP SSID='%s' PW='%s'\n", apSsidCfg.c_str(), apPassCfg.c_str());
  Serial.printf("STA SSID='%s' IP=%s\n", staSsidCfg.c_str(), WiFi.localIP().toString().c_str());
  Serial.printf("patterns: %u, groups: %u, lights: %u\n",
                (unsigned)patterns.size(), (unsigned)groups.size(), (unsigned)lights.size());
}

static void cliGroupList() {
  for (auto &g: groups) Serial.printf("id=%d name='%s' enabled=%d\n", g.id, g.name.c_str(), g.enabled);
}
static void cliPatList() {
  for (size_t i=0;i<patterns.size();++i) {
    Serial.printf("[%u] '%s' steps=%u\n", (unsigned)i, patterns[i].name.c_str(), (unsigned)patterns[i].steps.size());
  }
}
static void cliLightList() {
  for (size_t i=0;i<lights.size();++i) {
    auto &L = lights[i];
    Serial.printf("[%u] pin=%u ch=%u pidx=%u rest=%d gid=%d phase=%d\n",
      (unsigned)i, L.pin, L.channel, L.patternIndex, L.restOverrideMs, L.groupId, L.phase_ms);
  }
}

static bool cliWifiDhcp(uint32_t timeoutMs = 10000) {
  if (!wifiEnabled) { Serial.println("WiFi ist deaktiviert. Mit 'wifi on' einschalten."); return false; }
  if (wifiModeCfg != "sta") { Serial.println("Modus ist nicht 'sta'. Mit 'wifi mode sta' umschalten."); return false; }
  if (staSsidCfg.isEmpty()) { Serial.println("STA-SSID leer. Mit 'wifi set sta <ssid> <pass>' setzen."); return false; }

  Serial.printf("DHCP: versuche neue Lease im STA-Modus (SSID='%s')...\n", staSsidCfg.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(staSsidCfg.c_str(), staPassCfg.c_str());

  bool ok = waitForStaConnect(timeoutMs);
  if (!ok) { Serial.println("DHCP/Connect fehlgeschlagen."); return false; }

  staLastIP   = WiFi.localIP();
  staLastGW   = WiFi.gatewayIP();
  staLastMask = WiFi.subnetMask();
  saveConfig();

  Serial.printf("DHCP OK: IP=%s, GW=%s, MASK=%s, RSSI=%d dBm\n",
    staLastIP.toString().c_str(),
    staLastGW.toString().c_str(),
    staLastMask.toString().c_str(),
    WiFi.RSSI());
  return true;
}

static void cliHelp() {
  Serial.println(F(
    "\n=== CLI Befehle ===\n"
    "help                              : diese Hilfe\n"
    "status                            : Kurzstatus anzeigen\n"
    "save                              : Konfiguration speichern\n"
    "load                              : Konfiguration neu laden\n"
    "reboot                            : Neustart\n"
    "wifi on|off                       : WLAN/Webserver ein-/ausschalten (persistent)\n"
    "wifi mode ap|sta                  : WLAN-Modus wechseln\n"
    "wifi set ap  <ssid> <password>    : AP-SSID/PW setzen (PW leer => offener AP)\n"
    "wifi set sta <ssid> <password>    : STA-SSID/PW setzen\n"
    "wifi dhcp [timeout_ms]            : im STA-Modus neue DHCP-Adresse beziehen\n"
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

static void cliApplyAndMaybeStartWifi() {
  applyHardware();
  if (wifiEnabled && !serverRunning) startWiFi();
  if (!wifiEnabled && serverRunning) stopWiFi();
}

static void handleCliLine(const String& line) {
  // Tokenize
  std::vector<String> t; String cur; 
  for (size_t i=0;i<line.length();++i){ char c=line[i]; if (c==' '||c=='\t'){ if(cur.length()) {t.push_back(cur); cur="";} } else cur+=c; }
  if(cur.length()) t.push_back(cur);
  if (t.empty()) return;
  String cmd = t[0]; cmd.toLowerCase();

  if (cmd=="help") { cliHelp(); return; }
  if (cmd=="status") { cliStatus(); return; }
  if (cmd=="save") { bool ok=saveConfig(); Serial.println(ok?"OK saved":"ERR save"); return; }
  if (cmd=="load") { bool ok=loadConfig(); Serial.println(ok?"OK loaded":"ERR load"); cliApplyAndMaybeStartWifi(); return; }
  if (cmd=="reboot") { Serial.println("Rebooting..."); delay(100); ESP.restart(); }

  // wifi commands
  if (cmd=="wifi") {
    if (t.size()>=2) {
      String sub = t[1]; sub.toLowerCase();

      if (sub=="on")  { wifiEnabled=true; saveConfig(); startWiFi(); Serial.println("WiFi ON."); return; }
      if (sub=="off") { wifiEnabled=false; saveConfig(); stopWiFi(); Serial.println("WiFi OFF."); return; }

      if (sub=="mode" && t.size()>=3) {
        String m = t[2]; m.toLowerCase();
        if (m=="ap" || m=="sta") { wifiModeCfg=m; saveConfig(); stopWiFi(); startWiFi(); Serial.println("Mode updated."); }
        else Serial.println("Usage: wifi mode ap|sta");
        return;
      }
      if (sub=="set" && t.size()>=3) {
        String which=t[2]; which.toLowerCase();
        if (which=="ap" && t.size()>=5) {
          apSsidCfg=t[3]; apPassCfg=t[4];
          if (apSsidCfg.length()==0) apSsidCfg = makeDefaultApSsid();
          refreshDeviceNameFromApSsid();
          saveConfig(); stopWiFi(); startWiFi(); Serial.println("AP credentials updated."); return;
        }
        if (which=="sta" && t.size()>=5) {
          staSsidCfg=t[3]; staPassCfg=t[4];
          saveConfig(); stopWiFi(); startWiFi(); Serial.println("STA credentials updated."); return;
        }
        Serial.println("Usage: wifi set ap <ssid> <pass>  |  wifi set sta <ssid> <pass>");
        return;
      }
      if (sub=="dhcp") {
        uint32_t timeoutMs = 10000;
        if (t.size()>=3) timeoutMs = (uint32_t)asLong(t[2], 10000);
        bool ok = cliWifiDhcp(timeoutMs);
        Serial.println(ok ? "DHCP: OK" : "DHCP: FEHLER");
        return;
      }
    }
    Serial.println("wifi on|off | wifi mode ap|sta | wifi set ap <ssid> <pass> | wifi set sta <ssid> <pass> | wifi dhcp [ms]");
    return;
  }

  // group commands
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

  // pattern commands
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

  // light commands
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

    // Normalize Zeilenende (CR oder LF)
    if (c == '\r' || c == '\n') {
      Serial.print("\r\n");
      String line = cliLine; cliLine = "";
      line.trim();
      if (line.length()) handleCliLine(line);
      cliPrintPrompt();
      continue;
    }

    // Backspace (BS=8, DEL=127)
    if (c == 8 || c == 127) {
      if (cliLine.length() > 0) {
        cliLine.remove(cliLine.length() - 1);
        Serial.print("\b \b");
      }
      continue;
    }

    // Druckbare ASCII-Zeichen
    if (c >= 32 && c <= 126) {
      cliLine += c;
      Serial.write(c); // Echo
    }
  }
}

// -------------------- Setup & Loop --------------------
void setup() {
  if (GAMMA_CORRECTION) buildGammaTable();

  Serial.begin(115200);
  delay(200);
  setCpuFrequencyMhz(80);   // weniger Spitzenlast
  btStop();                 // Bluetooth aus

  Serial.println("\nBlaulicht-Controller (Web/CLI + LittleFS + Captive Portal)");
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
  cliPrintPrompt();
}

void loop() {
  if (serverRunning) {
    if (isAPModeActive()) dnsServer.processNextRequest(); // Captive-DNS
    server.handleClient();
  }

  size_t n = min<size_t>(8, lights.size());
  for (size_t i = 0; i < n; ++i) players[i].update();

  cliPoll();
}
