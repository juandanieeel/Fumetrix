#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include "FumetrixTelemetry.h"
#include "src/network/WifiSetup.h"
#include <math.h>
#include "SdResetKey.h"

String reportedLimits;
String sdInfo;
String sdRow;
unsigned long buzzerReadyAt = 0;
bool buzzerReady = false;
String buzzerAck;

bool validSound(const String& value) {
  return value == "test" || value == "stop" || value == "birthday" ||
         value == "jingle" || value == "twinkle" || value == "ode";
}
unsigned long limitsReplyAt = 0;

const char* apName = "Fumetrix-Wake";
const char* apPassword = AP_SETUP_PASSWORD;
const byte WAKE_PIN = 2;
const unsigned long WAKE_PULSE_MS = 1; // Change to 100 for the old visible flash.
// NON-INVERTING AO3400A shifter: gate=3.3V, source=GPIO2, drain=Arduino D2.
ESP8266WebServer server(80);
WifiSetup wifiSetup(server);
LineReceiver receiver;
GasIndices gasIndices;
ParticleReadings particles;
enum class State : byte { UNKNOWN, AWAKE, SLEEPING, SLEEP_PENDING, WAKING };
State state = State::UNKNOWN;
String operatingMode = "UNKNOWN", oledState = "UNKNOWN";
unsigned long modeReplyAt = 0, oledReplyAt = 0;
bool maintenanceReady() {
  return state == State::AWAKE && operatingMode == "MAINTENANCE" && millis()-modeReplyAt < 6000UL;
}
unsigned long sentAt = 0, lastPoll = 0, lastReply = 0;
String notice = "Waiting for Arduino.";
int fanRpm = -1;
unsigned long rpmReplyAt = 0;
String autoFanOverride;
unsigned long autoFanReplyAt = 0;
const char* shtStatus = "WAITING";
int speedDuty = -1;
unsigned long speedReplyAt = 0;
String fanState = "UNKNOWN";
bool fanPending = false;
bool fanTarget = false;
unsigned long fanSentAt = 0, fanReplyAt = 0;
String wakeLevel = "unknown";
bool sensorValid = false;
float temperature = 0, humidity = 0;
unsigned long lastSample = 0;
String sensorNotice = "Waiting for SHT40 reading.";

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Fumetrix SHT40 sleep test</title><style>body{font:18px Arial;max-width:600px;margin:40px auto;padding:20px}button{font-size:22px;padding:16px;margin:8px}pre{white-space:pre-wrap}</style></head>
<body><p><a href="/wifi">Wi-Fi setup (AP / STA)</a></p><h2>Fumetrix SHT40 sleep / wake test</h2><button onclick="send('sleep')">Sleep Arduino</button>
<button onclick="send('wake')">Wake Arduino</button><button onclick="send('fan/on')">Fan ON</button><button onclick="send('fan/off')">Fan OFF</button><button onclick="send('mode?value=MAINTENANCE')">MAINTENANCE</button><button onclick="send('mode?value=AUTO')">AUTO</button><pre id="result">Connecting...</pre>
<script>
let busy=false;
async function refresh(){if(busy)return;try{document.getElementById('result').textContent=await(await fetch('/status',{cache:'no-store'})).text();}catch(e){document.getElementById('result').textContent='ESP connection lost';}}
async function send(action){if(busy)return;busy=true;try{document.getElementById('result').textContent=await(await fetch('/'+action,{method:'POST'})).text();}catch(e){document.getElementById('result').textContent='Request failed';}busy=false;}
setInterval(refresh,1000);refresh();
</script></body></html>)HTML";

