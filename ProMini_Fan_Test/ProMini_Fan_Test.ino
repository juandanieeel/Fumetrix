#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>
#include <Wire.h>
#include "FumetrixProtocol.h"
#include "FumetrixSensors.h"
#include "FumetrixBuzzer.h"
#include "AutoFan.h"
#include "src/storage/SdJournal.h"

int16_t logTemp;
uint16_t logHumidity;
unsigned long climateAt;
bool climateReady;
unsigned long lastSdLog;
bool haveSdLog;

AutoFan autoFan;
GasReading gasReading;
enum class WifiDisplayMode : byte { Unknown, AP, STA, Both, Joining, Offline };
WifiDisplayMode wifiDisplayMode = WifiDisplayMode::Unknown;
unsigned long wifiReportedAt = 0;

void receiveWifiDisplayMode(const char* value) {
  if (!strcmp_P(value, PSTR("AP"))) wifiDisplayMode = WifiDisplayMode::AP;
  else if (!strcmp_P(value, PSTR("STA"))) wifiDisplayMode = WifiDisplayMode::STA;
  else if (!strcmp_P(value, PSTR("AP+STA"))) wifiDisplayMode = WifiDisplayMode::Both;
  else if (!strcmp_P(value, PSTR("JOINING"))) wifiDisplayMode = WifiDisplayMode::Joining;
  else if (!strcmp_P(value, PSTR("OFFLINE"))) wifiDisplayMode = WifiDisplayMode::Offline;
  else return;
  wifiReportedAt = millis();
}

PGM_P wifiDisplayLabel() {
  if (millis() - wifiReportedAt < 3500UL) {
    switch (wifiDisplayMode) {
      case WifiDisplayMode::AP: return PSTR("AP");
      case WifiDisplayMode::STA: return PSTR("STA");
      case WifiDisplayMode::Both: return PSTR("AP+STA");
      case WifiDisplayMode::Joining: return PSTR("JOINING");
      case WifiDisplayMode::Offline: return PSTR("OFFLINE");
      case WifiDisplayMode::Unknown: break;
    }
  }
  return PSTR("WiFi ?");
}

bool sleepRequested = false;

FumetrixProtocol protocol(Serial);
enum class OperatingMode : byte { AUTO, MAINTENANCE, SLEEP };
void selectAwakeMode(OperatingMode mode, bool usePot);
OperatingMode operatingMode = OperatingMode::AUTO;
// Set to 1 only if you want the existing OLED test enabled (requires U8g2).
#include "FumetrixLimits.h"
FumetrixLimits displayLimits;

#define USE_OLED 1
#if USE_OLED
#include "OledDisplay.h"
#else
class OledDisplay {
 public:
 const char* status() const { return "DISABLED"; } void setEnabled(bool) {}
 void begin() {} void end() {} void reading(int16_t,uint16_t) {} void error(const char*) {}
 void showFanSpeed() {} void clearFanSpeed() {}
 void tick(bool,byte,long,long,const char*,long,long,const GasReading&,PGM_P,bool,bool) {}
};
#endif
OledDisplay display;
const byte FAN_RELAY_PIN = 10;
const byte RELAY_ON = LOW;  // Release the coil to run the fan through NC
const byte RELAY_OFF = (RELAY_ON == HIGH) ? LOW : HIGH;
bool fanOn = false;
const byte FAN_PWM_PIN = 6;
const byte MIN_FAN_DUTY = 20;
const uint16_t PWM_COUNTS = F_CPU / 25000UL;
static_assert(F_CPU == 16000000UL, "Select the 16 MHz Pro Mini.");
byte fanDuty = 100;
byte manualFanDuty = 100;
byte appliedDuty = 255;
Tachometer tachometer;

// D6 drives the NPN. HIGH at D6 pulls the fan's PWM input LOW.
inline __attribute__((always_inline)) void driveFanPwm() {
  if (TCNT1 < OCR1B) PORTD &= ~_BV(PD6);
  else PORTD |= _BV(PD6);
}

ISR(TIMER1_COMPA_vect) {
  // Compare A can also run with the fan at full duty to clock the buzzer.
  if (TIMSK1 & _BV(OCIE1B)) driveFanPwm();
  buzzerTimerTick();
}

ISR(TIMER1_COMPB_vect) {
  // Either interrupt can be late. Use the current phase, even after wrap.
  driveFanPwm();
}

