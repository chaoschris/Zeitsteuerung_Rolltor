//ESP12F_Relay-X4

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <time.h>

// ==========================================
// 1. CONFIGURATION (Standardwerte beim ersten Start)
// ==========================================
const char* def_ssid         = "WLAN_SSID";
const char* def_password     = "wlan_password";
const char* www_username     = "admin";
const char* www_password     = "Password123";
const char* ntpServer        = "de.pool.ntp.org";
const char* timeZone         = "CET-1CEST,M3.5.0,M10.5.0/3";

// ==========================================
// 2. PIN-SETTINGS & STATUS
// ==========================================
int getRelayPin(int i) { return (i==0)?16:(i==1)?14:(i==2)?12:13; }
int getInputPin(int i) { return (i==0)?0:(i==1)?2:(i==2)?4:(i==3)?5:15; }

bool rState0 = false, rState1 = false, rState2 = false, rState3 = false;
bool lastBlockedState = false;

bool getRelayState(int i) {
  return (i==0)?rState0:(i==1)?rState1:(i==2)?rState2:rState3;
}

#define SSID_LEN 33
#define PASS_LEN 65
#define TIME_BUF_LEN 40

struct AppConfig {
  uint32_t magic;
  char ssid[SSID_LEN];
  char password[PASS_LEN];
  bool useStatic;
  uint32_t ipRaw, subRaw, gwRaw, dnsRaw;
  int pirInputIdx;
  bool pirActiveHigh;
  uint32_t pirDelaySec;
  int pulseRelayIdx;
  uint16_t pulseIntMin;
  uint16_t pulseDurMs;
  uint16_t winStartMin;
  uint16_t winEndMin;
};

AppConfig cfg;
const uint32_t EEPROM_MAGIC = 0xCAFEBAC4;
ESP8266WebServer server(80);

unsigned long pirHoldUntil = 0;
unsigned long lastPulseTriggerMs = 0;
unsigned long pulseOffAtMs = 0;
bool isPulseCurrentlyOn = false;

String getFormattedTime() {
  if (time(nullptr) < 100000) return "Zeit wird synchronisiert...";
  struct tm ti;
  if (!getLocalTime(&ti, 10)) return "Zeit wird synchronisiert...";
  char b[TIME_BUF_LEN];
  strftime(b, sizeof(b), "%d.%m.%Y - %H:%M:%S Uhr", &ti);
  return String(b);
}

void logSerial(String msg) {
  Serial.println("[" + getFormattedTime() + "] " + msg);
}

bool isPirPinActive() {
  if (cfg.pirInputIdx < 0 || cfg.pirInputIdx > 4) return false;
  int val = digitalRead(getInputPin(cfg.pirInputIdx));
  return cfg.pirActiveHigh ? (val == HIGH) : (val == LOW);
}

bool isSystemBlocked() {
  if (cfg.pirInputIdx < 0 || cfg.pirInputIdx > 4) return false;
  if (isPirPinActive()) {
    pirHoldUntil = millis() + (cfg.pirDelaySec * 1000UL);
    return true;
  }
  if (cfg.pirDelaySec > 0 && (long)(pirHoldUntil - millis()) > 0) return true;
  return false;
}

uint32_t getRemainingBlockSec() {
  if (isPirPinActive()) return cfg.pirDelaySec;
  long rem = (long)(pirHoldUntil - millis());
  return (rem > 0) ? (uint32_t)((rem + 999) / 1000) : 0;
}

void setRelayState(int i, bool s) {
  if (isSystemBlocked() && s) {
    logSerial("BLOCKIERT: Einschalten von Relais " + String(i + 1) + " durch Bewegungsmelder verhindert.");
    s = false;
  }
  
  bool prevState = getRelayState(i);
  if (i == 0) rState0 = s;
  else if (i == 1) rState1 = s;
  else if (i == 2) rState2 = s;
  else if (i == 3) rState3 = s;
  
  digitalWrite(getRelayPin(i), s ? HIGH : LOW);
  
  if (prevState != s) {
    logSerial("RELAIS: Relais " + String(i + 1) + " (GPIO" + String(getRelayPin(i)) + ") -> " + (s ? "EIN" : "AUS"));
  }
}