void wakeArduino() {
  digitalWrite(WAKE_PIN, LOW); // Source LOW -> drain/D2 LOW. Blue LED ON.
  delay(WAKE_PULSE_MS);        // Requested 1 ms. Too short to see clearly.
  digitalWrite(WAKE_PIN, HIGH); // Idle HIGH, blue LED OFF.
}
const char* stateName() {
  switch(state) {
    case State::AWAKE: return "AWAKE";
    case State::SLEEPING: return "SLEEP preparation acknowledged";
    case State::SLEEP_PENDING: return "Waiting for sleep acknowledgement";
    case State::WAKING: return "Waiting for wake acknowledgement";
    default: return "UNKNOWN (no confirmed Arduino state)";
  }
}
void requestFan(bool on) {
  if(!maintenanceReady() || fanPending) {
    server.send(409,"text/plain","Arduino must be awake and no fan command pending."); return;
  }
  fanTarget=on;fanPending=true;fanSentAt=millis();fanState="PENDING";
  Serial.println(on ? "FAN_ON" : "FAN_OFF");
  server.send(202,"text/plain","Fan request sent. Waiting for Arduino.");
}
void requestFanSpeed() {
  if (!maintenanceReady() || fanPending || speedDuty < 0 || millis() - speedReplyAt >= 6000UL) {
    server.send(409, "text/plain", "Fan speed control is unavailable."); return;
  }
  const String setting = server.arg("value");
  char* end;
  const char* value = setting.c_str();
  const long duty = strtol(value, &end, 10);
  if (end == value || *end != '\0' || duty < 20 || duty > 100) {
    server.send(400, "text/plain", "Use a duty from 20 to 100."); return;
  }
  Serial.print("SPEED,");
  Serial.println(setting);
  server.send(202, "text/plain", "Waiting for the Arduino speed setting.");
}


// Reuse the exact gas status published to the website. The OLED never runs
// a second independent algorithm or shows raw sensor ticks as an index.
void sendOledGas() {
  static unsigned long lastSent = 0;
  if (state != State::AWAKE || millis() - lastSent < 1000UL) return;
  lastSent = millis();
  gasIndices.sendToOled(Serial);
}

