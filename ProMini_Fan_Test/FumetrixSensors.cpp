#include "FumetrixSensors.h"
#include <Wire.h>

#include <avr/interrupt.h>
#include <util/atomic.h>
// Receive-only D7, 9600 8N1. Timer2 samples bits; never blocks for a byte.
// Timer0 millis and Timer1 fan PWM are retained. Do not use tone(),
// analogWrite(D3/D11), SoftwareSerial or another Timer2 library with this test.
namespace PmsRx {
static_assert(F_CPU==16000000UL,"PMS receiver requires the 16 MHz Pro Mini");
static volatile uint8_t ring[64], head=0, tail=0, phase=0, value=0;
static volatile bool enabled=false, busy=false, fault=false;
inline void finish() {
  TCCR2B=0; TIMSK2=0; busy=false;
  if(enabled) PCMSK2|=_BV(PCINT23);
}
void pinChange(uint8_t pins) {
  if(!enabled || busy || (pins & _BV(PD7))) return;
  busy=true; phase=0; value=0;
  PCMSK2&=~_BV(PCINT23);
  TCNT2=0; OCR2A=25; TIFR2=_BV(OCF2A);
  TIMSK2=_BV(OCIE2A); TCCR2B=_BV(CS21)|_BV(CS20); // /32: 2 us/tick
}
inline void sample() {
  const bool high=(PIND&_BV(PD7))!=0;
  if(phase==0) { // half a bit: reject a false start
    if(high) { finish(); return; }
    OCR2A=51; phase=1; return; // subsequent samples every 104 us
  }
  if(phase<=8) {
    if(high) value|=uint8_t(1U<<(phase-1));
    ++phase; return;
  }
  if(high) {
    uint8_t next=(head+1)&63;
    if(next!=tail) {ring[head]=value;head=next;} else fault=true;
  } else fault=true; // stop-bit/framing error
  finish();
}
inline void begin() {
  pinMode(7,INPUT); digitalWrite(7,LOW); // no 5 V pull-up into unpowered sensor
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    TCCR2B=0; TIMSK2=0; TCCR2A=_BV(WGM21); TCNT2=0;
    head=tail=0; fault=false; busy=false; enabled=true;
    PCMSK2|=_BV(PCINT23); PCICR|=_BV(PCIE2);
  }
}
inline void end() {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    enabled=false; TCCR2B=0; TIMSK2=0; PCMSK2&=~_BV(PCINT23);
    busy=false; head=tail=0; fault=false;
  }
}
inline bool takeFault() {
  bool bad;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    bad=fault; fault=false;
    if(bad) tail=head; // never splice bytes across a lost byte
  }
  return bad;
}
inline int read() {
  if(tail==head) return -1;
  const uint8_t b=ring[tail]; tail=(tail+1)&63; return b;
}
}
ISR(TIMER2_COMPA_vect) { PmsRx::sample(); }

// Uses the two pulses per revolution assumed by the bench sketch.
namespace {
constexpr byte FAN_TACH_PIN = 5; // External pull-up to 5 V.
constexpr byte TACH_PULSES_PER_REV = 2;
volatile uint16_t tachPulses = 0;
volatile bool tachWasHigh = false;
unsigned long lastRpmTime = 0;
long oledRpm = -1;
unsigned long oledRpmAt = 0;
bool tachEnabled = false;
}

ISR(PCINT2_vect) {
  const bool high = (PIND & _BV(PD5)) != 0;
  if (tachWasHigh && !high) ++tachPulses;
  tachWasHigh = high;
  PmsRx::pinChange(PIND); // D7 UART start; share this ISR with D5 tach.
}

void Tachometer::begin() {
  pinMode(FAN_TACH_PIN, INPUT);
  setEnabled(false);
}

long Tachometer::latest() const {
  return millis() - oledRpmAt < 3500UL ? oledRpm : -1;
}

void Tachometer::setEnabled(bool enabled) {
  tachEnabled = enabled;
  oledRpm = -1;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    PCMSK2 &= ~_BV(PCINT21);
    tachPulses = 0;
    tachWasHigh = (PIND & _BV(PD5)) != 0;
    // Do not clear PCIF2: a D7 start bit may be pending.
    if (enabled) {
      PCMSK2 |= _BV(PCINT21);
      PCICR |= _BV(PCIE2);
    }
  }
  lastRpmTime = millis();
}

void Tachometer::tick(FumetrixProtocol& protocol) {
  const unsigned long now = millis();
  const unsigned long elapsed = now - lastRpmTime;
  if (!tachEnabled || elapsed < 1000UL) return;
  uint16_t pulses;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    pulses = tachPulses;
    tachPulses = 0;
  }
  lastRpmTime = now;
  const unsigned long rpm = pulses * 60000UL / (elapsed * TACH_PULSES_PER_REV);
  oledRpm = rpm;
  oledRpmAt = now;
  protocol.rpm(rpm);
}