bool isWithinTimeWindow() {
  if (time(nullptr) < 100000) return false;
  struct tm ti;
  if (!getLocalTime(&ti, 10)) return false;
  uint16_t nowMin = ti.tm_hour * 60 + ti.tm_min;
  if (cfg.winStartMin == cfg.winEndMin) return true;
  if (cfg.winStartMin < cfg.winEndMin) return (nowMin >= cfg.winStartMin && nowMin < cfg.winEndMin);
  return (nowMin >= cfg.winStartMin || nowMin < cfg.winEndMin);
}

String minToHHMM(uint16_t m) {
  int hh = (m / 60) % 24, mm = m % 60;
  return (hh < 10 ? "0" : "") + String(hh) + ":" + (mm < 10 ? "0" : "") + String(mm);
}

uint16_t hhmmToMin(String s) {
  int sep = s.indexOf(':');
  if (sep < 0) return 0;
  return constrain(s.substring(0, sep).toInt(), 0, 23) * 60 + constrain(s.substring(sep + 1).toInt(), 0, 59);
}

void saveConfig() {
  cfg.magic = EEPROM_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
  logSerial("EEPROM: Konfiguration erfolgreich gespeichert.");
}

void loadConfig() {
  EEPROM.begin(sizeof(AppConfig));
  EEPROM.get(0, cfg);
  if (cfg.magic != EEPROM_MAGIC) {
    Serial.println("[EEPROM] Keine gueltige Konfiguration gefunden. Lade Standardwerte...");
    strncpy(cfg.ssid, def_ssid, sizeof(cfg.ssid) - 1);
    cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    strncpy(cfg.password, def_password, sizeof(cfg.password) - 1);
    cfg.password[sizeof(cfg.password) - 1] = '\0';
    
    cfg.useStatic = false;
    cfg.ipRaw  = (uint32_t)IPAddress(192, 168, 1, 200);
    cfg.subRaw = (uint32_t)IPAddress(255, 255, 255, 0);
    cfg.gwRaw  = (uint32_t)IPAddress(192, 168, 1, 1);
    cfg.dnsRaw = (uint32_t)IPAddress(192, 168, 1, 1);
    cfg.pirInputIdx = -1; cfg.pirActiveHigh = false; cfg.pirDelaySec = 10;
    cfg.pulseRelayIdx = -1; cfg.pulseIntMin = 5; cfg.pulseDurMs = 500;
    cfg.winStartMin = 8 * 60; cfg.winEndMin = 20 * 60;
    saveConfig();
  } else {
    Serial.println("[EEPROM] Konfiguration erfolgreich geladen.");
  }
}

void checkSerialCommands();

void connectWiFi() {
  Serial.print("[WLAN] Verbinde mit SSID: '");
  Serial.print(cfg.ssid);
  Serial.println("'");
  
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setPhyMode(WIFI_PHY_MODE_11N);
  WiFi.setOutputPower(17.5);
  WiFi.setAutoReconnect(true);
  
  if (cfg.useStatic) {
    Serial.println("[WLAN] Nutze statische IP-Konfiguration...");
    WiFi.config(IPAddress(cfg.ipRaw), IPAddress(cfg.gwRaw), IPAddress(cfg.subRaw), IPAddress(cfg.dnsRaw));
  } else {
    Serial.println("[WLAN] Nutze DHCP...");
  }
  
  WiFi.begin(cfg.ssid, cfg.password);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
    delay(250);
    Serial.print(".");
    checkSerialCommands();
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WLAN] Verbunden! IP-Adresse: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.print("[WLAN] FEHLER: Verbindung fehlgeschlagen! Status-Code: ");
    Serial.println(WiFi.status());
    Serial.println("[INFO] Moegliche Ursachen bei Code 4:");
    Serial.println("       - Router nutzt WPA3 / PMF (bitte auf reines WPA2 CCMP stellen)");
    Serial.println("       - MAC-Adressfilter im Router aktiv");
    Serial.println("       - 3.3V-Spannungseinbruch beim Senden");
    Serial.println("[INFO] Tippe 'wifi' in die serielle Konsole, um SSID/Passwort zu pruefen.");
  }
}

String readSerialLine(bool maskPassword = false) {
  delay(100);
  while (Serial.available()) Serial.read();
  
  String input = "";
  while (true) {
    if (Serial.available() > 0) {
      char c = Serial.read();
      
      if (c == '\n' || c == '\r') {
        if (input.length() > 0) {
          delay(20);
          while (Serial.available()) Serial.read();
          Serial.println();
          return input;
        }
      } 
      else if (c == 8 || c == 127) {
        if (input.length() > 0) {
          input.remove(input.length() - 1);
          Serial.print("\b \b");
        }
      } 
      else if (c >= 32 && c <= 126) {
        input += c;
        if (maskPassword) {
          Serial.print('*');
        } else {
          Serial.print(c);
        }
      }
    }
    delay(10);
  }
}

