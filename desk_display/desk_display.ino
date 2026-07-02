// ESP32 desk display firmware.
//
// First boot (or unreachable WiFi): starts an open SoftAP "DeskDisplay-Setup"
// with a captive portal at http://192.168.4.1 to enter WiFi credentials, which
// persist in NVS. Once on the local network it serves a config portal for the
// display content (mode, countdown target, weather city, message, timezone),
// also persisted in NVS.
//
// Display modes: Clock (NTP), Countdown, Weather (Open-Meteo, no API key),
// Message (scrolling text).

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESP32OTAPull.h>
#include <time.h>

// ---------- Firmware version / OTA ----------
// Keep in sync with version.txt at the repo root; the release workflow
// publishes ota/manifest.json + ota/desk_display.bin to the repo (served via
// raw.githubusercontent.com because ESP32-OTA-Pull's HTTP client does not
// follow the 302 redirects GitHub release-asset URLs use).
constexpr const char* kFirmwareVersion = "1.0.2";
constexpr const char* kOtaManifestUrl =
    "https://raw.githubusercontent.com/GhostService/esp32-desk-display/main/"
    "ota/manifest.json";
constexpr unsigned long kOtaCheckIntervalMs = 60UL * 60 * 1000;  // 1 hour

// ---------- Hardware ----------
constexpr int kSdaPin = 21;
constexpr int kSclPin = 22;
constexpr uint8_t kOledAddress = 0x3C;
constexpr int kScreenWidth = 128;
constexpr int kScreenHeight = 64;

Adafruit_SSD1306 display(kScreenWidth, kScreenHeight, &Wire, /*reset_pin=*/-1);

// ---------- Config (NVS) ----------
enum DisplayMode : uint8_t {
  MODE_CLOCK = 0,
  MODE_COUNTDOWN,
  MODE_WEATHER,
  MODE_MESSAGE,
  MODE_COUNT
};

struct Config {
  String wifiSsid;
  String wifiPass;
  uint8_t mode = MODE_CLOCK;
  String countdownTarget;  // "YYYY-MM-DDTHH:MM" (datetime-local format)
  String city;
  String message = "Hello, it works";
  String tz = "UTC0";  // POSIX TZ string
  bool useFahrenheit = false;
};

Preferences prefs;
Config config;

void loadConfig() {
  prefs.begin("deskdisp", false);
  config.wifiSsid = prefs.getString("ssid", "");
  config.wifiPass = prefs.getString("pass", "");
  config.mode = prefs.getUChar("mode", MODE_CLOCK);
  if (config.mode >= MODE_COUNT) config.mode = MODE_CLOCK;
  config.countdownTarget = prefs.getString("cdown", "");
  config.city = prefs.getString("city", "");
  config.message = prefs.getString("msg", "Hello, it works");
  config.tz = prefs.getString("tz", "UTC0");
  config.useFahrenheit = prefs.getBool("fahr", false);
  prefs.end();
}

void saveContentConfig() {
  prefs.begin("deskdisp", false);
  prefs.putUChar("mode", config.mode);
  prefs.putString("cdown", config.countdownTarget);
  prefs.putString("city", config.city);
  prefs.putString("msg", config.message);
  prefs.putString("tz", config.tz);
  prefs.putBool("fahr", config.useFahrenheit);
  prefs.end();
}

void saveWifiConfig() {
  prefs.begin("deskdisp", false);
  prefs.putString("ssid", config.wifiSsid);
  prefs.putString("pass", config.wifiPass);
  prefs.end();
}

// ---------- Runtime state ----------
constexpr const char* kApSsid = "DeskDisplay-Setup";
const IPAddress kApIp(192, 168, 4, 1);

bool apMode = false;
WebServer server(80);
DNSServer dnsServer;

enum WeatherStatus { WEATHER_IDLE, WEATHER_OK, WEATHER_ERROR };
WeatherStatus weatherStatus = WEATHER_IDLE;
String weatherError;
String weatherPlace;
float weatherTemp = 0;
int weatherWmoCode = 0;
bool geocoded = false;
double geoLat = 0, geoLon = 0;
unsigned long lastWeatherAttempt = 0;
bool weatherAttempted = false;

