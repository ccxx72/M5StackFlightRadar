// M5Radar Select - flight radar per M5Stack (Core/Core2/CoreS3/Fire)
// Adattamento compatto di https://github.com/AnthonySturdy/micro-radar
// BtnA (sx): cicla tra gli aerei sullo schermo (evidenzia il selezionato)
// BtnB (centro): schermata dettagli dell'aereo selezionato / torna al radar
// BtnC (dx): refresh immediato dei dati
// Librerie: M5Unified, ArduinoJson (v7)

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <vector>

// ================== CONFIGURAZIONE ==================
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const double LAT        = 0.0000;     // centro radar
const double LON        = 0.0000;
const double RAD        = 1.0;         // raggio in gradi (max 2)
const char* OSKY_ID     = "";          // client ID OpenSky (vuoto = anonimo)
const char* OSKY_SECRET = "";
// ====================================================

struct Plane {
  String callsign, country, squawk;
  float lat, lon, baroAlt, geoAlt, vel, track, vRate;
  long tPos, tContact;
  float fromLat, fromLon, alpha = 1;
  unsigned long seen, tick = 0;

  void predict(float& pLat, float& pLon) const {
    float dt = (millis() - seen) / 1000.0f + (tPos > 0 ? tContact - tPos : 0);
    float h = radians(track), m = 111320.0f;
    pLat = lat + vel * dt * cos(h) / m;
    pLon = lon + vel * dt * sin(h) / (m * cos(radians(lat)));
  }
  void display(float& dLat, float& dLon) {
    unsigned long now = millis();
    alpha = min(alpha + (now - tick) / 1000.0f * 0.15f, 1.0f); tick = now;
    predict(dLat, dLon);
    if (alpha < 1) {
      float t = alpha * alpha * (3 - 2 * alpha);
      dLat = fromLat + t * (dLat - fromLat);
      dLon = fromLon + t * (dLon - fromLon);
    }
  }
};

std::map<String, Plane> planes;
std::vector<String> onScreen;           // icao24 visibili, ordinati (per il ciclo)
M5Canvas cv(&M5.Display);
int CX, CY, R;
unsigned long fetchInterval, lastFetch = 0;
String token; unsigned long tokenExp = 0;
String selected = "";                   // icao24 selezionato
bool detailView = false;

String getToken() {
  if (!*OSKY_ID || !*OSKY_SECRET) return "";
  if (token.length() && millis() < tokenExp) return token;
  HTTPClient h;
  h.begin("https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token");
  h.addHeader("Content-Type", "application/x-www-form-urlencoded");
  if (h.POST("grant_type=client_credentials&client_id=" + String(OSKY_ID) + "&client_secret=" + String(OSKY_SECRET)) == 200) {
    JsonDocument d; deserializeJson(d, h.getString());
    token = d["access_token"] | ""; tokenExp = millis() + 29UL * 60000;
  } else token = "";
  h.end(); return token;
}

void fetchPlanes() {
  HTTPClient h;
  h.begin("https://opensky-network.org/api/states/all?lamin=" + String(LAT - RAD, 4) +
          "&lamax=" + String(LAT + RAD, 4) + "&lomin=" + String(LON - RAD, 4) + "&lomax=" + String(LON + RAD, 4));
  String tk = getToken();
  if (tk.length()) h.addHeader("Authorization", "Bearer " + tk);
  int code = h.GET();
  if (code != 200) { Serial.printf("[WARN] OpenSky HTTP %d\n", code); h.end(); return; }
  JsonDocument doc;
  if (deserializeJson(doc, h.getString())) { h.end(); return; }
  h.end();

  unsigned long now = millis();
  std::map<String, bool> live;
  for (JsonArray s : doc["states"].as<JsonArray>()) {
    if (s[8].as<bool>()) continue;               // a terra
    String id = s[0] | "";
    live[id] = true;
    Plane& p = planes[id];
    if (p.seen) { float a, b; p.display(a, b); p.fromLat = a; p.fromLon = b; p.alpha = 0; }
    p.callsign = s[1] | ""; p.callsign.trim();
    p.country  = s[2] | "";
    p.tPos = s[3] | 0L; p.tContact = s[4] | 0L;
    p.lon = s[5] | 0.0f; p.lat = s[6] | 0.0f; p.baroAlt = s[7] | 0.0f;
    p.vel = s[9] | 0.0f; p.track = s[10] | 0.0f; p.vRate = s[11] | 0.0f;
    p.geoAlt = s[13] | 0.0f; p.squawk = s[14] | "";
    if (!p.seen) { p.fromLat = p.lat; p.fromLon = p.lon; }
    p.seen = now;
  }
  for (auto it = planes.begin(); it != planes.end();)
    it = live.count(it->first) ? ++it : planes.erase(it);
  if (!planes.count(selected)) { selected = ""; detailView = false; }
}

// distanza (km) e rilevamento (°) dal centro radar
void distBearing(float pLat, float pLon, float& km, float& brg) {
  float dLat = radians(pLat - LAT), dLon = radians(pLon - LON);
  float a = sq(sin(dLat / 2)) + cos(radians(LAT)) * cos(radians(pLat)) * sq(sin(dLon / 2));
  km = 6371.0f * 2 * atan2(sqrt(a), sqrt(1 - a));
  brg = fmod(degrees(atan2(sin(dLon) * cos(radians(pLat)),
        cos(radians(LAT)) * sin(radians(pLat)) - sin(radians(LAT)) * cos(radians(pLat)) * cos(dLon))) + 360, 360);
}