void applyFanDuty() {
  const byte duty = fanOn ? fanDuty : 100;
  if (duty == appliedDuty) return;
  const uint16_t compare = uint16_t(PWM_COUNTS * duty / 100U);

  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    TIMSK1 = 0;
    TIFR1 = _BV(OCF1A) | _BV(OCF1B);
    if (duty < 100) {
      OCR1B = compare;
      driveFanPwm();
      TIMSK1 = _BV(OCIE1A) | _BV(OCIE1B);
    } else {
      PORTD &= ~_BV(PD6);
      if (buzzerPeriod) TIMSK1 = _BV(OCIE1A);
    }
  }

  appliedDuty = duty;
}


void setupFanPwm() {
  digitalWrite(FAN_PWM_PIN, LOW);
  pinMode(FAN_PWM_PIN, OUTPUT);

  TCCR1B = 0;
  TIMSK1 = 0;
  TCCR1A = 0;
  TCNT1 = 0;
  OCR1A = PWM_COUNTS - 1;
  TCCR1B = _BV(WGM12) | _BV(CS10);

  applyFanDuty();
}

void reportFanSpeed() {
  protocol.speed(fanDuty);
}

void setWebFanSpeed(const char* setting) {
  char* end;
  const long duty = strtol(setting, &end, 10);
  if (end == setting || *end != '\0' || duty < MIN_FAN_DUTY || duty > 100) return;
  fanDuty = manualFanDuty = duty;
  applyFanDuty();
  reportFanSpeed();
  display.showFanSpeed();
}

