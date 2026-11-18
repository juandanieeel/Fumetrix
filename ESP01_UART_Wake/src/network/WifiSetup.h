#pragma once
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <stddef.h>

// Separate networking state machine; never writes diagnostics to Arduino UART.
class WifiSetup {
 public:
  explicit WifiSetup(ESP8266WebServer& server) : server_(server) {}
  void begin(const char* apName, const char* apPassword) {
    apName_=apName; apPassword_=apPassword;
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false); // Reconnection is scheduled below.
    EEPROM.begin(512);
    EEPROM.get(0, config_);
    if (!validConfig()) memset(&config_, 0, sizeof(config_));
    token_=String(ESP.getCycleCount(), HEX)+String(micros(), HEX);
    server_.on("/wifi", HTTP_GET, [this](){ page(); });
    server_.on("/wifi-status", HTTP_GET, [this](){
      server_.sendHeader("Cache-Control", "no-store");
      server_.send(200, "text/plain", status());
    });
    server_.on("/wifi-save", HTTP_POST, [this](){ save(); });
    if(config_.ssid[0]) connect(); else { openAP(); message_="No saved Wi-Fi. Enter your router or hotspot credentials."; }
  }
  void tick() {
    const unsigned long now=millis();
    if(pending_ && now-pendingAt_>=500UL){pending_=false;connect();}
    if(pending_) return;
    switch(net_) {
      case Net::CONNECTING:
        if(WiFi.status()==WL_CONNECTED) {
          net_=Net::CONNECTED;
          message_="Connected to Wi-Fi. Internet access has not been tested.";
        } else if(now-attemptAt_>=20000UL) {
          WiFi.disconnect();openAP();retryAt_=now;
          message_="Connection failed. Check name/password and use 2.4 GHz Wi-Fi. Setup AP is available.";
        }
        break;
      case Net::CONNECTED:
        if(WiFi.status()!=WL_CONNECTED){
          WiFi.disconnect();openAP();retryAt_=now;
          message_="Wi-Fi disconnected. Setup AP is available; retrying in 30 seconds.";
        }
        break;
      case Net::AP_READY:
        if(config_.ssid[0] && now-retryAt_>=30000UL) connect();
        break;
    }
  }
  const char* oledMode() const {
    if (WiFi.status() == WL_CONNECTED) return apOpen_ ? "AP+STA" : "STA";
    if (apOpen_) return "AP";
    return net_ == Net::CONNECTING ? "JOINING" : "OFFLINE";
  }
  String status() const {
    String out=message_;
    out+="\nSTA: ";out+=(WiFi.status()==WL_CONNECTED ? "CONNECTED" : "NOT CONNECTED");
    if(WiFi.status()==WL_CONNECTED){
      out+="\nRouter IP: "+WiFi.localIP().toString();
      out+="\nLocal control page: http://"+WiFi.localIP().toString()+"/";
      out+="\nRSSI: "+String(WiFi.RSSI())+" dBm";
    }
    out+="\nSetup AP: ";out+=(apOpen_ ? apName_ : "off (STA mode)");
    if(apOpen_)out+="\nSetup page: http://"+WiFi.softAPIP().toString()+"/wifi";
    out+="\nSaved network: ";out+=(config_.ssid[0] ? config_.ssid : "none");
    out+="\nCloudflare dashboard bridge: not installed in this test.";
    return out;
  }
 private:
  enum class Net : uint8_t { AP_READY, CONNECTING, CONNECTED };
  struct Config { uint32_t magic; char ssid[33]; char password[65]; uint32_t checksum; };
  Config config_{};
  ESP8266WebServer& server_;
  const char* apName_=nullptr;
  const char* apPassword_=nullptr;
  String token_,message_;
  Net net_=Net::AP_READY;
  bool apOpen_=false,pending_=false;
  unsigned long attemptAt_=0,retryAt_=0,pendingAt_=0;
  uint32_t checksum(const Config& c) const {
    const uint8_t* bytes=reinterpret_cast<const uint8_t*>(&c);
    uint32_t hash=2166136261UL;
    for(size_t i=0;i<offsetof(Config,checksum);i++){hash^=bytes[i];hash*=16777619UL;}
    return hash;
  }
  bool validConfig() const {
    return config_.magic==0x46574D31UL && memchr(config_.ssid,0,sizeof(config_.ssid)) &&
      memchr(config_.password,0,sizeof(config_.password)) && config_.ssid[0] &&
      config_.checksum==checksum(config_);
  }
  void openAP() {
    WiFi.mode(WIFI_AP_STA);
    if(!apOpen_) {
      WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
      apOpen_=WiFi.softAP(apName_,apPassword_);
    }
    net_=Net::AP_READY;
  }
  void connect() {
    WiFi.mode(apOpen_ ? WIFI_AP_STA : WIFI_STA);
    WiFi.disconnect();
    WiFi.begin(config_.ssid,config_.password);
    net_=Net::CONNECTING;attemptAt_=millis();
    message_="Connecting to saved Wi-Fi (up to 20 seconds)...";
  }
  void save() {
    if(server_.arg("token")!=token_){server_.send(403,"text/plain","Reopen /wifi and try again.");return;}
    if(pending_){server_.send(409,"text/plain","A Wi-Fi change is pending. Please wait.");return;}
    String ssid=server_.arg("ssid"),password=server_.arg("password");
    if(ssid.length()<1 || ssid.length()>32 ||
       !(password.length()==0 || (password.length()>=8 && password.length()<=63))) {
      server_.send(400,"text/plain","SSID must be 1-32 bytes. Password must be 8-63 bytes, or empty for an open network. Go back and correct it.");return;
    }
    Config candidate{};candidate.magic=0x46574D31UL;
    ssid.toCharArray(candidate.ssid,sizeof(candidate.ssid));
    password.toCharArray(candidate.password,sizeof(candidate.password));
    candidate.checksum=checksum(candidate);
    const bool changed=memcmp(&candidate,&config_,sizeof(Config))!=0;
    if(changed){
      EEPROM.put(0,candidate);
      if(!EEPROM.commit()){server_.send(500,"text/plain","Could not save Wi-Fi settings. Please retry.");return;}
    }
    config_=candidate;
    pending_=true;pendingAt_=millis();
    server_.sendHeader("Cache-Control","no-store");
    server_.send(200,"text/html","<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><h2>Wi-Fi settings saved</h2><p>Connecting shortly. If disconnected, reconnect to Fumetrix-Wake and open 192.168.4.1/wifi. If the setup AP is off, join your router network and use the ESP IP from your router client list.</p><a href='/wifi'>Check connection</a>");
  }
  void page() {
    String html=F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Fumetrix Wi-Fi setup</title><style>body{font:16px Arial;max-width:600px;margin:30px auto;padding:20px;background:#151b1d;color:#eee}input,button{box-sizing:border-box;width:100%;padding:12px;margin:8px 0 18px;font-size:16px}button{background:#c9834a;border:0}a{color:#e2ac7d}pre{white-space:pre-wrap;line-height:1.6}</style></head><body><h2>Fumetrix Wi-Fi setup</h2><p>Connect to a 2.4 GHz router or phone hotspot. Settings survive power-off.</p><form method='post' action='/wifi-save'><input type='hidden' name='token' value='");
    html+=token_;
    html+=F("'><label for='ssid'>Wi-Fi name (SSID)</label><input id='ssid' name='ssid' maxlength='32' required autocomplete='off'><label for='password'>Wi-Fi password</label><input id='password' name='password' type='password' maxlength='63' autocomplete='new-password'><p>Leave password empty only for an open network.</p><button>Save and connect</button></form><h3>Connection status</h3><pre id='status'>Loading...</pre><a href='/'>SHT40 and sleep/wake controls</a><script>async function refresh(){try{const r=await fetch('/wifi-status',{cache:'no-store'});document.getElementById('status').textContent=await r.text();}catch(e){document.getElementById('status').textContent='Connection changed. Reconnect to Fumetrix-Wake if necessary.';}setTimeout(refresh,2000);}refresh();</script></body></html>");
    server_.sendHeader("Cache-Control","no-store");
    server_.send(200,"text/html",html);
  }
};