void runSerialWifiSetup() {
  Serial.println("\n==========================================");
  Serial.println("   WLAN-Konfiguration (Serielle Konsole)");
  Serial.println("==========================================");
  Serial.print("Neue WLAN-SSID: ");
  String newSsid = readSerialLine(false);
  newSsid.trim();
  
  Serial.print("WLAN-Passwort:  ");
  String newPass = readSerialLine(true);
  newPass.trim();
  
  strncpy(cfg.ssid, newSsid.c_str(), sizeof(cfg.ssid) - 1);
  cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
  strncpy(cfg.password, newPass.c_str(), sizeof(cfg.password) - 1);
  cfg.password[sizeof(cfg.password) - 1] = '\0';
  
  saveConfig();
  Serial.println("[SYSTEM] WLAN-Daten gespeichert! Neustart in 2 Sekunden...");
  delay(2000);
  ESP.restart();
}

void checkSerialCommands() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    
    if (cmd == "wifi" || cmd == "wlan") {
      runSerialWifiSetup();
    } else if (cmd == "status") {
      Serial.println("\n================ SYSTEM-STATUS ================");
      Serial.println("SSID:   " + String(cfg.ssid));
      
      if (WiFi.status() == WL_CONNECTED) {
        long rssi = WiFi.RSSI();
        int quality = constrain(2 * (rssi + 100), 0, 100);
        String rating = (rssi >= -55) ? "Sehr gut" :
                        (rssi >= -67) ? "Gut" :
                        (rssi >= -80) ? "Mittel" : "Schwach";
        
        Serial.println("WLAN:   Verbunden (" + WiFi.localIP().toString() + ")");
        Serial.println("Signal: " + String(quality) + "% (" + String(rssi) + " dBm - " + rating + ")");
      } else {
        Serial.println("WLAN:   Getrennt");
        Serial.println("Signal: Kein Signal");
      }

      Serial.println("Zeit:   " + getFormattedTime());
      Serial.println("Sperre: " + String(isSystemBlocked() ? "AKTIV (" + String(getRemainingBlockSec()) + "s verbleibend)" : "Frei"));
      
      Serial.println("\n--- Relais-Ausgaenge ---");
      for (int i = 0; i < 4; i++) {
        String role = (cfg.pulseRelayIdx == i) ? " [Impuls-Timer]" : "";
        Serial.println("Relais " + String(i + 1) + " (GPIO" + String(getRelayPin(i)) + "): " + 
                       String(getRelayState(i) ? "EIN" : "AUS") + role);
      }

      Serial.println("\n--- Digitale Eingaenge ---");
      for (int i = 0; i < 5; i++) {
        int pin = getInputPin(i);
        bool hi = (digitalRead(pin) == HIGH);
        String role = (cfg.pirInputIdx == i) ? " [PIR-Sperre]" : "";
        Serial.println("Eingang " + String(i + 1) + " (GPIO" + String(pin) + "): " + 
                       String(hi ? "HIGH (1)" : "LOW (0)") + role);
      }
      Serial.println("===============================================\n");
    } else if (cmd == "help" || cmd == "?") {
      Serial.println("\nVerfuegbare Befehle:");
      Serial.println("  wifi   - WLAN SSID und Passwort neu einrichten");
      Serial.println("  status - Aktuellen System-, Netzwerk- und Pin-Status anzeigen");
      Serial.println("  reboot - ESP8266 neu starten\n");
    } else if (cmd == "reboot") {
      Serial.println("[SYSTEM] Neustart wird eingeleitet...");
      delay(500);
      ESP.restart();
    }
  }
}

bool checkAuth() {
  if (!server.authenticate(www_username, www_password)) { server.requestAuthentication(); return false; }
  return true;
}

