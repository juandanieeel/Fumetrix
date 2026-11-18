#pragma once
#include <Arduino.h>

struct SdReading {
  uint32_t uptime;
  int16_t temperature;
  int16_t humidity;
  int32_t pm1, pm25, pm10;
  uint16_t voc;
  uint16_t nox;
  byte mode;
  byte duty;
};

void sdBegin();
void sdReleaseBus();
void sdAppend(const SdReading& reading);
void sdReport();
void sdSend(const char* index);
void sdReset(const char* parameters);