void reportFan() { protocol.fan(fanOn); }
void setFan(bool on, bool playChime) {
  const bool changed = fanOn != on;
  if (changed) tachometer.setEnabled(on);
  fanOn = on;
  applyFanDuty();
  digitalWrite(FAN_RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
  if (changed && playChime) {
    buzzerPlay(on ? BuzzerSound::FanStart : BuzzerSound::FanStop);
  }
  reportFan();
}

enum class FanOverride : byte { Automatic, On, Off, Manual };
FanOverride fanOverride = FanOverride::Automatic;

void reportFanOverride() {
  Serial.println(fanOverride == FanOverride::Manual ? F("AUTOFAN,MANUAL") :
                 fanOverride == FanOverride::On ? F("AUTOFAN,ON") :
                 fanOverride == FanOverride::Off ? F("AUTOFAN,OFF") : F("AUTOFAN,AUTO"));
}

void setAutoFanOverride(const char* value) {
  if (!strcmp_P(value, PSTR("ON"))) fanOverride = FanOverride::On;
  else if (!strcmp_P(value, PSTR("OFF"))) fanOverride = FanOverride::Off;
  else if (!strcmp_P(value, PSTR("AUTO"))) fanOverride = FanOverride::Automatic;
  else return;

  autoFan = AutoFan{};
  if (fanOverride != FanOverride::On) setFan(false, true);
  reportFanOverride();
}

void runAutoFan(long pm25) {
  const bool keepRunning = fanOverride == FanOverride::On;
  if (!displayLimits.configured && !keepRunning) return;
  const unsigned long now = millis();
  const bool freshGas = gasReading.fresh(now);
  byte duty = AutoFan::KEEP_RUNNING_DUTY;
  if (displayLimits.configured) {
    duty = autoFan.update(now, pm25,
      freshGas ? gasReading.voc : 0, freshGas ? gasReading.nox : 0,
      displayLimits.values[4], displayLimits.values[5], displayLimits.values[6],
      keepRunning);
  }
  if (duty && fanDuty != duty) {
    fanDuty = duty;
    applyFanDuty();
    reportFanSpeed();
  }
  if ((duty != 0) != fanOn) {
    setFan(duty != 0, true);
  }
}

const byte WAKE_PIN = 2;
const byte MODE_BUTTON_PIN = 3; // Active LOW, with the internal pull-up.
const byte FAN_BUTTON_PIN = 4;
const byte FAN_POT_PIN = A0;
unsigned long modeButtonChangedAt = 0;
volatile bool modePinChanged = true;
bool modeButtonLow = false;
bool modeButtonHandled = true; // Require release at startup and after wake.
unsigned long lastPotRead = 0;
unsigned long fanButtonChangedAt = 0;
bool fanButtonLow = false;
bool fanButtonHandled = false;
const byte GATE_5V = A1;
const byte GATE_3V3 = A2;
Sht40Sensor sht;
Sgp41Sensor gas(protocol);
Pms7003Sensor pms(protocol);
bool sensorInitialized = false;
unsigned long sensorPowerStarted = 0;
unsigned long lastReading = 0;
byte shtFailures = 0;

void restartSensor() {
  pms.begin(); // Restart 30-second settling period on every power cycle.
  sensorInitialized = false;
  climateReady = false;
  shtFailures = 0;
  sensorPowerStarted = millis();
}

void shtReadFailed(bool readError) {
  lastReading = millis();
  if (shtFailures < 3) ++shtFailures;
  // One or two transient bus errors do not invalidate a fresh reading.
  if (shtFailures == 3 && (!climateReady || lastReading - climateAt >= 5000UL)) {
    climateReady = false;
    ++shtFailures; // Report once until a valid sample or sensor restart.
    protocol.climateError(readError);
    display.error(readError ? "Read/CRC error" : "SHT40 missing");
  }
}

void updateSensor() {
  if (!sensorInitialized) {
    if (millis() - sensorPowerStarted < 20UL) return;
    sht.begin();
    display.begin();
    // Display setup may initialize Wire again: restore the shared bus settings.
    Wire.setClock(100000UL);
    Wire.setWireTimeout(25000UL, true);
    gas.begin();
    sensorInitialized = true;
    lastReading = millis() - 2000UL;
  }
  if (sht.ready()) {
    ClimateSample sample;
    if (sht.read(sample)) {
      shtFailures = 0;
      logTemp = sample.temp100; logHumidity = sample.rh100; climateAt = millis(); climateReady = true;
      gas.compensation(sample);
      protocol.climate(logTemp, logHumidity);
      display.reading(logTemp, logHumidity);
    } else {
      shtReadFailed(true);
    }
  }
  const unsigned long interval = shtFailures && shtFailures < 3 ? 100UL : 2000UL;
  if (!sht.busy() && millis() - lastReading >= interval) {
    lastReading = millis();
    if (!sht.start()) shtReadFailed(false);
  }
}

void onModePinChange() { modePinChanged = true; }

void wakeUp() {
  EIMSK &= ~(_BV(INT0) | _BV(INT1)); // Mask both level interrupts until awake.
}
void powerOn() {
  digitalWrite(GATE_5V, HIGH);
  digitalWrite(GATE_3V3, HIGH);
}
void reportLimits() {
  Serial.print(F("LIMITS,"));
  if (!displayLimits.configured) {
    Serial.println(F("UNSET"));
    return;
  }
  for (byte i = 0; i < 7; ++i) {
    if (i) Serial.print(',');
    const long value = displayLimits.values[i];
    if (i < 2 && value % 10) {
      if (value < 0) Serial.print('-');
      const long magnitude = labs(value);
      Serial.print(magnitude / 10);
      Serial.print('.');
      Serial.print(magnitude % 10);
    } else {
      Serial.print(i < 2 ? value / 10 : value);
    }
  }
  Serial.println();
}

void reportAwake() {
  sdReport();
  reportFanOverride();
  Serial.println(F("BUZZER,READY"));
  reportLimits();
  protocol.mode(operatingMode == OperatingMode::MAINTENANCE);
  protocol.oled(display.status());
  reportFanSpeed();
  reportFan();
  protocol.awake(digitalRead(WAKE_PIN) == HIGH);
}
void sleepArduino() {
  if (digitalRead(WAKE_PIN) == LOW) {
    protocol.wakeHeldLow();
    return; // Do not pretend to sleep or wait forever.
  }
  sdReleaseBus();
  digitalWrite(GATE_5V, LOW);
  digitalWrite(GATE_3V3, LOW);
  protocol.sleeping();
  protocol.flush(); // Complete UART acknowledgement before CPU sleep.

  uint8_t savedAdc = ADCSRA;
  ADCSRA &= ~_BV(ADEN);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  cli();
  uint8_t savedPcicr = PCICR;
  uint8_t savedUcsr = UCSR0B;
  PCICR = 0;
  UCSR0B &= ~(_BV(RXEN0) | _BV(RXCIE0) | _BV(TXCIE0) | _BV(UDRIE0));
  EIFR = _BV(INTF0) | _BV(INTF1);
  attachInterrupt(digitalPinToInterrupt(WAKE_PIN), wakeUp, LOW);
  attachInterrupt(digitalPinToInterrupt(MODE_BUTTON_PIN), wakeUp, LOW);
  if (digitalRead(WAKE_PIN) == HIGH && digitalRead(MODE_BUTTON_PIN) == HIGH) {
    sleep_enable();
    // Keep the AVR's timed BOD-off sequence contiguous. SEI lets SLEEP
    // execute before a pending wake ISR is serviced.
    sleep_bod_disable();
    sei();
    sleep_cpu(); // Actual sleep instruction, also used inside sleep_mode().
    sleep_disable();
  }
  cli();
  detachInterrupt(digitalPinToInterrupt(WAKE_PIN));
  detachInterrupt(digitalPinToInterrupt(MODE_BUTTON_PIN));
  EIFR = _BV(INTF0) | _BV(INTF1);
  PCIFR = _BV(PCIF0) | _BV(PCIF1) | _BV(PCIF2);
  PCICR = savedPcicr;
  UCSR0B = savedUcsr;
  ADCSRA = savedAdc;
  sei();
  protocol.discardInput();
  modeButtonHandled = true; // The wake press cannot select Manual too.
  modeButtonLow = digitalRead(MODE_BUTTON_PIN) == LOW;
  modeButtonChangedAt = millis();
  modePinChanged = true;
  attachInterrupt(digitalPinToInterrupt(MODE_BUTTON_PIN), onModePinChange, CHANGE);
  powerOn();
  delay(20); // Allow the switched SD rail to settle after wake.
  sdBegin();
  reportAwake();
}
void setup() {
  digitalWrite(FAN_RELAY_PIN, RELAY_OFF);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  MCUSR &= ~_BV(WDRF);
  wdt_disable();
  setupFanPwm();
  buzzerBegin();
  tachometer.begin();
  pinMode(WAKE_PIN, INPUT_PULLUP);
  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(MODE_BUTTON_PIN), onModePinChange, CHANGE);
  pinMode(FAN_BUTTON_PIN, INPUT_PULLUP);
  pinMode(FAN_POT_PIN, INPUT);
  DIDR0 |= _BV(ADC0D);       // A0 uses the ADC only.
  digitalWrite(GATE_5V, LOW);
  digitalWrite(GATE_3V3, LOW);
  pinMode(GATE_5V, OUTPUT);
  pinMode(GATE_3V3, OUTPUT);
  Serial.begin(9600); // D0 RX, D1 TX: ESP only, no laptop TX connection.
  powerOn(); // Start awake in AUTO.
  delay(20); // Allow the SD supply to settle before sending SPI commands.
  sdBegin();
  reportAwake();
  restartSensor();
  buzzerPlay(BuzzerSound::Startup);
}
void finishSleep() {
  sleepRequested = false;
  if (digitalRead(WAKE_PIN) == LOW) {
    protocol.wakeHeldLow();
    return;
  }
  autoFan = AutoFan{};
  gasReading = GasReading{};
  climateReady = false;
  wifiDisplayMode = WifiDisplayMode::Unknown;
  operatingMode = OperatingMode::SLEEP;
  fanOverride = FanOverride::Automatic;
  // Sensor lifecycle is OUTSIDE the proven sleepArduino() function.
  setFan(false, false); // The sleep or shutdown command has its own chime.
  display.end(); // Display OFF while the I2C bus is still powered.
  pms.end(); // Stop D7/Timer2 before sensor power-down.
  gas.end(); // Stop the gas heater before releasing the I2C bus.
  sht.end(); // Release I2C and its pull-ups before sensor rail power-off.
  operatingMode = OperatingMode::AUTO; // Hardware wake always returns to AUTO.
  sleepArduino(); // Same sleep/wake sequence; D13 status-LED writes removed.
  restartSensor();
  buzzerPlay(BuzzerSound::Startup);
}