String getHead(String title) {
  String h = "<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'><title>" + title + "</title><style>";
  h += "body{font-family:Arial,sans-serif;background:#1e1e2f;color:#f0f0f0;margin:0;padding:15px;text-align:center;}";
  h += ".c{max-width:620px;margin:auto;background:#2a2a40;padding:20px;border-radius:12px;}h1{color:#00d2ff;margin:0 0 10px;}";
  h += ".nav{margin-bottom:18px;display:flex;flex-wrap:wrap;justify-content:center;gap:6px;}.nav a{color:#00d2ff;text-decoration:none;font-weight:bold;padding:6px 10px;border-radius:6px;background:#343450;font-size:0.88em;}";
  h += ".time{color:#aaa;margin-bottom:12px;}.alert{background:#dc3545;color:#fff;padding:12px;border-radius:8px;font-weight:bold;margin-bottom:15px;}";
  h += ".info{background:#23354d;color:#7ec8ff;padding:10px;border-radius:8px;font-size:0.88em;margin-bottom:15px;}";
  h += ".card{background:#343450;padding:15px;border-radius:8px;margin-bottom:18px;text-align:left;}h2{margin:0 0 10px;font-size:1.1em;border-bottom:1px solid #4a4a6a;padding-bottom:6px;text-align:center;}";
  h += ".row{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #3f3f5e;}.row:last-child{border:none;}";
  h += ".badge{padding:4px 10px;border-radius:15px;font-weight:bold;font-size:0.85em;}.on{background:#28a745;}.off{background:#dc3545;}";
  h += ".tag{background:#ff9800;color:#000;padding:2px 7px;border-radius:10px;font-size:0.72em;margin-left:6px;}.tag-b{background:#00d2ff;color:#000;padding:2px 7px;border-radius:10px;font-size:0.72em;margin-left:6px;}";
  h += ".btn{border:none;cursor:pointer;padding:8px 14px;border-radius:6px;font-weight:bold;color:#fff;}.btn-on{background:#28a745;}.btn-off{background:#6c757d;}.btn-lock{background:#444;color:#999;cursor:not-allowed;}.btn-save{background:#007bff;width:100%;margin-top:10px;}";
  h += "input.txt,select.txt{width:100%;box-sizing:border-box;padding:8px;margin:4px 0 10px;border-radius:5px;border:1px solid #555;background:#222235;color:#fff;}";
  h += "</style></head><body><div class='c'><h1>" + title + "</h1>";
  h += "<div class='nav'><a href='/'>Steuerung</a><a href='/timer'>Impuls-Timer</a><a href='/pir'>Bewegungsmelder</a><a href='/network'>Netzwerk</a></div>";
  return h;
}

String getTimerStatusText() {
  if (cfg.pulseRelayIdx < 0 || cfg.pulseRelayIdx > 3) return "";
  String s = "Impuls-Timer (Relais " + String(cfg.pulseRelayIdx + 1) + ", " + minToHHMM(cfg.winStartMin) + "-" + minToHHMM(cfg.winEndMin) + "): ";
  if (!isWithinTimeWindow()) return s + "Au&szlig;erhalb Zeitfenster";
  unsigned long intervalMs = (unsigned long)cfg.pulseIntMin * 60000UL;
  unsigned long elapsed = millis() - lastPulseTriggerMs;
  long remSec = (elapsed >= intervalMs) ? 0 : (long)((intervalMs - elapsed + 999) / 1000);
  return s + "N&auml;chster Impuls in " + String(remSec) + "s (" + String(cfg.pulseDurMs / 1000.0, 1) + "s)";
}

void handleStatus() {
  if (!checkAuth()) return;
  bool blk = isSystemBlocked();
  String j = "{";
  j += "\"time\":\"" + getFormattedTime() + "\",";
  j += "\"blocked\":" + String(blk ? "true" : "false") + ",";
  j += "\"pinAct\":" + String(isPirPinActive() ? "true" : "false") + ",";
  j += "\"rem\":" + String(getRemainingBlockSec()) + ",";
  j += "\"tmr\":\"" + getTimerStatusText() + "\"";
  for (int i = 0; i < 4; i++) {
    j += ",\"r" + String(i) + "\":" + String(getRelayState(i) ? "true" : "false");
  }
  for (int i = 0; i < 5; i++) {
    j += ",\"i" + String(i) + "\":" + String(digitalRead(getInputPin(i)) == HIGH ? "1" : "0");
  }
  j += "}";
  server.send(200, "application/json", j);
}