void drawTriangle(int x, int y, float track, uint32_t col) {
  float dx = sin(radians(track)), dy = -cos(radians(track));
  cv.fillTriangle(x + dx * 6, y + dy * 6,
                  x - dx * 3 + dy * 1.5f, y - dy * 3 - dx * 1.5f,
                  x - dx * 3 - dy * 1.5f, y - dy * 3 + dx * 1.5f, col);
}

void drawRadar() {
  cv.fillScreen(TFT_BLACK);
  cv.drawCircle(CX, CY, R, cv.color888(0, 200, 0));
  cv.drawCircle(CX, CY, R * 2 / 3, cv.color888(0, 64, 0));
  cv.drawCircle(CX, CY, R / 3, cv.color888(0, 32, 0));
  float a = millis() / 3000.0f;
  for (int i = 0; i < 20; i++) {
    float aa = a - i * 0.015f;
    cv.drawLine(CX, CY, CX + cos(aa) * R, CY + sin(aa) * R, cv.color888(0, 128 - i * 6, 0));
  }

  onScreen.clear();
  for (auto& [id, p] : planes) {
    float pLat, pLon; p.display(pLat, pLon);
    int x = CX + (int)((pLon - LON) / RAD * R);
    int y = CY - (int)((pLat - LAT) / RAD * R);
    if ((x - CX) * (x - CX) + (y - CY) * (y - CY) > R * R) continue;
    onScreen.push_back(id);

    bool sel = (id == selected);
    uint32_t col = sel ? cv.color888(255, 255, 0) : cv.color888(0, 255, 0);
    drawTriangle(x, y, p.track, col);
    if (sel) {                                   // evidenzia il selezionato
      cv.drawCircle(x, y, 10, col);
      cv.drawCircle(x, y, 11, col);
    }
    cv.setTextColor(sel ? col : cv.color888(0, 128, 0));
    cv.drawString(p.callsign.length() ? p.callsign : id, x + 8, y + 8);
  }
  // barra di stato in basso
  cv.setTextColor(cv.color888(0, 100, 0));
  cv.drawString(String(planes.size()) + " voli  " + (selected.length() ? selected : "nessuna selezione"), 4, M5.Display.height() - 12);
  cv.pushSprite(0, 0);
}

void drawDetail() {
  Plane& p = planes[selected];
  float pLat, pLon; p.predict(pLat, pLon);
  float km, brg; distBearing(pLat, pLon, km, brg);

  cv.fillScreen(TFT_BLACK);
  cv.drawRect(2, 2, M5.Display.width() - 4, M5.Display.height() - 4, cv.color888(0, 200, 0));
  cv.setTextColor(cv.color888(0, 255, 0));
  cv.setTextSize(2);
  cv.drawString(p.callsign.length() ? p.callsign : "(no callsign)", 12, 12);
  cv.setTextSize(1);
  cv.setTextColor(cv.color888(0, 180, 0));

  int y = 42; const int lh = 15;
  auto row = [&](const String& k, const String& v) {
    cv.drawString(k, 12, y);
    cv.drawString(v, 120, y);
    y += lh;
  };
  row("ICAO24",      selected);
  row("Paese",       p.country);
  row("Squawk",      p.squawk.length() ? p.squawk : "-");
  row("Posizione",   String(pLat, 4) + ", " + String(pLon, 4));
  row("Alt. baro",   String((int)p.baroAlt) + " m");
  row("Alt. geo",    String((int)p.geoAlt) + " m");
  row("Velocita'",   String((int)p.vel) + " m/s (" + String((int)(p.vel * 3.6f)) + " km/h)");
  row("Rotta",       String((int)p.track) + " deg");
  row("Var. quota",  String(p.vRate, 1) + " m/s " + (p.vRate > 0.5f ? "(sale)" : p.vRate < -0.5f ? "(scende)" : "(livello)"));
  row("Distanza",    String(km, 1) + " km, rlv " + String((int)brg) + " deg");
  row("Dati agg.",   String((millis() - p.seen) / 1000) + " s fa");

  cv.setTextColor(cv.color888(0, 100, 0));
  cv.drawString("[B] torna al radar", 12, M5.Display.height() - 16);
  cv.pushSprite(0, 0);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  CX = M5.Display.width() / 2; CY = M5.Display.height() / 2;
  R = min(CX, CY) - 1;
  cv.setColorDepth(8);
  cv.createSprite(M5.Display.width(), M5.Display.height());

  M5.Display.setTextColor(TFT_GREEN);
  M5.Display.drawCenterString("Connessione WiFi...", CX, CY);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(250);

  fetchInterval = 86400000UL / ((getToken().length() ? 4000 : 400) - 3);
  lastFetch = millis() - fetchInterval;   // fetch immediato
}

void loop() {
  M5.update();

  // BtnA: cicla tra gli aerei visibili
  if (M5.BtnA.wasPressed() && !detailView && !onScreen.empty()) {
    int idx = -1;
    for (size_t i = 0; i < onScreen.size(); i++)
      if (onScreen[i] == selected) { idx = i; break; }
    selected = onScreen[(idx + 1) % onScreen.size()];
  }
  // BtnB: dettagli <-> radar
  if (M5.BtnB.wasPressed()) {
    if (detailView) detailView = false;
    else if (selected.length() && planes.count(selected)) detailView = true;
  }
  // BtnC: refresh immediato
  if (M5.BtnC.wasPressed()) lastFetch = 0;

  if (millis() - lastFetch >= fetchInterval) { lastFetch = millis(); fetchPlanes(); }

  if (detailView && planes.count(selected)) drawDetail();
  else { detailView = false; drawRadar(); }
}