void handleBuzzer(const char* value) {
  if (!strcmp(value, "stop")) buzzerStop();
  else if (!strcmp(value, "test") && operatingMode == OperatingMode::MAINTENANCE) buzzerPlay(BuzzerSound::Test);
  else if (!strcmp(value, "birthday")) buzzerPlay(BuzzerSound::Birthday);
  else if (!strcmp(value, "jingle")) buzzerPlay(BuzzerSound::Jingle);
  else if (!strcmp(value, "twinkle")) buzzerPlay(BuzzerSound::Twinkle);
  else if (!strcmp(value, "ode")) buzzerPlay(BuzzerSound::Ode);
  else return;
  Serial.print(F("BUZZER,"));
  Serial.println(value);
}

byte manualKnobDuty() {
  const uint16_t position = analogRead(FAN_POT_PIN);
  return MIN_FAN_DUTY + uint32_t(position) * (100 - MIN_FAN_DUTY) / 1023;
}

void selectAwakeMode(OperatingMode mode, bool usePot) {
  const bool changed = operatingMode != mode ||
                       (fanOverride == FanOverride::Manual) != usePot;
  operatingMode = mode;
  if (changed) {
    fanOverride = usePot ? FanOverride::Manual : FanOverride::Automatic;
    autoFan = AutoFan{};
    setFan(false, false);
    display.clearFanSpeed();
    if (usePot) {
      fanDuty = manualKnobDuty();
      lastPotRead = millis();
      fanButtonLow = digitalRead(FAN_BUTTON_PIN) == LOW;
      fanButtonHandled = fanButtonLow; // Consume any press already held on entry.
      fanButtonChangedAt = millis();
    } else if (mode == OperatingMode::MAINTENANCE) {
      fanDuty = manualFanDuty;
    }
    buzzerPlay(mode == OperatingMode::MAINTENANCE ? BuzzerSound::Maintenance : BuzzerSound::Auto);
  }
  display.setEnabled(true);
  reportAwake();
}