void handleRoot() {
  if (!checkAuth()) return;
  bool blk = isSystemBlocked();
  String tmrTxt = getTimerStatusText();
  String h = getHead("ESP12F Relay X4");
  h += "<div class='time'>&#128339; <span id='clock'>" + getFormattedTime() + "</span></div>";
  h += "<div id='pir-alert' class='alert' style='display:" + String(blk?"block":"none") + ";'>&#9888; Bewegungsmelder aktiv &ndash; Relais blockiert!</div>";
  h += "<div id='tmr-info' class='info' style='display:" + String(tmrTxt.length()>0?"block":"none") + ";'>" + tmrTxt + "</div>";

  h += "<div class='card'><h2>Relais-Ausg&auml;nge</h2>";
  for (int i = 0; i < 4; i++) {
    bool st = getRelayState(i);
    String rTag = (cfg.pulseRelayIdx == i) ? "<span class='tag-b'>Impuls-Timer</span>" : "";
    h += "<div class='row'><span><strong>Relais " + String(i+1) + "</strong> (GPIO" + String(getRelayPin(i)) + ")" + rTag + "</span>";
    h += "<span id='rb" + String(i) + "' class='badge " + String(st?"on'>EIN":"off'>AUS") + "</span>";
    h += "<button id='rt" + String(i) + "' onclick='tgl(" + String(i) + ")' class='btn " + String(blk?"btn-lock' disabled>GESPERRT":(st?"btn-off'>Ausschalten":"btn-on'>Einschalten")) + "</button></div>";
  }
  h += "</div><div class='card'><h2>Digitale Eing&auml;nge (Live)</h2>";
  for (int i = 0; i < 5; i++) {
    int pin = getInputPin(i);
    bool hi = (digitalRead(pin) == HIGH);
    h += "<div class='row'><span><strong>GPIO" + String(pin) + "</strong>" + String(cfg.pirInputIdx==i?"<span class='tag'>Bewegungsmelder Eingang</span>":"") + "</span>";
    h += "<span id='ib" + String(i) + "' class='badge " + String(hi?"on'>HIGH (1)":"off'>LOW (0)") + "</span></div>";
  }
  h += "</div><p style='font-size:0.8em;color:#888;'>IP: " + WiFi.localIP().toString() + " (" + String(cfg.useStatic?"Statisch":"DHCP") + ")</p></div>";

  h += "<script>function sr(i,s,b){var x=document.getElementById('rb'+i),y=document.getElementById('rt'+i);x.className=s?'badge on':'badge off';x.innerText=s?'EIN':'AUS';if(b){y.className='btn btn-lock';y.innerText='GESPERRT';y.disabled=true;}else{y.disabled=false;y.className=s?'btn btn-off':'btn btn-on';y.innerText=s?'Ausschalten':'Einschalten';}}";
  h += "function si(i,v){var x=document.getElementById('ib'+i);x.className=v===1?'badge on':'badge off';x.innerText=v===1?'HIGH (1)':'LOW (0)';}";
  h += "function upd(d){document.getElementById('clock').innerText=d.time;var a=document.getElementById('pir-alert');a.style.display=d.blocked?'block':'none';";
  h += "if(d.blocked){a.innerHTML=d.pinAct?'&#9888; Bewegungsmelder Eingang aktiv &ndash; Relais blockiert!':'&#9203; Nachlauf-Sperrzeit aktiv &ndash; Freigabe in '+d.rem+'s';}";
  h += "var t=document.getElementById('tmr-info');if(d.tmr){t.style.display='block';t.innerHTML=d.tmr;}else{t.style.display='none';}";
  h += "sr(0,d.r0,d.blocked);sr(1,d.r1,d.blocked);sr(2,d.r2,d.blocked);sr(3,d.r3,d.blocked);si(0,d.i0);si(1,d.i1);si(2,d.i2);si(3,d.i3);si(4,d.i4);}";
  h += "function f(){var x=new XMLHttpRequest();x.open('GET','/status',true);x.withCredentials=true;x.onreadystatechange=function(){if(x.readyState===4&&x.status===200)upd(JSON.parse(x.responseText));};x.send();}";
  h += "function tgl(i){var x=new XMLHttpRequest();x.open('GET','/toggle?relay='+i,true);x.withCredentials=true;x.onreadystatechange=function(){if(x.readyState===4&&x.status===200)f();};x.send();}";
  h += "setInterval(f,1000);</script></body></html>";
  server.send(200, "text/html", h);
}

