#pragma once
#include <Arduino.h>
#include "src/sensirion/sensirion_gas_index_algorithm.h"

// UART framing, sensor status, and limit validation for the ESP bridge.

struct FumetrixLimits {
  bool configured = false;
  long values[7] = {};

  bool parse(const char* input);
};

class LineReceiver {
 public:
  bool poll(Stream &port);
  const char * line() const;
  void reset();
 private:
  char data[100] = {};
  byte used = 0;
  bool dropping = false;
  unsigned long lastByte = 0;
};

class GasIndices {
 public:
  void reset(const char* reason);
  void receive(const char* line);
  void appendStatus(String &out, bool awake, bool sleeping) const;
  void sendToOled(Stream& port) const;
 private:
  const char* statusLabel(bool awake, bool sleeping, bool fresh) const;
  GasIndexAlgorithmParams vocAlgorithm, noxAlgorithm;
  int32_t voc = 0, nox = 0;
  uint16_t sequence = 0;
  unsigned long sampleAt = 0;
  bool haveSample = false, compensated = false;
  const char* status = "WAITING";
};

class ParticleReadings {
 public:
  void reset();
  void receive(const char* line);
  void appendStatus(String& out,bool awake,bool asleep) const;
 private:
  bool have=false;
  const char* label="WAITING";
  uint16_t pm1=0,pm25=0,pm10=0;
  unsigned long sampleAt=0;
};