unsigned long lastOtaCheckMs = 0;
bool otaCheckRequested = false;
String otaStatus = "not checked yet";

int16_t scrollX = kScreenWidth;
uint16_t messagePixelWidth = 0;
bool messageDirty = true;

unsigned long lastRender = 0;

// ---------- I2C scanner (bring-up diagnostic; keep) ----------
void scanI2C() {
  Serial.println("Scanning I2C bus...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found device at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  No I2C devices found — check wiring");
  } else {
    Serial.printf("Scan complete: %d device(s) found\n", found);
  }
}

// ---------- Display helpers ----------
void drawCentered(const String& text, int16_t y, uint8_t size) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((kScreenWidth - (int16_t)w) / 2, y);
  display.print(text);
}

void showStatus(const String& line1, const String& line2 = "",
                const String& line3 = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawCentered(line1, 8, 1);
  if (line2.length()) drawCentered(line2, 28, 1);
  if (line3.length()) drawCentered(line3, 44, 1);
  display.display();
}

// ---------- Small utilities ----------
String urlEncode(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

// Parses "YYYY-MM-DDTHH:MM" (HTML datetime-local) as local time.
bool parseDatetimeLocal(const String& s, time_t* out) {
  int y, mo, d, h, mi;
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
    return false;
  }
  struct tm tmv = {};
  tmv.tm_year = y - 1900;
  tmv.tm_mon = mo - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = h;
  tmv.tm_min = mi;
  tmv.tm_isdst = -1;
  time_t t = mktime(&tmv);
  if (t == (time_t)-1) return false;
  *out = t;
  return true;
}

bool timeIsSynced() {
  return time(nullptr) > 1000000000;  // any plausible post-2001 epoch
}

void applyTimezone() {
  configTzTime(config.tz.c_str(), "pool.ntp.org", "time.google.com");
}

// ---------- Web portal ----------
// The pages are static HTML/JS; all data moves through the JSON API
// (/api/config, /api/wifi, /api/wifireset) built on ArduinoJson.

