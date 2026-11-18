#pragma once
#include <Arduino.h>
#include "FumetrixProtocol.h"
#include <string.h>

// Plantower active-mode frame: atmospheric mass concentrations, ug/m3.
class PmsFrame {
 public:
  uint16_t pm1=0, pm25=0, pm10=0;
  void reset() { used=0; }
  // 1 = valid frame, -1 = damaged frame, 0 = incomplete.
  int8_t push(uint8_t b) {
    data[used++]=b;
    while (used && (data[0]!=0x42 || (used>=2 && data[1]!=0x4D) ||
           (used>=4 && (data[2]!=0 || data[3]!=28)))) discard();
    if (used<32) return 0;
    uint16_t sum=0;
    for(uint8_t i=0;i<30;++i) sum+=data[i];
    if(sum!=word(30)) { discard(); return -1; }
    pm1=word(10); pm25=word(12); pm10=word(14);
    used=0; return 1;
  }
 private:
  uint8_t data[32]={}, used=0;
  uint16_t word(uint8_t n) const { return (uint16_t(data[n])<<8)|data[n+1]; }
  void discard() { --used; memmove(data,data+1,used); }
};

struct GasReading {
  enum GasState : byte { WAITING, READY, WARMING, CONDITIONING, ERROR, STALE };
  uint16_t voc = 0, nox = 0;
  GasState gasState = WAITING;
  unsigned long gasAt = 0;
  bool haveGas = false;

  bool fresh(unsigned long now) const {
    return haveGas && now - gasAt < 3500UL && gasState == READY;
  }

  // ESP sends OLEDGAS,<status>,<VOC>,<NOx> once per second while awake.
  // Zero is a placeholder for unavailable data, never displayed as an index.
  void receive(const char* line);

};


struct ClimateSample {
  int16_t temp100;
  uint16_t rh100;
  uint16_t gasTemperature;
  uint16_t gasHumidity;
};

class Tachometer {
 public:
  void begin();
  void setEnabled(bool enabled);
  void tick(FumetrixProtocol& protocol);
  long latest() const;
};

// SHT40 high precision measurement, heater OFF. No external sensor library.
class Sht40Sensor {
 public:
  void begin();
  void end();
  bool start();
  bool ready() const;
  bool busy() const;
  bool read(ClimateSample& sample);
 private:
  static const uint8_t ADDRESS = 0x44; // Some variants use 0x45 or 0x46.
  bool pending = false;
  unsigned long started = 0;

};

// Raw SGP41 sampling only. The ESP runs Sensirion's Gas Index Algorithm.
// No delays during normal sampling, so UART and fan PWM continue to run.
class Sgp41Sensor {
 public:
  explicit Sgp41Sensor(FumetrixProtocol& protocol) : protocol(protocol) {}
  void begin();
  void compensation(const ClimateSample& sample);
  void tick();
  void end();
 private:
  FumetrixProtocol& protocol;
  bool pending = false, failed = true, conditioning = false;
  bool haveCompensation = false, usedCompensation = false;
  unsigned long started = 0, conditioningAt = 0, compensatedAt = 0;
  uint16_t sequence = 0;
  uint16_t temperatureTicks = 0, humidityTicks = 0;
  void word(uint16_t v);
  bool command(uint16_t cmd, uint16_t rh, uint16_t t);
  bool readWords(byte count, uint16_t &voc, uint16_t &nox);
  void heaterOff();
  void fail();
};

class Pms7003Sensor {
 public:
  explicit Pms7003Sensor(FumetrixProtocol& protocol) : protocol(protocol) {}
  void begin();
  void end();
  const char* status() const;
  long pm25() const;
  long pm1() const;
  long pm10() const;
  void tick();
 private:
  FumetrixProtocol& protocol;
  PmsFrame parser;
  bool running=false,haveSample=false,damaged=false;
  unsigned long started=0,lastByte=0,lastSample=0,lastReport=0;
};