void readManualDuty() {
  const byte duty = manualKnobDuty();
  if (duty != fanDuty &&
      (duty == 100 || duty == MIN_FAN_DUTY || abs(int(duty) - int(fanDuty)) >= 2)) {
    fanDuty = duty;
    applyFanDuty();
    reportFanSpeed();
    display.showFanSpeed();
  }
}

void updateManualFan() {
  const unsigned long now = millis();
  const bool pressed = digitalRead(FAN_BUTTON_PIN) == LOW;
  if (pressed != fanButtonLow) {
    fanButtonLow = pressed;
    fanButtonChangedAt = now;
  } else if (now - fanButtonChangedAt >= 30UL) {
    if (!pressed) {
      fanButtonHandled = false;
    } else if (!fanButtonHandled) {
      fanButtonHandled = true;
      if (!fanOn) {
        readManualDuty();
        lastPotRead = now;
      }
      setFan(!fanOn, true);
    }
  }
  if (now - lastPotRead >= 100UL) {
    lastPotRead = now;
    readManualDuty();
  }
}

void updateModeButton() {
  const unsigned long now = millis();
  if (modePinChanged) {
    bool pressed;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      modePinChanged = false;
      pressed = digitalRead(MODE_BUTTON_PIN) == LOW;
    }
    if (pressed != modeButtonLow) {
      modeButtonLow = pressed;
      modeButtonChangedAt = now;
      return;
    }
  }
  if (now - modeButtonChangedAt < 30UL) return;
  if (!modeButtonLow) {
    modeButtonHandled = false;
    return;
  }
  if (modeButtonHandled || sleepRequested) return;
  modeButtonHandled = true;
  switch (operatingMode) {
    case OperatingMode::MAINTENANCE:
      if (digitalRead(WAKE_PIN) == LOW) {
        protocol.wakeHeldLow();
      } else {
        sleepRequested = true;
        buzzerPlay(BuzzerSound::Sleep);
      }
      break;
    case OperatingMode::AUTO:
      if (fanOverride == FanOverride::Manual) selectAwakeMode(OperatingMode::MAINTENANCE, false);
      else selectAwakeMode(OperatingMode::AUTO, true);
      break;
    case OperatingMode::SLEEP:
      break;
  }
}

