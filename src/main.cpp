#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <esp_bt.h>
#include <math.h>
#include <vector>

/* =========================================================================
   Blaulicht-Controller (ESP32)
   - 8 Ausgänge, PWM (LEDC) 5 kHz / 8-bit, nicht-blockierend
   - Mehrere Patterns (letzter Step = Ruhepause), CRUD via Web-UI (AP/STA) ODER CLI
   - Lights: pin, channel, patternIndex, restOverrideMs, groupId, phase_ms
   - Gruppen an/aus (freeze der Zeit), Phasenverschiebung pro Ausgang
   - Persistenz: /config.json (LittleFS) + WLAN-Hauptschalter wifi_enabled
   - JsonDocument-API (ArduinoJson v7)

   Neu:
   - WLAN-Modus „sta“: DHCP beziehen; bei Misserfolg statisch mit zuletzt bekannten Werten verbinden
   - STA-SSID/PW und letzte IP/GW/Mask werden persistiert
   ========================================================================= */

/// -------------------- AP-/Netzwerk-Setup --------------------
const char *AP_SSID_DEFAULT = "Blaulicht-AP";
const char *AP_PASS_DEFAULT = "12345678"; // >=8 Zeichen für WPA2
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GW(192, 168, 4, 1);
const IPAddress AP_MASK(255, 255, 255, 0);

static WebServer server(80);
static bool wifiEnabled = true;    // aus config.json
static bool serverRunning = false; // interner Status

// Konfigurierbare WLAN-Parameter (werden persistiert)
static String wifiModeCfg = "ap"; // "ap" oder "sta"

// AP
static String apSsidCfg = AP_SSID_DEFAULT; // leer => Fallback auf Default
static String apPassCfg = AP_PASS_DEFAULT; // leer => offener AP

// STA
static String staSsidCfg = "";            // SSID des WLANs
static String staPassCfg = "";            // Passwort
static IPAddress staLastIP(0, 0, 0, 0);   // letzte erfolgreiche STA-IP
static IPAddress staLastGW(0, 0, 0, 0);   // letztes Gateway
static IPAddress staLastMask(0, 0, 0, 0); // letzte Subnetzmaske

/// -------------------- LEDC-Konfiguration --------------------
constexpr uint32_t LEDC_FREQ_HZ = 5000;
constexpr uint8_t LEDC_RES_BITS = 8;      // Duty 0..255
constexpr uint8_t MAX_LEDC_CHANNELS = 16; // ESP32

/// -------------------- (Optionale) Gamma-Korrektur --------------------
constexpr bool GAMMA_CORRECTION = false;
uint8_t gammaTable[256];
static inline uint8_t applyGamma(uint8_t v)
{
  if (!GAMMA_CORRECTION)
    return v;
  return gammaTable[v];
}
static void buildGammaTable()
{
  for (int i = 0; i < 256; ++i)
  {
    float norm = i / 255.0f;
    float gamma = powf(norm, 2.2f);
    gammaTable[i] = (uint8_t)roundf(gamma * 255.0f);
  }
}

/// -------------------- Datenmodelle --------------------
struct LEDStep
{
  uint16_t duration_ms; // Länge des Schritts
  uint8_t level;        // Zielhelligkeit (0..255)
  bool fade;            // weicher Übergang über die Dauer
};

struct Pattern
{
  String name;
  std::vector<LEDStep> steps; // letzter Step = Ruhepause
};

struct GroupCfg
{
  int id;
  String name;
  bool enabled;
};

struct LightCfg
{
  uint8_t pin;
  uint8_t channel;      // 0..15
  uint8_t patternIndex; // Index in patterns[]
  int restOverrideMs;   // -1 => Pattern-Pause nutzen; sonst Dauer für letzten Step
  int groupId;
  int phase_ms; // Phasenverschiebung relativ zum Pattern-Zyklus (>=0)
};

std::vector<Pattern> patterns;
std::vector<GroupCfg> groups;
std::vector<LightCfg> lights;

/// -------------------- Helpers --------------------
static bool isGroupEnabled(int gid)
{
  for (const auto &g : groups)
    if (g.id == gid)
      return g.enabled;
  return true; // unbekannte Gruppe => nicht blockieren
}

static String ipToStr(const IPAddress &ip)
{
  return ip.toString();
}
static bool strToIP(const char *s, IPAddress &out)
{
  if (!s || !*s)
    return false;
  return out.fromString(s);
}

/// -------------------- Player für EINEN Ausgang --------------------
class PatternPlayer
{
public:
  PatternPlayer() = default;

  void attach(uint8_t ledcChannel) { ch = ledcChannel; }
  void bind(int lightIndex) { boundLightIndex = lightIndex; }

  void begin()
  {
    prev = 0;
    idx = 0;
    started = millis();
    lastDt = 0;
    initialized = false;
  }