const char kContentHtml[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk Display</title><style>
body{font-family:sans-serif;margin:1.5em;background:#111;color:#eee}
label{display:block;margin-top:1em}
input,select,button{width:100%;padding:.6em;font-size:1em;box-sizing:border-box}
button{margin-top:1.5em;background:#2a7;color:#fff;border:0;border-radius:4px}
button.danger{background:#a33}
small{color:#999}h2{margin-top:0}hr{border-color:#333;margin:1.5em 0}
#toast{color:#2a7;min-height:1.2em}
</style></head><body><h2>Desk Display</h2>
<form id="f">
<label>Display mode<select id="mode">
<option value="0">Clock</option>
<option value="1">Countdown</option>
<option value="2">Weather</option>
<option value="3">Message</option>
</select></label>
<label>Countdown target<input type="datetime-local" id="countdown"></label>
<label>Weather city<input id="city" placeholder="e.g. Portland"></label>
<label>Temperature unit<select id="unit">
<option value="C">Celsius</option>
<option value="F">Fahrenheit</option>
</select></label>
<label>Message<input id="message"></label>
<label>Time zone<select id="tz">
<option value="UTC0">UTC</option>
<option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option>
<option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
<option value="MST7MDT,M3.2.0,M11.1.0">US Mountain</option>
<option value="MST7">US Arizona</option>
<option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option>
<option value="GMT0BST,M3.5.0/1,M10.5.0">UK</option>
<option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe</option>
<option value="IST-5:30">India</option>
<option value="JST-9">Japan</option>
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia Eastern</option>
<option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand</option>
<option value="custom">Custom (POSIX string)...</option>
</select></label>
<input id="tzcustom" style="display:none" placeholder="e.g. CET-1CEST,M3.5.0,M10.5.0/3">
<button type="submit">Save</button>
</form>
<p id="toast"></p>
<hr>
<button id="update">Check for update now</button>
<button id="wifireset" class="danger">Reset WiFi settings</button>
<p><small id="ipline"></small><br><small id="otaline"></small></p>
<script>
const $=id=>document.getElementById(id);
const tzSel=$('tz'),tzC=$('tzcustom');
tzSel.onchange=()=>{tzC.style.display=tzSel.value==='custom'?'block':'none';};
const tzValue=()=>tzSel.value==='custom'?tzC.value:tzSel.value;
function toast(t){$('toast').textContent=t;setTimeout(()=>$('toast').textContent='',4000);}
async function load(){
 const c=await(await fetch('/api/config')).json();
 $('mode').value=c.mode;$('countdown').value=c.countdown;
 $('city').value=c.city;$('message').value=c.message;
 $('unit').value=c.fahrenheit?'F':'C';
 if([...tzSel.options].some(o=>o.value===c.tz)){tzSel.value=c.tz;}
 else{tzSel.value='custom';tzC.value=c.tz;tzC.style.display='block';}
 $('ipline').textContent='Device: '+c.ip+'  |  WiFi: '+c.wifiSsid;
 $('otaline').textContent='Firmware v'+c.version+'  |  Update: '+c.otaStatus;
}
$('f').onsubmit=async e=>{
 e.preventDefault();
 const body={mode:+$('mode').value,countdown:$('countdown').value,
  city:$('city').value,message:$('message').value,tz:tzValue(),
  fahrenheit:$('unit').value==='F'};
 const r=await fetch('/api/config',{method:'POST',
  headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
 toast(r.ok?'Saved - display updated':'Save failed');
};
$('update').onclick=async()=>{
 const b=$('update');b.disabled=true;
 $('toast').textContent='Checking for update...';
 try{await fetch('/api/checkupdate',{method:'POST'});}catch(e){}
 // The device blocks while checking/flashing and reboots after a successful
 // update, so fetches fail for a while; poll until it answers with a result.
 for(let i=0;i<40;i++){
  await new Promise(r=>setTimeout(r,3000));
  try{
   const c=await(await fetch('/api/config')).json();
   if(c.otaStatus==='checking')continue;
   await load();
   toast('v'+c.version+' - '+c.otaStatus);
   b.disabled=false;
   return;
  }catch(e){}
 }
 b.disabled=false;
 toast('No response from device - refresh this page');
};
$('wifireset').onclick=async()=>{
 if(!confirm('Forget WiFi and reboot into setup mode?'))return;
 await fetch('/api/wifireset',{method:'POST'});
 alert('WiFi cleared. Rebooting into setup AP "DeskDisplay-Setup".');
};
load();
</script></body></html>)HTML";

const char kSetupHtml[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk Display Setup</title><style>
body{font-family:sans-serif;margin:1.5em;background:#111;color:#eee}
label{display:block;margin-top:1em}
input,button{width:100%;padding:.6em;font-size:1em;box-sizing:border-box}
button{margin-top:1.5em;background:#2a7;color:#fff;border:0;border-radius:4px}
h2{margin-top:0}#toast{color:#2a7;min-height:1.2em}
</style></head><body><h2>WiFi setup</h2>
<p>Connect the display to your home network.</p>
<form id="f">
<label>Network name (SSID)<input id="ssid"></label>
<label>Password<input type="password" id="pass"></label>
<button type="submit">Save &amp; reboot</button>
</form>
<p id="toast"></p>
<script>
const $=id=>document.getElementById(id);
fetch('/api/config').then(r=>r.json()).then(c=>{$('ssid').value=c.wifiSsid;}).catch(()=>{});
$('f').onsubmit=async e=>{
 e.preventDefault();
 const r=await fetch('/api/wifi',{method:'POST',
  headers:{'Content-Type':'application/json'},
  body:JSON.stringify({ssid:$('ssid').value,pass:$('pass').value})});
 $('toast').textContent=r.ok?'Saved. Rebooting and connecting...':'Save failed - is the SSID empty?';
};
</script></body></html>)HTML";

void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void sendJsonError(int code, const char* msg) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = msg;
  sendJson(code, doc);
}

void handleRoot() {
  server.send_P(200, "text/html", apMode ? kSetupHtml : kContentHtml);
}

void handleConfigGet() {
  JsonDocument doc;
  doc["mode"] = config.mode;
  doc["countdown"] = config.countdownTarget;
  doc["city"] = config.city;
  doc["message"] = config.message;
  doc["tz"] = config.tz;
  doc["fahrenheit"] = config.useFahrenheit;
  doc["version"] = kFirmwareVersion;
  doc["otaStatus"] = otaStatus;
  doc["wifiSsid"] = config.wifiSsid;
  doc["ip"] =
      apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  sendJson(200, doc);
}

void handleConfigPost() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJsonError(400, "invalid JSON");
    return;
  }

  if (doc["mode"].is<int>()) {
    int mode = doc["mode"];
    if (mode >= 0 && mode < MODE_COUNT) config.mode = (uint8_t)mode;
  }
  if (doc["countdown"].is<const char*>()) {
    config.countdownTarget = doc["countdown"].as<String>();
  }
  if (doc["city"].is<const char*>()) {
    String newCity = doc["city"].as<String>();
    newCity.trim();
    if (newCity != config.city) {
      config.city = newCity;
      geocoded = false;
      weatherStatus = WEATHER_IDLE;
      weatherAttempted = false;  // fetch immediately
    }
  }
  if (doc["message"].is<const char*>()) {
    String newMessage = doc["message"].as<String>();
    if (newMessage != config.message) {
      config.message = newMessage;
      messageDirty = true;
    }
  }
  if (doc["fahrenheit"].is<bool>()) {
    config.useFahrenheit = doc["fahrenheit"].as<bool>();
  }
  if (doc["tz"].is<const char*>()) {
    String newTz = doc["tz"].as<String>();
    newTz.trim();
    if (newTz.length() && newTz != config.tz) {
      config.tz = newTz;
      applyTimezone();
    }
  }

  saveContentConfig();
  JsonDocument res;
  res["ok"] = true;
  sendJson(200, res);
}

void handleWifiPost() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendJsonError(400, "invalid JSON");
    return;
  }
  String ssid = doc["ssid"] | "";
  ssid.trim();
  if (ssid.isEmpty()) {
    sendJsonError(400, "ssid required");
    return;
  }
  config.wifiSsid = ssid;
  config.wifiPass = doc["pass"] | "";
  saveWifiConfig();

  JsonDocument res;
  res["ok"] = true;
  res["rebooting"] = true;
  sendJson(200, res);
  delay(1200);  // let the response flush before dropping the AP
  ESP.restart();
}

void handleWifiReset() {
  config.wifiSsid = "";
  config.wifiPass = "";
  saveWifiConfig();

  JsonDocument res;
  res["ok"] = true;
  res["rebooting"] = true;
  sendJson(200, res);
  delay(1200);
  ESP.restart();  // boots into the setup AP since the SSID is now empty
}

void handleCheckUpdate() {
  otaCheckRequested = true;  // picked up by loop(); the check blocks rendering
  otaStatus = "checking";    // so /api/config polls never show a stale result
  JsonDocument res;
  res["ok"] = true;
  res["status"] = "checking";
  sendJson(200, res);
}

void handleNotFound() {
  if (apMode) {
    // Captive portal: bounce every probe to the setup page.
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302);
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/wifireset", HTTP_POST, handleWifiReset);
  server.on("/api/checkupdate", HTTP_POST, handleCheckUpdate);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ---------- WiFi ----------
void startSoftAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(kApSsid);
  dnsServer.start(53, "*", kApIp);
  Serial.printf("SoftAP started: %s, http://%s\n", kApSsid,
                WiFi.softAPIP().toString().c_str());
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
  Serial.printf("Connecting to %s", config.wifiSsid.c_str());
  showStatus("Connecting to WiFi", config.wifiSsid);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Serial.printf("Connected, IP: %s\n", ip.c_str());
    applyTimezone();
    showStatus("WiFi connected", ip, "Portal: http://" + ip);
    delay(10000);
  } else {
    Serial.println("WiFi connect failed — starting setup AP");
    startSoftAP();
  }
}

// ---------- Weather (Open-Meteo, no API key) ----------
String httpGetBody(const String& url, int* httpCode) {
  WiFiClientSecure client;
  client.setInsecure();  // local hobby device; Open-Meteo data is not sensitive
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    *httpCode = -1;
    return "";
  }
  int code = http.GET();
  *httpCode = code;
  String body = (code > 0) ? http.getString() : "";
  http.end();
  return body;
}

const char* wmoToText(int code) {
  switch (code) {
    case 0: return "Clear sky";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: case 82: return "Rain showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm+hail";
    default: return "Unknown";
  }
}

void setWeatherError(const String& msg) {
  weatherStatus = WEATHER_ERROR;
  weatherError = msg;
  Serial.println("Weather: " + msg);
}

void fetchWeather() {
  if (!geocoded) {
    String url =
        "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en"
        "&format=json&name=" +
        urlEncode(config.city);
    int code;
    String body = httpGetBody(url, &code);
    if (code != 200) {
      setWeatherError("Geocode HTTP " + String(code));
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      setWeatherError("Geocode parse error");
      return;
    }
    JsonVariant result = doc["results"][0];
    if (result.isNull()) {
      setWeatherError("ERROR: City Not Found.");
      return;
    }
    geoLat = result["latitude"].as<double>();
    geoLon = result["longitude"].as<double>();
    weatherPlace = result["name"].as<String>();
    geocoded = true;
    Serial.printf("Geocoded %s -> %.4f,%.4f\n", weatherPlace.c_str(), geoLat,
                  geoLon);
  }

  String url =
      "https://api.open-meteo.com/v1/forecast?current=temperature_2m,"
      "weather_code&latitude=" +
      String(geoLat, 4) + "&longitude=" + String(geoLon, 4);
  int code;
  String body = httpGetBody(url, &code);
  if (code != 200) {
    setWeatherError("Weather HTTP " + String(code));
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    setWeatherError("Weather parse error");
    return;
  }
  JsonVariant current = doc["current"];
  if (current.isNull()) {
    setWeatherError("No current data");
    return;
  }
  weatherTemp = current["temperature_2m"].as<float>();
  weatherWmoCode = current["weather_code"].as<int>();
  weatherStatus = WEATHER_OK;
  Serial.printf("Weather: %.1fC, %s\n", weatherTemp,
                wmoToText(weatherWmoCode));
}

void maybeFetchWeather() {
  if (apMode || config.mode != MODE_WEATHER) return;
  if (WiFi.status() != WL_CONNECTED || config.city.isEmpty()) return;
  unsigned long interval =
      (weatherStatus == WEATHER_OK) ? 10UL * 60 * 1000 : 60UL * 1000;
  if (!weatherAttempted || millis() - lastWeatherAttempt >= interval) {
    weatherAttempted = true;
    lastWeatherAttempt = millis();
    fetchWeather();
  }
}

// ---------- OTA updates (ESP32-OTA-Pull) ----------
void otaProgress(int offset, int totallength) {
  static int lastPct = -1;
  int pct = totallength > 0 ? (int)(((int64_t)offset * 100) / totallength) : 0;
  if (pct == lastPct) return;
  lastPct = pct;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawCentered("Updating firmware", 8, 1);
  drawCentered(String(pct) + "%", 26, 2);
  display.drawRect(14, 52, 100, 8, SSD1306_WHITE);
  display.fillRect(14, 52, pct, 8, SSD1306_WHITE);
  display.display();
}

// Compares dotted numeric versions; returns <0, 0, >0 like strcmp.
// ESP32-OTA-Pull compares version strings byte-wise, which sorts
// "1.0.10" before "1.0.9" — so the manifest check is done here instead.
int compareVersions(const String& a, const String& b) {
  unsigned int ia = 0, ib = 0;
  while (ia < a.length() || ib < b.length()) {
    long na = 0, nb = 0;
    while (ia < a.length() && a[ia] != '.') na = na * 10 + (a[ia++] - '0');
    while (ib < b.length() && b[ib] != '.') nb = nb * 10 + (b[ib++] - '0');
    if (na != nb) return na < nb ? -1 : 1;
    ia++;
    ib++;
  }
  return 0;
}

// Fetches the manifest and, if it lists a newer version for this board,
// downloads the image, flashes it, and reboots (inside CheckForOTAUpdate).
// Reaching the bottom switch means an update should have happened but didn't.
void runOtaCheck() {
  if (apMode || WiFi.status() != WL_CONNECTED) {
    otaStatus = "skipped (no WiFi)";
    return;
  }
  lastOtaCheckMs = millis();
  Serial.printf("OTA: checking %s (running v%s)\n", kOtaManifestUrl,
                kFirmwareVersion);
  showStatus("Checking for", "firmware update...",
             "current v" + String(kFirmwareVersion));

  // The manifest is fetched here (not left to the library) for two reasons:
  // the cache-busting query defeats raw.githubusercontent's 5-minute CDN
  // cache (clicking "check" right after a release used to see the previous
  // manifest and report "up to date"), and the numeric compare above replaces
  // the library's byte-wise one.
  String url = String(kOtaManifestUrl) + "?nocache=" + String(millis());
  int code;
  String body = httpGetBody(url, &code);
  if (code != 200) {
    otaStatus = "manifest HTTP " + String(code);
    Serial.println("OTA: " + otaStatus);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    otaStatus = "bad manifest";
    Serial.println("OTA: " + otaStatus);
    return;
  }
  String newVersion;
  for (JsonVariant cfg : doc["Configurations"].as<JsonArray>()) {
    String board = cfg["Board"] | "";
    if (board.isEmpty() || board == ARDUINO_BOARD) {
      newVersion = cfg["Version"] | "";
      break;
    }
  }
  if (newVersion.isEmpty()) {
    otaStatus = "no build for this board";
    Serial.println("OTA: " + otaStatus);
    return;
  }
  if (compareVersions(newVersion, kFirmwareVersion) <= 0) {
    otaStatus = "up to date (v" + String(kFirmwareVersion) + ")";
    Serial.println("OTA: " + otaStatus);
    return;
  }

  Serial.printf("OTA: updating to v%s\n", newVersion.c_str());
  showStatus("Updating to", "v" + newVersion, "do not power off");
  // The newer-version decision is already made above; AllowDowngrades makes
  // the library act on any version difference so its own byte-wise compare
  // cannot veto the update. It re-fetches the manifest at the same
  // cache-busted URL, so it sees the same version this check just did.
  ESP32OTAPull ota;
  ota.SetCallback(otaProgress);
  ota.AllowDowngrades(true);
  int ret = ota.CheckForOTAUpdate(url.c_str(), kFirmwareVersion,
                                  ESP32OTAPull::UPDATE_AND_BOOT);
  switch (ret) {
    case ESP32OTAPull::WRITE_ERROR:
    case ESP32OTAPull::OTA_UPDATE_FAIL:
      otaStatus = "update to v" + newVersion + " failed";
      break;
    case ESP32OTAPull::HTTP_FAILED:
      otaStatus = "image unreachable";
      break;
    default:
      otaStatus = "update error " + String(ret);
      break;
  }
  Serial.println("OTA: " + otaStatus);
}

// ---------- Renderers ----------
void renderApMode() {
  drawCentered("WiFi Setup", 0, 1);
  drawCentered("Join network:", 18, 1);
  drawCentered(kApSsid, 30, 1);
  drawCentered("then open:", 44, 1);
  drawCentered("http://192.168.4.1", 54, 1);
}

void renderClock() {
  if (!timeIsSynced()) {
    drawCentered("Syncing time...", 28, 1);
    return;
  }
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%H:%M", &tmv);
  drawCentered(buf, 14, 3);
  strftime(buf, sizeof(buf), "%a %d %b %Y", &tmv);
  drawCentered(buf, 48, 1);
}

void renderCountdown() {
  time_t target;
  if (config.countdownTarget.isEmpty() ||
      !parseDatetimeLocal(config.countdownTarget, &target)) {
    drawCentered("No countdown set", 22, 1);
    drawCentered("Set one via portal", 36, 1);
    return;
  }
  if (!timeIsSynced()) {
    drawCentered("Syncing time...", 28, 1);
    return;
  }
  drawCentered("COUNTDOWN", 0, 1);
  long diff = (long)difftime(target, time(nullptr));
  char buf[24];
  if (diff <= 0) {
    drawCentered("Time's up!", 22, 2);
  } else {
    long days = diff / 86400;
    long hours = (diff % 86400) / 3600;
    long mins = (diff % 3600) / 60;
    long secs = diff % 60;
    if (days > 0) {
      snprintf(buf, sizeof(buf), "%ldd %02ld:%02ld", days, hours, mins);
    } else {
      snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", hours, mins, secs);
    }
    drawCentered(buf, 22, 2);
  }
  String pretty = config.countdownTarget;
  pretty.replace("T", " ");
  drawCentered(pretty, 52, 1);
}

void renderWeather() {
  if (config.city.isEmpty()) {
    drawCentered("No city set", 22, 1);
    drawCentered("Set one via portal", 36, 1);
    return;
  }
  if (weatherStatus == WEATHER_ERROR) {
    drawCentered("Weather error:", 18, 1);
    drawCentered(weatherError, 34, 1);
    return;
  }
  if (weatherStatus != WEATHER_OK) {
    drawCentered("Fetching weather...", 28, 1);
    return;
  }
  drawCentered(weatherPlace, 0, 1);
  char buf[16];
  float temp =
      config.useFahrenheit ? weatherTemp * 9.0f / 5.0f + 32.0f : weatherTemp;
  snprintf(buf, sizeof(buf), "%.0f%c%c", temp, (char)248,  // ° (CP437)
           config.useFahrenheit ? 'F' : 'C');
  drawCentered(buf, 18, 3);
  drawCentered(wmoToText(weatherWmoCode), 52, 1);
}

void renderMessage() {
  if (config.message.isEmpty()) {
    drawCentered("No message set", 28, 1);
    return;
  }
  display.setTextSize(2);
  if (messageDirty) {
    int16_t x1, y1;
    uint16_t h;
    display.getTextBounds(config.message, 0, 0, &x1, &y1, &messagePixelWidth,
                          &h);
    scrollX = kScreenWidth;
    messageDirty = false;
  }
  if (messagePixelWidth <= kScreenWidth) {
    drawCentered(config.message, 24, 2);
    return;
  }
  display.setCursor(scrollX, 24);
  display.print(config.message);
  scrollX -= 3;
  if (scrollX < -(int16_t)messagePixelWidth) scrollX = kScreenWidth;
}

void render() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  if (apMode) {
    renderApMode();
  } else {
    switch (config.mode) {
      case MODE_CLOCK: renderClock(); break;
      case MODE_COUNTDOWN: renderCountdown(); break;
      case MODE_WEATHER: renderWeather(); break;
      case MODE_MESSAGE: renderMessage(); break;
      default: renderClock(); break;
    }
  }
  display.display();
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  delay(500);  // give the serial monitor time to attach

  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(400000);  // fast mode; full-frame updates at ~23ms
  scanI2C();

  if (!display.begin(SSD1306_SWITCHCAPVCC, kOledAddress)) {
    Serial.println("SSD1306 init failed — halting");
    for (;;) {
      delay(1000);
    }
  }
  display.cp437(true);  // real code page 437 so char 248 is the degree sign
  showStatus("Desk Display", "starting...");

  loadConfig();

  if (config.wifiSsid.isEmpty()) {
    startSoftAP();
  } else {
    connectWiFi();
  }
  startWebServer();
  runOtaCheck();  // boot-time update check (no-op in AP mode / no WiFi)
}

void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();
  maybeFetchWeather();

  if (!apMode && WiFi.status() == WL_CONNECTED &&
      (otaCheckRequested || millis() - lastOtaCheckMs >= kOtaCheckIntervalMs)) {
    otaCheckRequested = false;
    runOtaCheck();
  }

  // Message mode animates a scroll, so it renders faster.
  unsigned long interval =
      (!apMode && config.mode == MODE_MESSAGE) ? 33 : 250;
  if (millis() - lastRender >= interval) {
    lastRender = millis();
    render();
  }
}