void handleTimerConfig() {
  if (!checkAuth()) return;
  String durStr = String(cfg.pulseDurMs / 1000.0, 1);
  String h = getHead("Impuls-Timer");
  h += "<div class='card'><h2>Zyklischer Relais-Impuls im Zeitfenster</h2><form action='/savetimer' method='POST'>";
  h += "<label>Zugewiesener Relaisausgang:</label><select name='r_idx' class='txt'>";
  h += "<option value='-1'" + String(cfg.pulseRelayIdx==-1?" selected":"") + ">-- Deaktiviert --</option>";
  for (int i = 0; i < 4; i++) h += "<option value='" + String(i) + "'" + String(cfg.pulseRelayIdx==i?" selected":"") + ">Relais " + String(i+1) + " (GPIO" + String(getRelayPin(i)) + ")</option>";
  h += "</select><label>Wiederholungs-Intervall (1 bis 30 Minuten):</label>";
  h += "<input type='number' min='1' max='30' name='int_min' class='txt' value='" + String(cfg.pulseIntMin) + "' required>";
  h += "<label>Anzugsdauer / Impuls (0.1 bis 3.0 Sekunden):</label>";
  h += "<input type='number' step='0.1' min='0.1' max='3.0' name='dur_sec' class='txt' value='" + durStr + "' required>";
  h += "<label>Zeitfenster Start (Uhrzeit):</label>";
  h += "<input type='time' name='w_start' class='txt' value='" + minToHHMM(cfg.winStartMin) + "' required>";
  h += "<label>Zeitfenster Ende (Uhrzeit):</label>";
  h += "<input type='time' name='w_end' class='txt' value='" + minToHHMM(cfg.winEndMin) + "' required>";
  h += "<button type='submit' class='btn btn-save'>Speichern</button></form></div></div></body></html>";
  server.send(200, "text/html", h);
}

void handleSaveTimer() {
  if (!checkAuth()) return;
  if (server.hasArg("r_idx")) cfg.pulseRelayIdx = server.arg("r_idx").toInt();
  if (server.hasArg("int_min")) cfg.pulseIntMin = constrain(server.arg("int_min").toInt(), 1, 30);
  if (server.hasArg("dur_sec")) {
    String ds = server.arg("dur_sec"); ds.replace(",", ".");
    cfg.pulseDurMs = constrain((int)(ds.toFloat() * 1000.0 + 0.5), 100, 3000);
  }
  if (server.hasArg("w_start")) cfg.winStartMin = hhmmToMin(server.arg("w_start"));
  if (server.hasArg("w_end"))   cfg.winEndMin   = hhmmToMin(server.arg("w_end"));
  lastPulseTriggerMs = millis(); isPulseCurrentlyOn = false;
  
  logSerial("WEB-CONFIG: Impuls-Timer aktualisiert -> Relais: " + String(cfg.pulseRelayIdx + 1) + 
            ", Intervall: " + String(cfg.pulseIntMin) + "m, Dauer: " + String(cfg.pulseDurMs) + 
            "ms, Fenster: " + minToHHMM(cfg.winStartMin) + "-" + minToHHMM(cfg.winEndMin));
            
  saveConfig(); server.sendHeader("Location", "/"); server.send(303);
}

void handlePirConfig() {
  if (!checkAuth()) return;
  String h = getHead("Bewegungsmelder Eingang");
  h += "<div class='card'><h2>Sperreingang &amp; Zeitverz&ouml;gerung</h2><form action='/savepir' method='POST'><label>Zugewiesener Eingang:</label><select name='idx' class='txt'>";
  h += "<option value='-1'" + String(cfg.pirInputIdx==-1?" selected":"") + ">-- Deaktiviert --</option>";
  for (int i = 0; i < 5; i++) h += "<option value='" + String(i) + "'" + String(cfg.pirInputIdx==i?" selected":"") + ">Eingang GPIO" + String(getInputPin(i)) + "</option>";
  h += "</select><label>Aktiver Zustand:</label><select name='pol' class='txt'>";
  h += "<option value='0'" + String(!cfg.pirActiveHigh?" selected":"") + ">Active-LOW (GND blockiert Relais)</option>";
  h += "<option value='1'" + String(cfg.pirActiveHigh?" selected":"") + ">Active-HIGH (3.3V blockiert Relais)</option>";
  h += "</select><label>Nachlauf-Sperrzeit nach Bewegungsende (Sekunden):</label>";
  h += "<input type='number' min='0' max='86400' name='delay' class='txt' value='" + String(cfg.pirDelaySec) + "' required>";
  h += "<button type='submit' class='btn btn-save'>Speichern</button></form></div></div></body></html>";
  server.send(200, "text/html", h);
}