static uint8_t sensirionCrc(const uint8_t *p) {
    uint8_t result = 0xFF;
    for (byte i = 0; i < 2; ++i) {
      result ^= p[i];
      for (byte j = 0; j < 8; ++j)
        result = (result & 0x80) ? (result << 1) ^ 0x31 : result << 1;
    }
    return result;
  }

void GasReading::receive(const char* line) {
  if (strncmp(line, "OLEDGAS,", 8)) return;
  const char* start = line + 8;
  const char* comma = strchr(start, ',');
  if (!comma || comma - start > 12) return;
  char label[13]; size_t n = comma - start;
  memcpy(label, start, n); label[n] = '\0';
  GasState parsed;
  if (!strcmp(label,"READY")) parsed = READY;
  else if (!strcmp(label,"WARMING")) parsed = WARMING;
  else if (!strcmp(label,"CONDITIONING")) parsed = CONDITIONING;
  else if (!strcmp(label,"ERROR")) parsed = ERROR;
  else if (!strcmp(label,"STALE")) parsed = STALE;
  else if (!strcmp(label,"WAITING") || !strcmp(label,"OFF")) parsed = WAITING;
  else return;
  const char* p = comma + 1;
  if (*p < '0' || *p > '9') return;
  char* end; long v = strtol(p, &end, 10);
  if (*end != ',' || v < 0 || v > 500) return;
  p = end + 1;
  if (*p < '0' || *p > '9') return;
  long x = strtol(p, &end, 10);
  if (*end != '\0' || x < 0 || x > 500) return;
  if (parsed == READY && (!v || !x)) return;
  voc = v; nox = x; gasState = parsed;
  gasAt = millis(); haveGas = true;
}

void Sht40Sensor::begin() {
    Wire.begin();
    Wire.setClock(100000UL);
    Wire.setWireTimeout(25000UL, true);
    pending = false;
  }

void Sht40Sensor::end() {
    pending = false;
    Wire.end();
    pinMode(A4, INPUT);
    pinMode(A5, INPUT);
    digitalWrite(A4, LOW); // Disable internal pull-ups before cutting power.
    digitalWrite(A5, LOW);
  }

bool Sht40Sensor::start() {
    Wire.clearWireTimeoutFlag();
    Wire.beginTransmission(ADDRESS);
    Wire.write(0xFD);
    if (Wire.endTransmission() != 0 || Wire.getWireTimeoutFlag()) return false;
    started = millis();
    pending = true;
    return true;
  }

bool Sht40Sensor::ready() const { return pending && millis() - started >= 10UL; }

bool Sht40Sensor::busy() const { return pending; }

bool Sht40Sensor::read(ClimateSample& sample) {
    pending = false;
    Wire.clearWireTimeoutFlag(); // Check only this SHT40 transaction.
    byte count = Wire.requestFrom(ADDRESS, (uint8_t)6);
    if (count != 6 || Wire.getWireTimeoutFlag()) {
      while (Wire.available()) Wire.read();
      return false;
    }
    uint8_t b[6];
    for (byte i = 0; i < 6; ++i) b[i] = Wire.read();
    if (sensirionCrc(b) != b[2] || sensirionCrc(b + 3) != b[5]) return false;
    uint16_t rawT = ((uint16_t)b[0] << 8) | b[1];
    uint16_t rawH = ((uint16_t)b[3] << 8) | b[4];
    // Use integer hundredths for the wire/display values. The SGP41
    // temperature encoding is identical to the SHT40 raw temperature.
    sample.temp100 = int32_t((17500UL * rawT + 32767) / 65535UL) - 4500;
    const int32_t rh = 125L * rawH - 6L * 65535;
    const uint32_t clampedRh = constrain(rh, 0L, 100L * 65535);
    sample.rh100 = (clampedRh * 100UL + 32767) / 65535UL;
    sample.gasTemperature = rawT;
    sample.gasHumidity = (clampedRh + 50) / 100;
    return true;
  }

void Sgp41Sensor::begin() {
    pending = false; failed = false; conditioning = true; haveCompensation = false;
    sequence = 0;
    protocol.gasStatus(F("GAS,RESET"));
    // One conditioning command starts continuous heater operation.
    // Transition at 9 s leaves scheduling margin below the 10 s maximum.
    if (!command(0x2612, 0x8000, 0x6666)) { fail(); return; }
    conditioningAt = started = millis();
    pending = true;
    protocol.gasStatus(F("GAS,CONDITIONING"));
  }

void Sgp41Sensor::compensation(const ClimateSample& sample) {
    temperatureTicks = sample.gasTemperature;
    humidityTicks = sample.gasHumidity;
    compensatedAt = millis();
    haveCompensation = true;
  }