void setup() {
  digitalWrite(WAKE_PIN, HIGH);
  pinMode(WAKE_PIN, OUTPUT);
  Serial.begin(9600);
  gasIndices.reset("WAITING");
  wifiSetup.begin(apName, apPassword);
  server.on("/", HTTP_GET, [](){server.send_P(200,"text/html",PAGE);});
  server.on("/limits", HTTP_POST, [](){
    if (state != State::AWAKE) {
      server.send(409, "text/plain", "Arduino is not awake.");
      return;
    }
    String packet = server.arg("values");
    FumetrixLimits limits;
    if (packet.length() > 40 || !limits.parse(packet.c_str())) {
      server.send(400, "text/plain", "Invalid thresholds.");
      return;
    }
    Serial.print("LIMITS,");
    Serial.println(packet);
    server.send(202, "text/plain", "Waiting for Arduino threshold confirmation.");
  });
  server.on("/buzzer", HTTP_POST, [](){
    const String value = server.arg("value");
    if (!validSound(value)) { server.send(400, "text/plain", "Invalid buzzer selection."); return; }
    if (state != State::AWAKE || !buzzerReady || millis() - buzzerReadyAt >= 6000UL ||
        (value == "test" && !maintenanceReady())) {
      server.send(409, "text/plain", "Buzzer unavailable in the current state.");
      return;
    }
    buzzerAck = "";
    Serial.print("BUZZER,");
    Serial.println(value);
    server.send(202, "text/plain", "Waiting for Arduino buzzer acknowledgement.");
  });
  server.collectHeaders("X-Fumetrix-SD-Key");
  server.on("/sd-reset", HTTP_POST, [](){
    if (server.header("X-Fumetrix-SD-Key") != SD_RESET_KEY) {
      server.send(403, "text/plain", "SD reset authentication failed."); return;
    }
    const String expected = server.arg("expected");
    const String replacement = server.arg("replacement");
    if (state != State::AWAKE || !sdInfo.length() || sdInfo == "ERROR" ||
        !expected.length() || expected != sdInfo) {
      server.send(409, "text/plain", "SD log changed or is unavailable."); return;
    }
    if (!replacement.length() || replacement.length() > 10 ||
        strspn(replacement.c_str(), "0123456789") != replacement.length()) {
      server.send(400, "text/plain", "Invalid new log ID."); return;
    }
    Serial.print(F("SDRESET,"));
    Serial.print(expected);
    Serial.print(',');
    Serial.println(replacement);
    server.send(202, "text/plain", "SD reset requested.");
  });
  server.on("/sd-read", HTTP_POST, [](){
    const String index = server.arg("slot");
    if (state != State::AWAKE || !sdInfo.length() || sdInfo == "ERROR") {
      server.send(409, "text/plain", "SD unavailable."); return;
    }
    if (!index.length() || index.length() > 4 ||
        strspn(index.c_str(), "0123456789") != index.length() ||
        index.toInt() >= 4096) {
      server.send(400, "text/plain", "Invalid slot."); return;
    }
    Serial.print(F("SDGET,"));
    Serial.println(index);
    server.send(202, "text/plain", "SD record requested.");
  });
  server.on("/status", HTTP_GET, [](){
    String out = String("Arduino: ") + stateName() + "\nLast reported D2: " + wakeLevel + "\n" + notice;
    out += "\nMode: " + (state == State::SLEEPING ? String("SLEEP") :
      (state == State::AWAKE && millis()-modeReplyAt < 6000UL ? operatingMode : String("UNKNOWN")));
    out += "\nOLED: " + (state == State::SLEEPING ? String("OFF") :
      (state == State::AWAKE && millis()-oledReplyAt < 6000UL ? oledState : String("UNKNOWN")));
    if (state == State::AWAKE && reportedLimits.length() && millis() - limitsReplyAt < 6000UL) {
      out += "\nOLED limits: " + reportedLimits;
    }
    if (state == State::AWAKE && buzzerReady && millis() - buzzerReadyAt < 6000UL) {
      out += "\nBuzzer: READY";
      if (buzzerAck.length()) out += "\nBuzzer ACK: " + buzzerAck;
    }
    if (state == State::AWAKE && sdInfo.length()) {
      out += "\nSD reset: supported";
      out += "\nSD log: " + sdInfo;
      if (sdRow.length()) out += "\nSD row: " + sdRow;
    }
    bool fresh = sensorValid && state == State::AWAKE && millis()-lastSample < 6000UL;
    if (state == State::AWAKE && autoFanOverride.length() && millis()-autoFanReplyAt < 6000UL) {
      out += "\nFan override: " + autoFanOverride;
    }
    out += "\nSHT40: ";
    out += state == State::SLEEPING ? "OFF" : state != State::AWAKE ? "WAITING" :
           fresh ? "READY" : sensorValid ? "STALE" : shtStatus;
    if(fresh) {
      out += "\n\nTemperature: " + String(temperature,2) + " C";
      out += "\nHumidity: " + String(humidity,2) + " %";
      out += "\nReading age: " + String((millis()-lastSample)/1000UL) + " s";
    } else {
      out += "\n\nTemperature: --\nHumidity: --\n";
      out += state == State::SLEEPING ? "Sensor power is OFF." : sensorNotice;
    }
    out += "\nFan relay: " + ((state==State::AWAKE && millis()-fanReplyAt<6000UL) || state==State::SLEEPING ? fanState : String("UNKNOWN"));
    if (state == State::AWAKE && speedDuty >= 0 && millis() - speedReplyAt < 6000UL) {
      out += "\nFan speed: WEB," + String(speedDuty);
    }
    if (state == State::AWAKE && fanState == "ON" && fanRpm >= 0 &&
        millis() - rpmReplyAt < 3500UL) {
      out += "\nFan RPM: " + String(fanRpm);
    }
    gasIndices.appendStatus(out, state == State::AWAKE, state == State::SLEEPING);
    particles.appendStatus(out, state == State::AWAKE, state == State::SLEEPING);
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"text/plain",out);
  });
  server.on("/sleep", HTTP_POST, [](){
    if (state != State::AWAKE || fanPending) {server.send(409,"text/plain","Arduino must first be confirmed awake.");return;}
    state=State::SLEEP_PENDING;sentAt=millis();
    notice="UART SLEEP sent. Waiting for Arduino.";
    Serial.println(server.arg("shutdown") == "1" ? "SHUTDOWN" : "SLEEP");
    server.send(202,"text/plain",notice);
  });
  server.on("/wake", HTTP_POST, [](){
    if (fanPending || state==State::SLEEP_PENDING || state==State::WAKING) {server.send(409,"text/plain","Wait for the current command to finish.");return;}
    state=State::WAKING;sentAt=millis();
    wakeArduino();Serial.println("STATUS");
    notice="LOW wake pulse sent. Waiting for Arduino acknowledgement.";
    server.send(202,"text/plain",notice);
  });
  server.on("/mode", HTTP_POST, [](){
    String value = server.arg("value");
    if (value != "AUTO" && value != "MAINTENANCE") { server.send(400,"text/plain","Invalid mode."); return; }
    if (state != State::AWAKE || fanPending) { server.send(409,"text/plain","Wake the Arduino first."); return; }
    operatingMode = "UNKNOWN";
    Serial.print("MODE,"); Serial.println(value);
    server.send(202,"text/plain","Waiting for Arduino mode acknowledgement.");
  });
  server.on("/oled", HTTP_POST, [](){
    String value = server.arg("value");
    if (value != "ON" && value != "OFF") { server.send(400,"text/plain","Invalid display state."); return; }
    if (!maintenanceReady()) { server.send(409,"text/plain","Select MAINTENANCE first."); return; }
    oledState = "UNKNOWN";
    Serial.print("OLED,"); Serial.println(value);
    server.send(202,"text/plain","Waiting for OLED acknowledgement.");
  });
  server.on("/fan/override", HTTP_POST, [](){
    if (state != State::AWAKE || operatingMode != "AUTO" || millis()-modeReplyAt >= 6000UL ||
        fanPending || !autoFanOverride.length() || millis()-autoFanReplyAt >= 6000UL) {
      server.send(409, "text/plain", "Wait for AUTO and fan status."); return;
    }
    if (autoFanOverride == "MANUAL") {
      server.send(409, "text/plain", "The A0 knob controls the fan in Manual."); return;
    }
    const String value = server.arg("value");
    if (value != "ON" && value != "OFF" && value != "AUTO") {
      server.send(400, "text/plain", "Invalid fan override."); return;
    }
    autoFanOverride = ""; // Publish only after the Arduino reports the setting again.
    Serial.print(F("AUTOFAN,"));
    Serial.println(value);
    server.send(202, "text/plain", "Waiting for Arduino fan confirmation.");
  });
  server.on("/fan/speed", HTTP_POST, requestFanSpeed);
  server.on("/fan/on", HTTP_POST, [](){requestFan(true);});
  server.on("/fan/off", HTTP_POST, [](){requestFan(false);});
  server.begin();
}
void loop() {
  if(receiver.poll(Serial)) {
    const char* line=receiver.line();
    if (!strncmp(line, "SD,", 3)) {
      const int separator = sdInfo.indexOf(',');
      if (separator < 0 || strncmp(line + 3, sdInfo.c_str(), separator + 1)) sdRow = "";
      sdInfo = line + 3;
    } else if (!strncmp(line, "SDROW,", 6)) {
      sdRow = line + 6;
    }
    if (!strcmp(line, "BUZZER,READY")) {
      buzzerReady = true;
      buzzerReadyAt = millis();
    } else if (!strncmp(line, "BUZZER,", 7) && validSound(line + 7)) {
      buzzerAck = line + 7;
    }
    if (!strncmp(line, "LIMITS,", 7)) {
      FumetrixLimits limits;
      if (!strcmp(line + 7, "UNSET") || limits.parse(line + 7)) {
        reportedLimits = line + 7;
        limitsReplyAt = millis();
      }
    }
    if (!strcmp(line,"MODE,AUTO") || !strcmp(line,"MODE,MAINTENANCE")) {
      operatingMode = line + 5; modeReplyAt = millis();
    }
    if (!strcmp(line,"OLED,ON") || !strcmp(line,"OLED,OFF") || !strcmp(line,"OLED,MISSING") || !strcmp(line,"OLED,DISABLED")) {
      oledState = line + 5; oledReplyAt = millis();
    }
    if(state==State::AWAKE) particles.receive(line);
    if (state == State::AWAKE || !strncmp(line, "GAS,", 4)) gasIndices.receive(line);
    if (!strcmp(line, "AUTOFAN,AUTO") || !strcmp(line, "AUTOFAN,ON") || !strcmp(line, "AUTOFAN,OFF") ||
        !strcmp(line, "AUTOFAN,MANUAL")) {
      autoFanOverride = line + 8;
      autoFanReplyAt = millis();
    }
    if (strncmp(line, "SPEED,WEB,", 10) == 0) {
      const char* value = line + 10;
      char* end;
      const long duty = strtol(value, &end, 10);
      if (end != value && *end == '\0' && duty >= 20 && duty <= 100) {
        speedDuty = duty;
        speedReplyAt = millis();
      }
    }
    if (strncmp(line, "RPM,", 4) == 0 && state == State::AWAKE && fanState == "ON") {
      const char* value = line + 4;
      char* end;
      const long rpm = strtol(value, &end, 10);
      if (end != value && *end == '\0' && rpm >= 0 && rpm <= 30000) {
        fanRpm = rpm;
        rpmReplyAt = millis();
      }
    }
    if(strcmp(line,"FAN,ON")==0 || strcmp(line,"FAN,OFF")==0) {
      bool on=strcmp(line,"FAN,ON")==0;
      fanReplyAt=lastReply=millis();
      if (!fanPending || on == fanTarget) {
        if (!on || fanState != "ON") fanRpm = -1;
        fanState = on ? "ON" : "OFF";
        fanPending = false;
      }
    }
    if(strncmp(line,"DATA,",5)==0) {
      char *endT, *endH;
      float t = strtof(line+5, &endT);
      if(endT != line+5 && *endT == ',') {
        float h = strtof(endT+1, &endH);
        if(endH != endT+1 && *endH == '\0' && isfinite(t) && isfinite(h) &&
           t >= -45 && t <= 130 && h >= 0 && h <= 100 && state == State::AWAKE) {
          temperature=t;humidity=h;lastSample=lastReply=millis();sensorValid=true;
          sensorNotice="No recent SHT40 reading.";
        }
      }
    } else if(strcmp(line,"ERR,SHT_MISSING")==0 || strcmp(line,"ERR,SHT_READ")==0) {
      sensorValid=false;lastReply=millis();
      shtStatus = !strcmp(line, "ERR,SHT_MISSING") ? "MISSING" : "ERROR";
      sensorNotice="SHT40 error: check switched 3.3 V, SDA/SCL and address 0x44.";
    }
    if(strcmp(line,"STATE,AWAKE")==0) {
      lastReply=millis();
      if(state!=State::SLEEP_PENDING) {
        if(state!=State::AWAKE) notice="Arduino awake; A1/A2 ON.";
        state=State::AWAKE;
      }
    } else if(strcmp(line,"STATE,SLEEP")==0) {
      buzzerAck = "";
      buzzerReady = false;
      sdInfo = "";
      sdRow = "";
      reportedLimits = "";
      autoFanOverride = "";
      shtStatus = "WAITING";
      gasIndices.reset("OFF");
      particles.reset();
      fanRpm = -1;
      sensorValid=false;sensorNotice="Waiting for fresh SHT40 reading after wake.";
      lastReply=millis();state=State::SLEEPING;
      notice="Arduino switched A1/A2 OFF and is entering power-down.";
    } else if(strcmp(line,"ERR,WAKE_LOW")==0) {
      lastReply=millis();state=State::AWAKE;wakeLevel="LOW";
      notice="Sleep refused: D2 is LOW. Check AO3400A gate/source wiring.";
    } else if(strcmp(line,"PIN,HIGH")==0) wakeLevel="HIGH";
    else if(strcmp(line,"PIN,LOW")==0) wakeLevel="LOW";
  }
  if((state==State::SLEEP_PENDING || state==State::WAKING) && millis()-sentAt>=5000UL) {
    state=State::UNKNOWN;notice="No acknowledgement: check UART wiring, baud rate and wake signal.";
  }
  if(state==State::AWAKE && millis()-lastReply>=10000UL) {
    state=State::UNKNOWN;notice="Arduino UART stopped responding.";
  }
  if(state!=State::SLEEPING && state!=State::SLEEP_PENDING && millis()-lastPoll>=2000UL) {
    lastPoll=millis();Serial.println("STATUS");
  }
  if(fanPending && millis()-fanSentAt>=5000UL) {fanPending=false;fanState="UNKNOWN";}
  sendOledGas();
  wifiSetup.tick();
  static unsigned long lastWifiReport = 0;
  if (state == State::AWAKE && millis() - lastWifiReport >= 1000UL) {
    lastWifiReport = millis();
    Serial.print(F("OLEDWIFI,"));
    Serial.println(wifiSetup.oledMode());
  }
  server.handleClient();
  yield();
}