void handleSavePir() {
  if (!checkAuth()) return;
  if (server.hasArg("idx")) cfg.pirInputIdx = server.arg("idx").toInt();
  if (server.hasArg("pol")) cfg.pirActiveHigh = (server.arg("pol").toInt() == 1);
  if (server.hasArg("delay")) { long d = server.arg("delay").toInt(); cfg.pirDelaySec = (d < 0) ? 0 : (uint32_t)d; }
  pirHoldUntil = 0; 
  
  logSerial("WEB-CONFIG: Bewegungsmelder aktualisiert -> Eingang: " + 
            (cfg.pirInputIdx >= 0 ? "GPIO" + String(getInputPin(cfg.pirInputIdx)) : "Deaktiviert") + 
            ", Logik: " + (cfg.pirActiveHigh ? "Active-HIGH" : "Active-LOW") + 
            ", Nachlauf: " + String(cfg.pirDelaySec) + "s");
            
  saveConfig(); server.sendHeader("Location", "/"); server.send(303);
}

void handleNetwork() {
  if (!checkAuth()) return;
  String h = getHead("Netzwerk-Einstellungen");
  h += "<div class='card'><h2>IP-Adressvergabe</h2><form action='/savenetwork' method='POST'>";
  h += "<p><label><input type='radio' name='mode' value='dhcp' onclick='tf(false)' " + String(!cfg.useStatic?"checked":"") + "> Automatisch (DHCP)</label></p>";
  h += "<p><label><input type='radio' name='mode' value='static' onclick='tf(true)' " + String(cfg.useStatic?"checked":"") + "> Statische IP-Adresse</label></p><div id='sf'>";
  h += "<label>IP-Adresse:</label><input id='f1' class='txt' type='text' name='ip' value='" + IPAddress(cfg.ipRaw).toString() + "' required>";
  h += "<label>Subnetzmaske:</label><input id='f2' class='txt' type='text' name='subnet' value='" + IPAddress(cfg.subRaw).toString() + "' required>";
  h += "<label>Gateway:</label><input id='f3' class='txt' type='text' name='gateway' value='" + IPAddress(cfg.gwRaw).toString() + "' required>";
  h += "<label>DNS-Server:</label><input id='f4' class='txt' type='text' name='dns' value='" + IPAddress(cfg.dnsRaw).toString() + "' required>";
  h += "</div><button type='submit' class='btn btn-save'>Speichern &amp; Neu starten</button></form></div></div>";
  h += "<script>function tf(s){document.getElementById('f1').disabled=!s;document.getElementById('f2').disabled=!s;document.getElementById('f3').disabled=!s;document.getElementById('f4').disabled=!s;document.getElementById('sf').style.opacity=s?'1':'0.4';}tf(" + String(cfg.useStatic?"true":"false") + ");</script></body></html>";
  server.send(200, "text/html", h);
}

void handleSaveNetwork() {
  if (!checkAuth()) return;
  if (server.arg("mode") == "static") {
    IPAddress ip, sub, gw, dns;
    if (ip.fromString(server.arg("ip")) && sub.fromString(server.arg("subnet")) && gw.fromString(server.arg("gateway")) && dns.fromString(server.arg("dns"))) {
      cfg.useStatic = true; cfg.ipRaw = (uint32_t)ip; cfg.subRaw = (uint32_t)sub; cfg.gwRaw = (uint32_t)gw; cfg.dnsRaw = (uint32_t)dns;
      logSerial("WEB-CONFIG: Statische IP gespeichert (" + ip.toString() + ")");
    } else { 
      logSerial("WEB-CONFIG FEHLER: Ungueltiges IP-Format eingegeben.");
      server.send(400, "text/plain", "Ungueltiges IP-Format!"); return; 
    }
  } else {
    cfg.useStatic = false;
    logSerial("WEB-CONFIG: Netzwerk auf DHCP umgestellt.");
  }
  saveConfig();
  server.send(200, "text/html", "<html><body style='background:#1e1e2f;color:#fff;font-family:Arial;text-align:center;padding:40px;'><h2>Gespeichert! Neustart...</h2></body></html>");
  logSerial("SYSTEM: Neustart wird durchgefuehrt...");
  delay(1500); ESP.restart();
}

