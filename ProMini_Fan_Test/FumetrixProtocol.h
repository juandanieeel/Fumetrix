#pragma once
#include <Arduino.h>

// Fixed buffer; damaged/oversized lines are discarded until the next newline.
class FumetrixProtocol {
 public:
  enum class Command : byte { None, Sleep, FanOn, FanOff, Speed, Status, OledGas, Auto, Maintenance, OledOn, OledOff, Limits, Shutdown, Buzzer, SdRead, SdReset, OledWifi, AutoFanOverride };

  explicit FumetrixProtocol(Stream& uart) : uart(uart) {}
  Command nextCommand();
  const char* speedSetting() const;
  void mode(bool maintenance);
  void oled(const char* status);
  void rpm(unsigned long value);
  void speed(byte duty);
  void fan(bool on);
  void awake(bool wakeHigh);
  void sleeping();
  void wakeHeldLow();
  void flush();
  void discardInput();
  void climate(int16_t temp100, uint16_t rh100);
  void climateError(bool readError);
  void gasStatus(const __FlashStringHelper* status);
  void gasSample(uint16_t sequence, uint16_t voc, uint16_t nox, bool compensated);
  void particleSample(uint16_t pm1, uint16_t pm25, uint16_t pm10);
  void particleStatus(const char* status);

 private:
  bool poll();
 public:
  const char *line() const;
 private:
  void reset();
  Stream& uart;
  char data[64] = {};
  byte used = 0;
  bool dropping = false;
  unsigned long lastByte = 0;
};