  void update()
  {
    if (!isValidBinding())
      return;

    const LightCfg &L = lights[boundLightIndex];
    if ((size_t)L.patternIndex >= patterns.size())
    {
      writeLevel(0);
      return;
    }
    const Pattern &P = patterns[L.patternIndex];
    if (P.steps.empty())
    {
      writeLevel(0);
      return;
    }

    if (!initialized)
    {
      initWithPhase(L, P);
      initialized = true;
    }

    if (!isGroupEnabled(L.groupId))
    {
      writeLevel(0);
      started = millis() - lastDt; // freeze
      return;
    }

    const LEDStep &s = P.steps[idx];
    const uint16_t dur = effectiveDuration(L, P, idx, s.duration_ms);

    const uint32_t now = millis();
    lastDt = now - started;

    if (lastDt >= dur)
    {
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
  int boundLightIndex = -1;

  bool initialized = false;
  size_t idx = 0;
  uint32_t started = 0;
  uint32_t lastDt = 0;
  uint8_t prev = 0;
  uint8_t target = 0;

  inline void writeLevel(uint8_t lvl) { ledcWrite(ch, applyGamma(lvl)); }

  bool isValidBinding() const
  {
    return boundLightIndex >= 0 && (size_t)boundLightIndex < lights.size();
  }

  static size_t restIndexOf(const Pattern &P)
  {
    return P.steps.empty() ? 0 : (P.steps.size() - 1);
  }

  static uint16_t effectiveDuration(const LightCfg &L, const Pattern &P, size_t i, uint16_t nominal)
  {
    if (i == restIndexOf(P) && L.restOverrideMs >= 0)
      return (uint16_t)L.restOverrideMs;
    return nominal;
  }

  static uint32_t patternTotalMs(const LightCfg &L, const Pattern &P)
  {
    uint32_t total = 0;
    for (size_t i = 0; i < P.steps.size(); ++i)
      total += effectiveDuration(L, P, i, P.steps[i].duration_ms);
    return total;
  }

  void initWithPhase(const LightCfg &L, const Pattern &P)
  {
    uint32_t total = patternTotalMs(L, P);
    uint32_t phase = (L.phase_ms >= 0 && total > 0) ? (uint32_t)L.phase_ms % total : 0;

    size_t step = 0;
    uint32_t acc = 0;
    while (step < P.steps.size())
    {
      uint16_t dur = effectiveDuration(L, P, step, P.steps[step].duration_ms);
      if (phase < acc + dur)
        break;
      acc += dur;
      ++step;
    }
    if (step >= P.steps.size())
      step = 0;

    idx = step;
    const LEDStep &s = P.steps[idx];
    prev = endLevelOfStep(P, idx == 0 ? P.steps.size() - 1 : idx - 1);
    target = s.level;
    if (!s.fade)
      writeLevel(target);

    started = millis() - (phase - acc);
    lastDt = (phase - acc);
  }

  static uint8_t endLevelOfStep(const Pattern &P, size_t i)
  {
    if (i >= P.steps.size())
      return 0;
    return P.steps[i].level;
  }

  void nextStep(const LightCfg &L, const Pattern &P)
  {
    prev = target;
    idx = (idx + 1) % P.steps.size();
    const LEDStep &s = P.steps[idx];
    target = s.level;
    if (!s.fade)
      writeLevel(target);
    started = millis();
    lastDt = 0;
  }
};

static PatternPlayer players[8];

/// -------------------- Persistenz (JsonDocument-API v7) --------------------
static bool saveConfig()
{
  File f = LittleFS.open("/config.json", FILE_WRITE);
  if (!f)
    return false;

  JsonDocument doc;
  doc["wifi_enabled"] = wifiEnabled;

  // WiFi Settings
  JsonObject jw = doc["wifi"].to<JsonObject>();
  jw["mode"] = wifiModeCfg;

  JsonObject jap = jw["ap"].to<JsonObject>();
  jap["ssid"] = apSsidCfg;
  jap["password"] = apPassCfg;

  JsonObject jsta = jw["sta"].to<JsonObject>();
  jsta["ssid"] = staSsidCfg;
  jsta["password"] = staPassCfg;
  jsta["last_ip"] = ipToStr(staLastIP);
  jsta["last_gw"] = ipToStr(staLastGW);
  jsta["last_mask"] = ipToStr(staLastMask);

  // patterns
  JsonArray jPatterns = doc["patterns"].to<JsonArray>();
  for (const auto &p : patterns)
  {
    JsonObject jp = jPatterns.add<JsonObject>();
    jp["name"] = p.name;
    JsonArray jSteps = jp["steps"].to<JsonArray>();
    for (const auto &st : p.steps)
    {
      JsonObject js = jSteps.add<JsonObject>();
      js["duration_ms"] = st.duration_ms;
      js["level"] = st.level;
      js["fade"] = st.fade;
    }
  }
  // groups
  JsonArray jGroups = doc["groups"].to<JsonArray>();
  for (const auto &g : groups)
  {
    JsonObject jg = jGroups.add<JsonObject>();
    jg["id"] = g.id;
    jg["name"] = g.name;
    jg["enabled"] = g.enabled;
  }
  // lights
  JsonArray jLights = doc["lights"].to<JsonArray>();
  for (const auto &L : lights)
  {
    JsonObject jl = jLights.add<JsonObject>();
    jl["pin"] = L.pin;
    jl["channel"] = L.channel;
    jl["patternIndex"] = L.patternIndex;
    jl["restOverrideMs"] = L.restOverrideMs;
    jl["groupId"] = L.groupId;
    jl["phase_ms"] = L.phase_ms;
  }

  auto n = serializeJsonPretty(doc, f);
  f.close();
  return n > 0;
}

static bool loadConfig()
{
  if (!LittleFS.exists("/config.json"))
    return false;
  File f = LittleFS.open("/config.json", FILE_READ);
  if (!f)
    return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err)
    return false;

  wifiEnabled = (bool)(doc["wifi_enabled"] | true);

  // WiFi/AP/STA Settings laden
  wifiModeCfg = String((const char *)(doc["wifi"]["mode"] | "ap"));

  apSsidCfg = String((const char *)(doc["wifi"]["ap"]["ssid"] | AP_SSID_DEFAULT));
  apPassCfg = String((const char *)(doc["wifi"]["ap"]["password"] | AP_PASS_DEFAULT));

  staSsidCfg = String((const char *)(doc["wifi"]["sta"]["ssid"] | ""));
  staPassCfg = String((const char *)(doc["wifi"]["sta"]["password"] | ""));

  // letzte STA-IP/GW/Mask laden (Strings -> IPAddress)
  {
    IPAddress ip(0, 0, 0, 0), gw(0, 0, 0, 0), mask(0, 0, 0, 0);
    const char *sIP = doc["wifi"]["sta"]["last_ip"] | "";
    const char *sGW = doc["wifi"]["sta"]["last_gw"] | "";
    const char *sMask = doc["wifi"]["sta"]["last_mask"] | "";
    if (strToIP(sIP, ip))
      staLastIP = ip;
    if (strToIP(sGW, gw))
      staLastGW = gw;
    if (strToIP(sMask, mask))
      staLastMask = mask;
  }

  patterns.clear();
  groups.clear();
  lights.clear();

  for (JsonObject jp : doc["patterns"].as<JsonArray>())
  {
    Pattern p;
    p.name = jp["name"].as<const char *>();
    for (JsonObject js : jp["steps"].as<JsonArray>())
    {
      LEDStep st;
      st.duration_ms = (uint16_t)(js["duration_ms"] | 0);
      st.level = (uint8_t)(js["level"] | 0);
      st.fade = (bool)(js["fade"] | false);
      p.steps.push_back(st);
    }
    patterns.push_back(std::move(p));
  }

  for (JsonObject jg : doc["groups"].as<JsonArray>())
  {
    GroupCfg g;
    g.id = (int)(jg["id"] | 0);
    g.name = jg["name"].as<const char *>();
    g.enabled = (bool)(jg["enabled"] | true);
    groups.push_back(std::move(g));
  }

  for (JsonObject jl : doc["lights"].as<JsonArray>())
  {
    LightCfg L;
    L.pin = (uint8_t)(jl["pin"] | 255);
    L.channel = (uint8_t)(jl["channel"] | 0);
    L.patternIndex = (uint8_t)(jl["patternIndex"] | 0);
    L.restOverrideMs = (int)(jl["restOverrideMs"] | -1);
    L.groupId = (int)(jl["groupId"] | 0);
    L.phase_ms = (int)(jl["phase_ms"] | 0);
    lights.push_back(std::move(L));
  }
  return true;
}

static void makeDefaultConfig()
{
  wifiEnabled = true;
  wifiModeCfg = "ap";
  apSsidCfg = AP_SSID_DEFAULT;
  apPassCfg = AP_PASS_DEFAULT;

  staSsidCfg = "";
  staPassCfg = "";
  staLastIP = IPAddress(0, 0, 0, 0);
  staLastGW = IPAddress(0, 0, 0, 0);
  staLastMask = IPAddress(0, 0, 0, 0);

  patterns.clear();
  groups.clear();
  lights.clear();

  // Pattern 0 (dein Blaulicht)
  {
    Pattern p;
    p.name = "Pat0-Blaulicht";
    LEDStep s[] = {
        {30, 255, false}, {30, 0, false}, {30, 255, false}, {30, 0, false}, {150, 255, false}, {30, 0, true}, {30, 255, false}, {0, 0, false}, {250, 0, false}};
    p.steps.assign(std::begin(s), std::end(s));
    patterns.push_back(std::move(p));
  }
  // Pattern 1
  {
    Pattern p;
    p.name = "Pat1-DoppelStrobeWeich";
    LEDStep s[] = {{60, 255, true}, {40, 0, true}, {60, 255, true}, {40, 0, true}, {180, 0, false}};
    p.steps.assign(std::begin(s), std::end(s));
    patterns.push_back(std::move(p));
  }

  groups.push_back({0, "Front", true});
  groups.push_back({1, "Heck", true});

  lights = {
      LightCfg{21, 0, 0, -1, 0, 0},
      LightCfg{22, 1, 0, 400, 0, 50},
      LightCfg{18, 2, 1, 250, 1, 100},
      LightCfg{19, 3, 0, 600, 1, 150},
      LightCfg{23, 4, 0, 300, 0, 200},
      LightCfg{25, 5, 1, -1, 0, 250},
      LightCfg{26, 6, 1, 500, 1, 300},
      LightCfg{27, 7, 0, 200, 1, 350}};
}

/// Hardware anwenden (LEDC setup/attach) + Player binden & (re)starten
static void applyHardware()
{
  for (const auto &L : lights)
  {
    if (L.channel >= MAX_LEDC_CHANNELS)
      continue;
    ledcSetup(L.channel, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(L.pin, L.channel);
    ledcWrite(L.channel, 0);
  }
  size_t n = min<size_t>(8, lights.size());
  for (size_t i = 0; i < n; ++i)
  {
    players[i].attach(lights[i].channel);
    players[i].bind((int)i);
    players[i].begin();
  }
}

/// -------------------- Static Files --------------------
static String contentTypeFor(const String &path)
{
  if (path.endsWith(".html"))
    return "text/html; charset=utf-8";
  if (path.endsWith(".css"))
    return "text/css; charset=utf-8";
  if (path.endsWith(".js"))
    return "application/javascript; charset=utf-8";
  if (path.endsWith(".json"))
    return "application/json; charset=utf-8";
  if (path.endsWith(".ico"))
    return "image/x-icon";
  if (path.endsWith(".png"))
    return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg"))
    return "image/jpeg";
  if (path.endsWith(".svg"))
    return "image/svg+xml; charset=utf-8";
  return "text/plain; charset=utf-8";
}
static bool handleFileRead(String path)
{
  if (path.endsWith("/"))
    path += "index.html";
  if (!LittleFS.exists(path))
    return false;
  File f = LittleFS.open(path, "r");
  if (!f)
    return false;
  server.streamFile(f, contentTypeFor(path));
  f.close();
  return true;
}

/// -------------------- REST Helpers --------------------
static void addCORS()
{
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,PUT,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
static void sendJSON(const String &json)
{
  addCORS();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

/// ---------- Vorwärtsdeklarationen ----------
static void startWiFi();
static void stopWiFi();

/// -------------------- REST Endpoints --------------------
static void handleGetConfig()
{
  JsonDocument doc;
  doc["wifi_enabled"] = wifiEnabled;

  // WiFi config zurückgeben
  JsonObject jw = doc["wifi"].to<JsonObject>();
  jw["mode"] = wifiModeCfg;

  jw["ap"]["ssid"] = apSsidCfg;
  jw["ap"]["password"] = apPassCfg; // Hinweis: in Produktivsystemen nicht im Klartext senden

  jw["sta"]["ssid"] = staSsidCfg;
  jw["sta"]["password"] = staPassCfg;
  jw["sta"]["last_ip"] = ipToStr(staLastIP);
  jw["sta"]["last_gw"] = ipToStr(staLastGW);
  jw["sta"]["last_mask"] = ipToStr(staLastMask);

  // patterns
  JsonArray jPatterns = doc["patterns"].to<JsonArray>();
  for (const auto &p : patterns)
  {
    JsonObject jp = jPatterns.add<JsonObject>();
    jp["name"] = p.name;
    JsonArray jSteps = jp["steps"].to<JsonArray>();
    for (const auto &st : p.steps)
    {
      JsonObject js = jSteps.add<JsonObject>();
      js["duration_ms"] = st.duration_ms;
      js["level"] = st.level;
      js["fade"] = st.fade;
    }
  }
  // groups
  JsonArray jGroups = doc["groups"].to<JsonArray>();
  for (const auto &g : groups)
  {
    JsonObject jg = jGroups.add<JsonObject>();
    jg["id"] = g.id;
    jg["name"] = g.name;
    jg["enabled"] = g.enabled;
  }
  // lights
  JsonArray jLights = doc["lights"].to<JsonArray>();
  for (const auto &L : lights)
  {
    JsonObject jl = jLights.add<JsonObject>();
    jl["pin"] = L.pin;
    jl["channel"] = L.channel;
    jl["patternIndex"] = L.patternIndex;
    jl["restOverrideMs"] = L.restOverrideMs;
    jl["groupId"] = L.groupId;
    jl["phase_ms"] = L.phase_ms;
  }

  String out;
  serializeJson(doc, out);
  sendJSON(out);
}

// /api/config: GET=lesen, PUT/POST=schreiben, OPTIONS=CORS
static void handleConfigEndpoint()
{
  if (server.method() == HTTP_OPTIONS)
  {
    addCORS();
    server.send(204);
    return;
  }
  if (server.method() == HTTP_GET)
  {
    handleGetConfig();
    return;
  }

  // PUT oder POST -> speichern
  if (!server.hasArg("plain"))
  {
    addCORS();
    server.send(400, "text/plain", "Missing body");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err)
  {
    addCORS();
    server.send(400, "text/plain", String("JSON error: ") + err.c_str());
    return;
  }

  // wifi_enabled optional
  if (doc["wifi_enabled"].is<bool>())
  {
    wifiEnabled = (bool)doc["wifi_enabled"];
  }

  // WiFi Settings optional
  if (doc["wifi"].is<JsonObject>())
  {
    if (doc["wifi"]["mode"].is<const char *>())
      wifiModeCfg = doc["wifi"]["mode"].as<const char *>();

    // AP
    if (doc["wifi"]["ap"]["ssid"].is<const char *>())
      apSsidCfg = doc["wifi"]["ap"]["ssid"].as<const char *>();
    if (doc["wifi"]["ap"]["password"].is<const char *>())
      apPassCfg = doc["wifi"]["ap"]["password"].as<const char *>();

    // STA
    if (doc["wifi"]["sta"]["ssid"].is<const char *>())
      staSsidCfg = doc["wifi"]["sta"]["ssid"].as<const char *>();
    if (doc["wifi"]["sta"]["password"].is<const char *>())
      staPassCfg = doc["wifi"]["sta"]["password"].as<const char *>();
  }

  // Patterns/Groups/Lights optional (nur übernehmen, wenn vorhanden)
  if (doc["patterns"].is<JsonArray>())
  {
    patterns.clear();
    for (JsonObject jp : doc["patterns"].as<JsonArray>())
    {
      Pattern p;
      p.name = jp["name"].as<const char *>();
      for (JsonObject js : jp["steps"].as<JsonArray>())
      {
        LEDStep st;
        st.duration_ms = (uint16_t)(js["duration_ms"] | 0);
        st.level = (uint8_t)(js["level"] | 0);
        st.fade = (bool)(js["fade"] | false);
        p.steps.push_back(st);
      }
      patterns.push_back(std::move(p));
    }
  }
  if (doc["groups"].is<JsonArray>())
  {
    groups.clear();
    for (JsonObject jg : doc["groups"].as<JsonArray>())
    {
      GroupCfg g;
      g.id = (int)(jg["id"] | 0);
      g.name = jg["name"].as<const char *>();
      g.enabled = (bool)(jg["enabled"] | true);
      groups.push_back(std::move(g));
    }
  }
  if (doc["lights"].is<JsonArray>())
  {
    lights.clear();
    for (JsonObject jl : doc["lights"].as<JsonArray>())
    {
      LightCfg L;
      L.pin = (uint8_t)(jl["pin"] | 255);
      L.channel = (uint8_t)(jl["channel"] | 0);
      L.patternIndex = (uint8_t)(jl["patternIndex"] | 0);
      L.restOverrideMs = (int)(jl["restOverrideMs"] | -1);
      L.groupId = (int)(jl["groupId"] | 0);
      L.phase_ms = (int)(jl["phase_ms"] | 0);
      lights.push_back(std::move(L));
    }
    applyHardware();
  }

  bool ok = saveConfig();
  addCORS();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK (saved)" : "ERROR (save failed)");
}

// /api/sysinfo
static void handleSysInfo()
{
  JsonDocument doc;
  doc["max_ledc_channels"] = MAX_LEDC_CHANNELS;
  doc["ledc_freq_hz"] = LEDC_FREQ_HZ;
  doc["ledc_res_bits"] = LEDC_RES_BITS;
  doc["num_lights"] = (uint32_t)lights.size();
  if (wifiModeCfg == "sta")
  {
    doc["ip"] = WiFi.localIP().toString();
    doc["gw"] = WiFi.gatewayIP().toString();
    doc["mask"] = WiFi.subnetMask().toString();
    doc["rssi"] = WiFi.RSSI();
  }
  else
  {
    doc["ip"] = WiFi.softAPIP().toString();
  }
  String out;
  serializeJson(doc, out);
  sendJSON(out);
}

// /api/wifi: GET=Status, PUT/POST=ein/aus & Modus/SSIDs/PW, OPTIONS=CORS
static void handleWifiEndpoint()
{
  if (server.method() == HTTP_OPTIONS)
  {
    addCORS();
    server.send(204);
    return;
  }
  if (server.method() == HTTP_GET)
  {
    JsonDocument d;
    d["enabled"] = wifiEnabled;
    d["mode"] = wifiModeCfg;
    if (wifiModeCfg == "ap")
    {
      d["ssid"] = apSsidCfg;
      d["ip"] = WiFi.softAPIP().toString();
    }
    else
    {
      d["ssid"] = staSsidCfg;
      d["ip"] = WiFi.localIP().toString();
      d["gw"] = WiFi.gatewayIP().toString();
      d["mask"] = WiFi.subnetMask().toString();
      d["rssi"] = WiFi.RSSI();
    }
    String out;
    serializeJson(d, out);
    sendJSON(out);
    return;
  }
  if (server.method() == HTTP_PUT || server.method() == HTTP_POST)
  {
    if (!server.hasArg("plain"))
    {
      addCORS();
      server.send(400, "text/plain", "Missing body");
      return;
    }
    JsonDocument d;
    DeserializationError err = deserializeJson(d, server.arg("plain"));
    if (err)
    {
      addCORS();
      server.send(400, "text/plain", String("JSON error: ") + err.c_str());
      return;
    }

    if (d["enabled"].is<bool>())
      wifiEnabled = (bool)d["enabled"];
    if (d["mode"].is<const char *>())
      wifiModeCfg = d["mode"].as<const char *>();

    if (d["ap"]["ssid"].is<const char *>())
      apSsidCfg = d["ap"]["ssid"].as<const char *>();
    if (d["ap"]["password"].is<const char *>())
      apPassCfg = d["ap"]["password"].as<const char *>();

    if (d["sta"]["ssid"].is<const char *>())
      staSsidCfg = d["sta"]["ssid"].as<const char *>();
    if (d["sta"]["password"].is<const char *>())
      staPassCfg = d["sta"]["password"].as<const char *>();

    saveConfig();
    if (wifiEnabled)
      startWiFi();
    else
      stopWiFi();

    JsonDocument r;
    r["enabled"] = wifiEnabled;
    r["mode"] = wifiModeCfg;
    String out;
    serializeJson(r, out);
    sendJSON(out);
    return;
  }
  addCORS();
  server.send(405, "text/plain", "Method Not Allowed");
}

// /api/reboot: POST/ANY -> Neustart
static void handleReboot()
{
  if (server.method() == HTTP_OPTIONS)
  {
    addCORS();
    server.send(204);
    return;
  }
  addCORS();
  server.send(200, "text/plain", "Rebooting");
  delay(100);
  ESP.restart();
}

/// -------------------- WLAN Start/Stop + Server Start/Stop --------------------
static void startServerRoutes()
{
  server.on("/", HTTP_GET, []()
            {
    if (!handleFileRead("/index.html")) server.send(404,"text/plain","index.html not found"); });
  server.on("/favicon.ico", HTTP_GET, []()
            {
    if (!handleFileRead("/favicon.ico")) server.send(204); });

  server.on("/api/config", HTTP_ANY, handleConfigEndpoint);
  server.on("/api/sysinfo", HTTP_GET, handleSysInfo);
  server.on("/api/wifi", HTTP_ANY, handleWifiEndpoint);
  server.on("/api/reboot", HTTP_ANY, handleReboot);

  server.onNotFound([]()
                    {
    String path = server.uri();
    if (path.startsWith("/api/")) {
      JsonDocument d; d["ok"]=false; d["error"]="API route not found"; d["path"]=path;
      String out; serializeJson(d, out);
      addCORS();
      server.send(404, "application/json; charset=utf-8", out);
      return;
    }
    if (handleFileRead(path)) return;
    server.send(404, "text/plain", "Not found"); });
}

// Hilfsfunktion: warte bis zu timeoutMs auf WL_CONNECTED
static bool waitForStaConnect(uint32_t timeoutMs)
{
  uint32_t start = millis();
  wl_status_t st;
  do
  {
    st = WiFi.status();
    if (st == WL_CONNECTED)
      return true;
    delay(100);
  } while (millis() - start < timeoutMs);
  return WiFi.status() == WL_CONNECTED;
}

static void startWiFi()
{
  // Server ggf. neu starten
  if (serverRunning)
  {
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    serverRunning = false;
  }

  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_5dBm); // moderat reduziert

  if (wifiModeCfg == "sta")
  {
    Serial.println("WLAN: wechsle in STA-Modus, versuche DHCP…");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    delay(100);

    // DHCP aktivieren: config() mit INADDR_NONE setzt DHCP
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.begin(staSsidCfg.c_str(), staPassCfg.c_str());

    bool ok = waitForStaConnect(10000); // 10s DHCP/Connect
    if (!ok)
    {
      Serial.println("DHCP/Connect fehlgeschlagen – versuche statisch mit letzten Werten…");
      if (staLastIP != IPAddress(0, 0, 0, 0) && staLastGW != IPAddress(0, 0, 0, 0) && staLastMask != IPAddress(0, 0, 0, 0))
      {
        WiFi.disconnect();
        delay(100);
        WiFi.config(staLastIP, staLastGW, staLastMask);
        WiFi.begin(staSsidCfg.c_str(), staPassCfg.c_str());
        ok = waitForStaConnect(6000);
      }
    }

    if (ok)
    {
      Serial.printf("STA verbunden: IP=%s, GW=%s, MASK=%s, RSSI=%d dBm\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str(),
                    WiFi.subnetMask().toString().c_str(),
                    WiFi.RSSI());
      // letzte erfolgreichen Werte speichern
      staLastIP = WiFi.localIP();
      staLastGW = WiFi.gatewayIP();
      staLastMask = WiFi.subnetMask();
      saveConfig();
    }
    else
    {
      Serial.println("STA-Verbindung fehlgeschlagen.");
    }

    // Webserver starten (auch wenn noch keine IP – sobald verbunden, erreichbar)
    startServerRoutes();
    server.begin();
    serverRunning = true;
  }
  else
  {
    // AP-Modus
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);

    // Fallbacks: leere SSID -> Default-SSID; leeres Passwort -> offener AP
    String ssid = apSsidCfg.length() ? apSsidCfg : String(AP_SSID_DEFAULT);
    String pass = apPassCfg; // darf leer sein für offenen AP

    bool apok = false;
    if (pass.length() == 0)
    {
      apok = WiFi.softAP(ssid.c_str()); // offener AP
    }
    else if (pass.length() >= 8)
    {
      apok = WiFi.softAP(ssid.c_str(), pass.c_str()); // WPA2
    }
    else
    {
      Serial.println("Warnung: Passwort < 8 Zeichen -> starte offenen AP.");
      apok = WiFi.softAP(ssid.c_str());
    }

    Serial.printf("AP gestartet: SSID='%s' (%s), ok=%d\n", ssid.c_str(), WiFi.softAPIP().toString().c_str(), apok);

    startServerRoutes();
    server.begin();
    serverRunning = true;
  }
}

static void stopWiFi()
{
  if (serverRunning)
  {
    server.stop();
    serverRunning = false;
  }
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WLAN/Server gestoppt.");
}

/// -------------------- CLI (Serielle Konsole) --------------------
static String cliLine;

static void cliHelp()
{
  Serial.println(F(
      "\n=== CLI Befehle ===\n"
      "help                              : diese Hilfe\n"
      "status                            : Kurzstatus anzeigen\n"
      "save                              : Konfiguration speichern\n"
      "load                              : Konfiguration neu laden\n"
      "reboot                            : Neustart\n"
      "wifi on|off                       : WLAN/Webserver einschalten/abschalten (persistent)\n"
      "wifi set ap  <ssid> <password>    : AP-SSID/PW setzen (PW leer => offener AP)\n"
      "wifi set sta <ssid> <password>    : STA-SSID/PW setzen\n"
      "wifi mode ap|sta                  : WLAN-Modus wechseln\n"
      "wifi dhcp [timeout_ms]            : im STA-Modus neue DHCP-Adresse beziehen\n" // <-- NEU
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
      "light del <idx>                   : Ausgang löschen\n"));
}

static long asLong(const String &s, long def = 0)
{
  char *e = nullptr;
  long v = strtol(s.c_str(), &e, 10);
  return e && *e == 0 ? v : def;
}

static void cliStatus()
{
  Serial.printf("wifi_enabled: %s, serverRunning: %d\n", wifiEnabled ? "true" : "false", (int)serverRunning);
  Serial.printf("wifi.mode='%s'\n", wifiModeCfg.c_str());
  if (wifiModeCfg == "ap")
  {
    Serial.printf("AP: ssid='%s', ip=%s\n", apSsidCfg.c_str(), WiFi.softAPIP().toString().c_str());
  }
  else
  {
    Serial.printf("STA: ssid='%s', ip=%s, gw=%s, mask=%s, rssi=%d\n",
                  staSsidCfg.c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.subnetMask().toString().c_str(),
                  WiFi.RSSI());
    Serial.printf("STA last: ip=%s gw=%s mask=%s\n",
                  ipToStr(staLastIP).c_str(),
                  ipToStr(staLastGW).c_str(),
                  ipToStr(staLastMask).c_str());
  }
  Serial.printf("patterns: %u, groups: %u, lights: %u\n", (unsigned)patterns.size(), (unsigned)groups.size(), (unsigned)lights.size());
}

static void cliGroupList()
{
  for (auto &g : groups)
    Serial.printf("id=%d name='%s' enabled=%d\n", g.id, g.name.c_str(), g.enabled);
}
static void cliPatList()
{
  for (size_t i = 0; i < patterns.size(); ++i)
  {
    Serial.printf("[%u] '%s' steps=%u\n", (unsigned)i, patterns[i].name.c_str(), (unsigned)patterns[i].steps.size());
  }
}
static void cliLightList()
{
  for (size_t i = 0; i < lights.size(); ++i)
  {
    auto &L = lights[i];
    Serial.printf("[%u] pin=%u ch=%u pidx=%u rest=%d gid=%d phase=%d\n",
                  (unsigned)i, L.pin, L.channel, L.patternIndex, L.restOverrideMs, L.groupId, L.phase_ms);
  }
}

static void cliApplyAndMaybeStartWifi()
{
  applyHardware();
  if (wifiEnabled)
    startWiFi();
  else
    stopWiFi();
}

// Versucht im STA-Modus per DHCP eine Adresse zu beziehen.
// timeoutMs = Wartezeit für Verbindungsaufbau/DHCP.
static bool cliWifiDhcp(uint32_t timeoutMs = 10000)
{
  if (!wifiEnabled)
  {
    Serial.println("WiFi ist deaktiviert. Mit 'wifi on' einschalten.");
    return false;
  }
  if (wifiModeCfg != "sta")
  {
    Serial.println("WLAN-Modus ist nicht 'sta'. Mit 'wifi mode sta' umschalten.");
    return false;
  }
  if (staSsidCfg.isEmpty())
  {
    Serial.println("STA-SSID ist leer. Mit 'wifi set sta <ssid> <password>' setzen.");
    return false;
  }

  Serial.printf("DHCP: versuche neue Lease im STA-Modus (SSID='%s')...\n", staSsidCfg.c_str());
  WiFi.mode(WIFI_STA);

  // DHCP aktivieren
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);

  // Neu verbinden (ohne gespeicherte Credentials zu löschen)
  WiFi.disconnect(); // trennt ggf. existierende Verbindung
  delay(100);
  WiFi.begin(staSsidCfg.c_str(), staPassCfg.c_str());

  bool ok = waitForStaConnect(timeoutMs);
  if (!ok)
  {
    Serial.println("DHCP/Connect fehlgeschlagen.");
    return false;
  }

  // Erfolg → Werte übernehmen & persistieren
  staLastIP = WiFi.localIP();
  staLastGW = WiFi.gatewayIP();
  staLastMask = WiFi.subnetMask();
  saveConfig();

  Serial.printf("DHCP OK: IP=%s, GW=%s, MASK=%s, RSSI=%d dBm\n",
                staLastIP.toString().c_str(),
                staLastGW.toString().c_str(),
                staLastMask.toString().c_str(),
                WiFi.RSSI());
  return true;
}

static void handleCliLine(const String &line)
{
  // Tokenize
  std::vector<String> t;
  String cur;
  for (size_t i = 0; i < line.length(); ++i)
  {
    char c = line[i];
    if (c == ' ' || c == '\t')
    {
      if (cur.length())
      {
        t.push_back(cur);
        cur = "";
      }
    }
    else
      cur += c;
  }
  if (cur.length())
    t.push_back(cur);
  if (t.empty())
    return;
  String cmd = t[0];
  cmd.toLowerCase();

  if (cmd == "help")
  {
    cliHelp();
    return;
  }
  if (cmd == "status")
  {
    cliStatus();
    return;
  }
  if (cmd == "save")
  {
    bool ok = saveConfig();
    Serial.println(ok ? "OK saved" : "ERR save");
    return;
  }
  if (cmd == "load")
  {
    bool ok = loadConfig();
    Serial.println(ok ? "OK loaded" : "ERR load");
    cliApplyAndMaybeStartWifi();
    return;
  }
  if (cmd == "reboot")
  {
    Serial.println("Rebooting...");
    delay(100);
    ESP.restart();
  }

  // wifi commands
  if (cmd == "wifi" && t.size() >= 2)
  {
    String sub = t[1];
    sub.toLowerCase();
    if (sub == "on")
    {
      wifiEnabled = true;
      saveConfig();
      startWiFi();
      Serial.println("WiFi ON.");
      return;
    }
    if (sub == "off")
    {
      wifiEnabled = false;
      saveConfig();
      stopWiFi();
      Serial.println("WiFi OFF.");
      return;
    }
    if (sub == "set" && t.size() >= 5 && t[2] == "ap")
    {
      apSsidCfg = t[3];
      apPassCfg = t[4]; // darf leer sein -> offener AP
      saveConfig();
      if (wifiEnabled && wifiModeCfg == "ap")
        startWiFi();
      Serial.println("AP-Settings aktualisiert.");
      return;
    }
    if (sub == "set" && t.size() >= 5 && t[2] == "sta")
    {
      staSsidCfg = t[3];
      staPassCfg = t[4];
      saveConfig();
      if (wifiEnabled && wifiModeCfg == "sta")
        startWiFi();
      Serial.println("STA-Settings aktualisiert.");
      return;
    }
    if (sub == "mode" && t.size() >= 3)
    {
      String m = t[2];
      m.toLowerCase();
      if (m == "ap" || m == "sta")
      {
        wifiModeCfg = m;
        saveConfig();
        startWiFi();
        Serial.println("WLAN-Modus umgeschaltet.");
        return;
      }
      Serial.println("Usage: wifi mode ap|sta");
      return;
      if (sub == "dhcp")
      {
        uint32_t timeoutMs = 10000;
        if (t.size() >= 3)
          timeoutMs = (uint32_t)asLong(t[2], 10000);
        bool ok = cliWifiDhcp(timeoutMs);
        Serial.println(ok ? "DHCP: OK" : "DHCP: FEHLER");
        return;
      }
    }
    Serial.println("Usage: wifi on|off | wifi set ap <ssid> <password> | wifi set sta <ssid> <password> | wifi mode ap|sta");
    return;
  }

  // group commands
  if (cmd == "group" && t.size() >= 2)
  {
    String sub = t[1];
    sub.toLowerCase();
    if (sub == "list")
    {
      cliGroupList();
      return;
    }
    if (sub == "add" && t.size() >= 4)
    {
      int id = asLong(t[2]);
      String name = t[3];
      groups.push_back({id, name, true});
      saveConfig();
      Serial.println("OK");
      return;
    }
    if (sub == "set" && t.size() >= 5)
    {
      int id = asLong(t[2]);
      String field = t[3];
      field.toLowerCase();
      String val = t[4];
      for (auto &g : groups)
        if (g.id == id)
        {
          if (field == "enabled")
            g.enabled = (asLong(val) != 0);
          else if (field == "name")
            g.name = val;
          else
          {
            Serial.println("Unknown field");
            return;
          }
          saveConfig();
          Serial.println("OK");
          return;
        }
      Serial.println("Group not found");
      return;
    }
    if (sub == "del" && t.size() >= 3)
    {
      int id = asLong(t[2]);
      for (size_t i = 0; i < groups.size(); ++i)
        if (groups[i].id == id)
        {
          groups.erase(groups.begin() + i);
          saveConfig();
          Serial.println("OK");
          return;
        }
      Serial.println("Group not found");
      return;
    }
  }

  // pattern commands
  if (cmd == "pat" && t.size() >= 2)
  {
    String sub = t[1];
    sub.toLowerCase();
    if (sub == "list")
    {
      cliPatList();
      return;
    }
    if (sub == "add" && t.size() >= 3)
    {
      Pattern p;
      p.name = t[2];
      patterns.push_back(std::move(p));
      saveConfig();
      cliPatList();
      return;
    }
    if (sub == "del" && t.size() >= 3)
    {
      int idx = asLong(t[2], -1);
      if (idx < 0 || idx >= (int)patterns.size())
      {
        Serial.println("Index!");
        return;
      }
      patterns.erase(patterns.begin() + idx);
      saveConfig();
      cliPatList();
      return;
    }
    if (sub == "step" && t.size() >= 3)
    {
      String ssub = t[2];
      ssub.toLowerCase();
      if (ssub == "add" && t.size() >= 7)
      {
        int pidx = asLong(t[3], -1);
        uint16_t dur = asLong(t[4]);
        uint8_t lvl = asLong(t[5]);
        bool fade = asLong(t[6]) != 0;
        if (pidx < 0 || pidx >= (int)patterns.size())
        {
          Serial.println("pidx!");
          return;
        }
        patterns[pidx].steps.push_back(LEDStep{dur, lvl, fade});
        saveConfig();
        Serial.println("OK");
        return;
      }
      if (ssub == "set" && t.size() >= 8)
      {
        int pidx = asLong(t[3], -1);
        int sidx = asLong(t[4], -1);
        uint16_t dur = asLong(t[5]);
        uint8_t lvl = asLong(t[6]);
        bool fade = asLong(t[7]) != 0;
        if (pidx < 0 || pidx >= (int)patterns.size())
        {
          Serial.println("pidx!");
          return;
        }
        if (sidx < 0 || sidx >= (int)patterns[pidx].steps.size())
        {
          Serial.println("sidx!");
          return;
        }
        patterns[pidx].steps[sidx] = LEDStep{dur, lvl, fade};
        saveConfig();
        Serial.println("OK");
        return;
      }
      if (ssub == "del" && t.size() >= 5)
      {
        int pidx = asLong(t[3], -1);
        int sidx = asLong(t[4], -1);
        if (pidx < 0 || pidx >= (int)patterns.size())
        {
          Serial.println("pidx!");
          return;
        }
        if (sidx < 0 || sidx >= (int)patterns[pidx].steps.size())
        {
          Serial.println("sidx!");
          return;
        }
        patterns[pidx].steps.erase(patterns[pidx].steps.begin() + sidx);
        saveConfig();
        Serial.println("OK");
        return;
      }
    }
  }

  // light commands
  if (cmd == "light" && t.size() >= 2)
  {
    String sub = t[1];
    sub.toLowerCase();
    if (sub == "list")
    {
      cliLightList();
      return;
    }
    if (sub == "add" && t.size() >= 8)
    {
      LightCfg L;
      L.pin = asLong(t[2]);
      L.channel = asLong(t[3]);
      L.patternIndex = asLong(t[4]);
      L.restOverrideMs = asLong(t[5]);
      L.groupId = asLong(t[6]);
      L.phase_ms = asLong(t[7]);
      lights.push_back(L);
      saveConfig();
      cliApplyAndMaybeStartWifi();
      Serial.println("OK");
      return;
    }
    if (sub == "set" && t.size() >= 5)
    {
      int idx = asLong(t[2], -1);
      String field = t[3];
      field.toLowerCase();
      if (idx < 0 || idx >= (int)lights.size())
      {
        Serial.println("idx!");
        return;
      }
      long val = asLong(t[4]);
      auto &L = lights[idx];
      if (field == "pin")
        L.pin = val;
      else if (field == "ch")
        L.channel = val;
      else if (field == "pidx")
        L.patternIndex = val;
      else if (field == "rest")
        L.restOverrideMs = val;
      else if (field == "gid")
        L.groupId = val;
      else if (field == "phase")
        L.phase_ms = val;
      else
      {
        Serial.println("Unknown field");
        return;
      }
      saveConfig();
      cliApplyAndMaybeStartWifi();
      Serial.println("OK");
      return;
    }
    if (sub == "del" && t.size() >= 3)
    {
      int idx = asLong(t[2], -1);
      if (idx < 0 || idx >= (int)lights.size())
      {
        Serial.println("idx!");
        return;
      }
      lights.erase(lights.begin() + idx);
      saveConfig();
      cliApplyAndMaybeStartWifi();
      Serial.println("OK");
      return;
    }
  }

  Serial.println("Unknown or bad command. 'help' für Hilfe.");
}

static void cliPrintPrompt()
{
  Serial.print("\r\n> ");
  Serial.flush();
}

static void cliPoll()
{
  while (Serial.available())
  {
    char c = (char)Serial.read();

    // Normalize Zeilenende (CR oder LF)
    if (c == '\r' || c == '\n')
    {
      // Zeile abschließen & anzeigen
      Serial.print("\r\n");
      String line = cliLine;
      cliLine = "";
      line.trim();
      if (line.length())
      {
        handleCliLine(line);
      }
      // neuen Prompt anzeigen
      cliPrintPrompt();
      continue;
    }

    // Backspace (BS=8, DEL=127)
    if (c == 8 || c == 127)
    {
      if (cliLine.length() > 0)
      {
        cliLine.remove(cliLine.length() - 1);
        // visuell ein Zeichen löschen: Cursor zurück, Leerzeichen, zurück
        Serial.print("\b \b");
      }
      continue;
    }

    // Nur druckbare ASCII-Zeichen annehmen
    if (c >= 32 && c <= 126)
    {
      cliLine += c;
      Serial.write(c); // Echo
    }
    // andere Steuerzeichen ignorieren
  }
}

/// -------------------- Setup & Loop --------------------
void setup()
{
  if (GAMMA_CORRECTION)
    buildGammaTable();

  Serial.begin(115200);
  delay(200);
  setCpuFrequencyMhz(80); // weniger Spitzenlast
  btStop();               // Bluetooth aus

  Serial.println("\nBlaulicht-Controller (Web/CLI + LittleFS) – WLAN-Hauptschalter");
  Serial.printf("LEDC-Kanaele gesamt: %u\n", MAX_LEDC_CHANNELS);

  if (!LittleFS.begin(true))
    Serial.println("LittleFS init fehlgeschlagen.");

  if (!loadConfig())
  {
    Serial.println("Keine config.json gefunden – erstelle Default.");
    makeDefaultConfig();
    saveConfig();
  }

  applyHardware();

  if (wifiEnabled)
    startWiFi();
  else
  {
    WiFi.mode(WIFI_OFF);
    Serial.println("WLAN ist laut config.json deaktiviert (wifi_enabled=false).");
  }

  Serial.println("\nCLI bereit. 'help' eingeben.");
  cliPrintPrompt();
}

void loop()
{
  if (serverRunning)
    server.handleClient();

  size_t n = min<size_t>(8, lights.size());
  for (size_t i = 0; i < n; ++i)
    players[i].update();

  cliPoll();
}