void handleToggle() {
  if (!checkAuth()) return;
  if (!isSystemBlocked() && server.hasArg("relay")) {
    int idx = server.arg("relay").toInt();
    if (idx >= 0 && idx < 4) {
      logSerial("WEB-AKTION: Manueller Schaltbefehl fuer Relais " + String(idx + 1));
      setRelayState(idx, !getRelayState(idx));
    }
  } else if (isSystemBlocked()) {
    logSerial("WEB-AKTION ABGEWIESEN: Relais-Schaltversuch waehrend PIR-Sperre.");
  }
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n==========================================");
  Serial.println("   ESP12F Relay-X4 Rolltor-Steuerung");
  Serial.println("==========================================");
  Serial.println("[INFO] Tippe 'help' oder 'wifi' fuer Konsolenbefehle.\n");
  
  Serial.println("[INIT] Initialisiere Relais-Ausgaenge (alle AUS)...");
  for (int i = 0; i < 4; i++) { pinMode(getRelayPin(i), OUTPUT); digitalWrite(getRelayPin(i), LOW); }
  
  Serial.println("[INIT] Initialisiere digitale Eingaenge...");
  for (int i = 0; i < 5; i++) { int p = getInputPin(i); pinMode(p, (p == 15) ? INPUT : INPUT_PULLUP); }
  
  loadConfig(); 
  connectWiFi(); 
  
  Serial.println("[NTP] Starte Zeitsynchronisation mit " + String(ntpServer) + "...");
  configTime(timeZone, ntpServer);
  
  lastPulseTriggerMs = millis();
  server.on("/", handleRoot); server.on("/status", handleStatus); server.on("/toggle", handleToggle);
  server.on("/timer", handleTimerConfig); server.on("/savetimer", HTTP_POST, handleSaveTimer);
  server.on("/pir", handlePirConfig); server.on("/savepir", HTTP_POST, handleSavePir);
  server.on("/network", handleNetwork); server.on("/savenetwork", HTTP_POST, handleSaveNetwork);
  server.begin();
  Serial.println("[HTTP] Webserver gestartet auf Port 80.");
  Serial.println("[SYSTEM] Setup abgeschlossen. Bereit.\n");
}

void loop() {
  checkSerialCommands();
  
  bool blocked = isSystemBlocked();
  
  if (blocked != lastBlockedState) {
    if (blocked) {
      logSerial("ALARM: Bewegungsmelder hat ausgeloest! Relais werden blockiert (Nachlauf: " + String(cfg.pirDelaySec) + "s).");
    } else {
      logSerial("ENTWARNUNG: Bewegungsmelder-Sperre beendet. Relais wieder freigegeben.");
    }
    lastBlockedState = blocked;
  }

  if (blocked) {
    for (int i = 0; i < 4; i++) {
      if (getRelayState(i)) setRelayState(i, false);
    }
    if (isPulseCurrentlyOn) {
      logSerial("TIMER: Laufender Impuls durch Bewegungsmelder abgebrochen!");
      isPulseCurrentlyOn = false;
    }
  }
  
  if (cfg.pulseRelayIdx >= 0 && cfg.pulseRelayIdx <= 3) {
    if (isPulseCurrentlyOn) {
      if ((long)(millis() - pulseOffAtMs) >= 0) { 
        logSerial("TIMER: Impuls-Dauer abgelaufen. Schalte Relais " + String(cfg.pulseRelayIdx + 1) + " aus.");
        setRelayState(cfg.pulseRelayIdx, false); 
        isPulseCurrentlyOn = false; 
      }
    } else if (!blocked && isWithinTimeWindow()) {
      if (millis() - lastPulseTriggerMs >= (unsigned long)cfg.pulseIntMin * 60000UL) {
        lastPulseTriggerMs = millis(); 
        logSerial("TIMER: Zyklischer Impuls ausgeloest fuer Relais " + String(cfg.pulseRelayIdx + 1) + " (" + String(cfg.pulseDurMs) + " ms).");
        setRelayState(cfg.pulseRelayIdx, true);
        pulseOffAtMs = millis() + cfg.pulseDurMs; 
        isPulseCurrentlyOn = true;
      }
    }
  }
  server.handleClient();
}