void loop() {
  buzzerUpdate();
  updateModeButton();
  // Release D3 before sleep so the same press cannot wake the Arduino.
  if (sleepRequested && !buzzerPlaying() && !modeButtonLow && !modeButtonHandled) finishSleep();
  const auto received = protocol.nextCommand();
  const auto command = sleepRequested ? FumetrixProtocol::Command::None : received;
  switch (command) {
    case FumetrixProtocol::Command::Buzzer:
      handleBuzzer(protocol.line() + 7);
      break;
    case FumetrixProtocol::Command::Auto:
    case FumetrixProtocol::Command::Maintenance:
      selectAwakeMode(command == FumetrixProtocol::Command::Auto ?
                      OperatingMode::AUTO : OperatingMode::MAINTENANCE, false);
      break;
    case FumetrixProtocol::Command::OledOn:
    case FumetrixProtocol::Command::OledOff:
      if (operatingMode == OperatingMode::MAINTENANCE) {
        display.setEnabled(command == FumetrixProtocol::Command::OledOn);
        protocol.oled(display.status());
      }
      break;
    case FumetrixProtocol::Command::Shutdown:
    case FumetrixProtocol::Command::Sleep:
      if (digitalRead(WAKE_PIN) == LOW) { protocol.wakeHeldLow(); break; }
      sleepRequested = true;
      buzzerPlay(command == FumetrixProtocol::Command::Shutdown ? BuzzerSound::Shutdown : BuzzerSound::Sleep);
      break;
    case FumetrixProtocol::Command::FanOn:
      if (operatingMode == OperatingMode::MAINTENANCE) setFan(true, true);
      break;
    case FumetrixProtocol::Command::FanOff:
      if (operatingMode == OperatingMode::MAINTENANCE) setFan(false, true);
      break;
    case FumetrixProtocol::Command::AutoFanOverride:
      if (operatingMode == OperatingMode::AUTO && fanOverride != FanOverride::Manual)
        setAutoFanOverride(protocol.line() + 8);
      break;
    case FumetrixProtocol::Command::Speed:
      if (operatingMode == OperatingMode::MAINTENANCE) setWebFanSpeed(protocol.speedSetting());
      break;
    case FumetrixProtocol::Command::Status:
      reportAwake();
      break;
    case FumetrixProtocol::Command::Limits:
      if (displayLimits.parse(protocol.line() + 7)) reportLimits();
      break;
    case FumetrixProtocol::Command::OledGas:
      gasReading.receive(protocol.line());
      break;
    case FumetrixProtocol::Command::SdReset:
      if (!sleepRequested) sdReset(protocol.line() + 8);
      break;
    case FumetrixProtocol::Command::SdRead:
      sdSend(protocol.line() + 6);
      break;
    case FumetrixProtocol::Command::OledWifi:
      receiveWifiDisplayMode(protocol.line() + 9);
      break;
    case FumetrixProtocol::Command::None:
      break;
  }
  pms.tick();
  tachometer.tick(protocol);
  updateSensor();
  const long pm25 = pms.pm25();
  const unsigned long now = millis();
  const bool climateFresh = climateReady && now - climateAt < 5000UL;
  const bool gasFresh = gasReading.fresh(now);
  if (!sleepRequested && (climateFresh || gasFresh || pm25 >= 0) &&
      (!haveSdLog || now - lastSdLog >= 180000UL)) {
    lastSdLog = now;
    haveSdLog = true;
    sdAppend(SdReading{uint32_t(now / 1000UL),
      climateFresh ? int16_t(logTemp / 10) : int16_t(-32768),
      climateFresh ? int16_t(logHumidity / 10) : int16_t(-32768),
      int32_t(pms.pm1()), int32_t(pm25), int32_t(pms.pm10()), gasFresh ? gasReading.voc : uint16_t(0),
      gasFresh ? gasReading.nox : uint16_t(0),
      // SD mode codes: 0 = Auto, 1 = Maintenance, 2 = Manual.
      byte(operatingMode == OperatingMode::MAINTENANCE ? 1 :
           fanOverride == FanOverride::Manual ? 2 : 0),
      byte(fanOn ? fanDuty : 0)});
  }
  if (operatingMode == OperatingMode::AUTO && !sleepRequested) {
    if (fanOverride == FanOverride::Manual) updateManualFan();
    else if (fanOverride != FanOverride::Off) runAutoFan(pm25);
  }
  if (sensorInitialized) {
    gas.tick();
    display.tick(fanOn, fanDuty, tachometer.latest(), pm25, pms.status(), pms.pm1(), pms.pm10(), gasReading,
                 wifiDisplayLabel(), operatingMode == OperatingMode::MAINTENANCE,
                 fanOverride == FanOverride::Manual);
  }
}