void Sgp41Sensor::tick() {
    if (failed) return;
    unsigned long now = millis();
    if (pending) {
      if (now - started < 50UL) return;
      pending = false;
      uint16_t voc, nox = 0;
      if (!readWords(conditioning ? 1 : 2, voc, nox)) { fail(); return; }
      if (!conditioning) {
        protocol.gasSample(sequence++, voc, nox, usedCompensation);
      }
    }
    now = millis();
    if (conditioning) {
      if (now - conditioningAt < 9000UL) return;
      conditioning = false;
    } else if (now - started < 1000UL) return;
    usedCompensation = haveCompensation && now - compensatedAt < 6000UL;
    const uint16_t rh = usedCompensation ? humidityTicks : 0x8000;
    const uint16_t t = usedCompensation ? temperatureTicks : 0x6666;
    if (!command(0x2619, rh, t)) { fail(); return; }
    started = millis(); pending = true;
  }

void Sgp41Sensor::end() {
    // A measurement cannot accept a command until its 50 ms window ends.
    const unsigned long elapsed = millis() - started;
    if (pending && elapsed < 50UL) delay(50UL - elapsed);
    heaterOff();
    pending = false; failed = true; haveCompensation = false;
  }

void Sgp41Sensor::word(uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    Wire.write(b, 2); Wire.write(sensirionCrc(b));
  }

bool Sgp41Sensor::command(uint16_t cmd, uint16_t rh, uint16_t t) {
    Wire.clearWireTimeoutFlag();
    Wire.beginTransmission(0x59);
    Wire.write((uint8_t)(cmd >> 8)); Wire.write((uint8_t)cmd);
    word(rh); word(t);
    return Wire.endTransmission() == 0 && !Wire.getWireTimeoutFlag();
  }

bool Sgp41Sensor::readWords(byte count, uint16_t &voc, uint16_t &nox) {
    Wire.clearWireTimeoutFlag();
    if (Wire.requestFrom((uint8_t)0x59, (uint8_t)(count * 3)) != count * 3 || Wire.getWireTimeoutFlag()) {
      while (Wire.available()) Wire.read();
      return false;
    }
    uint16_t values[2] = {};
    for (byte i=0;i<count;++i) {
      uint8_t b[2] = {(uint8_t)Wire.read(), (uint8_t)Wire.read()};
      const uint8_t receivedCrc = Wire.read();
      if (sensirionCrc(b) != receivedCrc) {
        while (Wire.available()) Wire.read();
        return false;
      }
      values[i] = ((uint16_t)b[0] << 8) | b[1];
    }
    voc=values[0]; nox=values[1]; return true;
  }

void Sgp41Sensor::heaterOff() {
    Wire.beginTransmission(0x59); Wire.write(0x36); Wire.write(0x15);
    Wire.endTransmission(); delay(1);
  }

void Sgp41Sensor::fail() {
    heaterOff(); pending = false; failed = true;
    protocol.gasStatus(F("GAS,ERROR"));
    // Fix wiring, then sleep/wake to retry. Never keep conditioning on errors.
  }

void Pms7003Sensor::begin() {
    parser.reset(); started=millis(); lastByte=started; lastSample=0;
    haveSample=false; damaged=false; running=true; lastReport=started-1000UL;
    PmsRx::begin();
  }

void Pms7003Sensor::end() { PmsRx::end(); running=false; haveSample=false; parser.reset(); }

const char* Pms7003Sensor::status() const {
    if(!running) return "OFF";
    if(millis()-started<30000UL) return "WARMING";
    if(damaged) return "ERROR";
    if(!haveSample) return "MISSING";
    return millis()-lastSample<5000UL ? "READY" : "STALE";
  }

long Pms7003Sensor::pm25() const { return !strcmp(status(), "READY") ? static_cast<long>(parser.pm25) : -1L; }

void Pms7003Sensor::tick() {
    if(!running) return;
    if(PmsRx::takeFault()) { parser.reset(); damaged=true; haveSample=false; }
    if(millis()-lastByte>100UL) parser.reset();
    int b; bool newSample=false;
    while((b=PmsRx::read())>=0) {
      lastByte=millis();
      const int8_t result=parser.push(uint8_t(b));
      if(result<0) {damaged=true;haveSample=false;}
      else if(result>0) {
        damaged=false;
        // Ignore warmup samples; require a new frame after 30 seconds.
        if(millis()-started>=30000UL) {
          haveSample=true;lastSample=millis();newSample=true;
        }
      }
    }
    if(newSample && !strcmp(status(),"READY")) {
      protocol.particleSample(parser.pm1, parser.pm25, parser.pm10);
    }
    if(millis()-lastReport>=1000UL) {
      lastReport=millis();protocol.particleStatus(status());
    }
  }

long Pms7003Sensor::pm1() const {
  return strcmp(status(), "READY") == 0 ? static_cast<long>(parser.pm1) : -1L;
}
long Pms7003Sensor::pm10() const {
  return strcmp(status(), "READY") == 0 ? static_cast<long>(parser.pm10) : -1L;
}
