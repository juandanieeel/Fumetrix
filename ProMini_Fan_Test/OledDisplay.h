#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <U8x8lib.h>
#include <stdlib.h>
#include <string.h>
#include <avr/pgmspace.h>
#include "FumetrixLimits.h"
#include "FumetrixSensors.h"
extern FumetrixLimits displayLimits;

// 16 columns x 8 rows; text mode saves SRAM on the Pro Mini.
class OledDisplay {
 public:
  void begin() {
    active = false; hidden = false; haveReading = false; shtError = false;
    speedVisible = false;
    nextRow = 0;
    page = 0; pageAt = millis();
    memset(previous, 0, sizeof(previous));
    byte address = responds(0x3C) ? 0x3C : (responds(0x3D) ? 0x3D : 0);
    if (!address) return;
    oledAddress = address;
    const static byte init[] PROGMEM = {
      0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,0x8D,0x14,
      0x20,0x00,0xA1,0xC8,0xDA,0x12,0x81,0xCF,0xD9,0xF1,0xDB,0x40,0xA4,0xA6
    };
    active = commands(init, sizeof(init));
    if (!active) return;
    for (byte y = 0; y < 8; ++y) row(y, "");
    power(true);
    lastPaint = millis() - 40UL;
  }
  const char* status() const { return !active ? "MISSING" : (hidden ? "OFF" : "ON"); }
  void setEnabled(bool enabled) {
    if (!active) return;
    power(enabled);
    hidden = !enabled;
    if (hidden) clearFanSpeed();
  }
  void reading(int16_t t, uint16_t h) {
    temperature = t; humidity = h; sampleAt = millis();
    haveReading = true; shtError = false;
  }
  void error(const char*) { haveReading = false; shtError = true; }

  void showFanSpeed() {
    if (!active || hidden) return;
    const unsigned long now = millis();
    if (!speedVisible) {
      speedVisible = true;
      nextRow = 0;
      lastPaint = now - 40UL;
    }
    speedAt = now;
  }
  void clearFanSpeed() {
    if (!speedVisible) return;
    speedVisible = false;
    nextRow = 0;
    pageAt = millis();
  }

  // One row per tick keeps UART and fan-control processing responsive.
  // Pages change without delay() and do not restart the sensors.
  void tick(bool fanOn, byte duty, long rpm, long pm25, const char* pmStatus,
            long pm1, long pm10, const GasReading& gas,
            PGM_P wifiLabel, bool maintenance, bool manual) {
    if (!active || hidden) return;
    const unsigned long now = millis();
    if (speedVisible && now - speedAt >= 3000UL) clearFanSpeed();
    if (!speedVisible && now - pageAt >= 10000UL) {
      page = (page + 1) % 4;
      pageAt = now;
      nextRow = 0;
    }
    if (now - lastPaint < 40UL) return;
    lastPaint = now;
    char text[24] = {};
    if (speedVisible) {
      speedRow(text, fanOn, duty);
      row(nextRow, text);
      nextRow = (nextRow + 1) % 8;
      return;
    }
    const bool climateFresh = haveReading && now - sampleAt < 6000UL;
    const bool gasFresh = gas.fresh(now);
    if (nextRow == 0) {
      switch (page) {
        case 0: strcpy_P(text, PSTR("SHT40       1/4")); break;
        case 1: strcpy_P(text, PSTR("SGP41       2/4")); break;
        case 2: strcpy_P(text, PSTR("PMS7003     3/4")); break;
        case 3: strcpy_P(text, PSTR("FAN / TACH  4/4")); break;
      }
    } else if (nextRow == 1) {
      strcpy_P(text, wifiLabel);
      strcat_P(text, maintenance ? PSTR(" MAINT") : manual ? PSTR(" MANUAL") : PSTR(" AUTO"));
    } else if (page == 0) {
      if ((nextRow == 2 || nextRow == 4) && climateFresh) {
        const int16_t value = nextRow == 2 ? temperature : int16_t(humidity);
        const int16_t tenths = (value + (value < 0 ? -5 : 5)) / 10;
        strcpy_P(text, nextRow == 2 ? PSTR("Temp: ") : PSTR("RH: "));
        if (tenths < 0) strcat_P(text, PSTR("-"));
        appendNumber(text, abs(tenths) / 10);
        strcat_P(text, PSTR("."));
        appendNumber(text, abs(tenths) % 10);
        strcat_P(text, nextRow == 2 ? PSTR(" C") : PSTR(" %"));
      } else if (nextRow == (maintenance ? 7 : 2) && (!climateFresh || maintenance)) {
        strcpy_P(text, climateFresh ? PSTR("CONNECTED") : shtError ? PSTR("READ ERROR") :
                       haveReading ? PSTR("STALE") : PSTR("WAITING"));
      } else if (nextRow == 3 && climateFresh) {
        strcpy_P(text, assess(temperature, displayLimits.configured, displayLimits.values[0] * 10, displayLimits.values[1] * 10));
      } else if (nextRow == 5 && climateFresh) {
        strcpy_P(text, assess(humidity, displayLimits.configured, displayLimits.values[2] * 100, displayLimits.values[3] * 100));
      }
    } else if (page == 1) {
      if ((nextRow == 2 || nextRow == 4) && gasFresh) {
        number(text, nextRow == 2 ? PSTR("VOC: ") : PSTR("NOx: "),
               nextRow == 2 ? gas.voc : gas.nox);
        strcat_P(text, PSTR(" idx"));
      } else if (nextRow == (maintenance ? 7 : 2) && (!gasFresh || maintenance)) {
        strcpy_P(text, gasFresh ? PSTR("CONNECTED") : gasStatus(gas, now));
      } else if (nextRow == 3 && gasFresh) strcpy_P(text, assess(gas.voc, displayLimits.configured, 1, displayLimits.values[5]));
      else if (nextRow == 5 && gasFresh) strcpy_P(text, assess(gas.nox, displayLimits.configured, 1, displayLimits.values[6]));
    } else if (page == 2) {
      const bool ready = !strcmp(pmStatus, "READY") && pm25 >= 0;
      if (nextRow == 2 && ready && pm1 >= 0) number(text, PSTR("PM1.0: "), pm1);
      else if (nextRow == (maintenance ? 7 : 2) && (!ready || maintenance)) {
        if (ready) strcpy_P(text, PSTR("CONNECTED"));
        else strcpy(text, pmStatus);
      } else if (nextRow == 3 && ready) number(text, PSTR("PM2.5: "), pm25);
      else if (nextRow == 4 && ready && pm10 >= 0) number(text, PSTR("PM10:  "), pm10);
      else if (nextRow == 5 && ready) strcpy_P(text, PSTR("ug/m3"));
      else if (nextRow == 6 && ready) strcpy_P(text, assess(pm25, displayLimits.configured, 0, displayLimits.values[4]));
    } else {
      if (nextRow == 2) strcpy_P(text, fanOn ? PSTR("Fan: ON") : PSTR("Fan: OFF"));
      else if (nextRow == 3) { number(text, PSTR("PWM: "), duty); strcat_P(text, PSTR("%")); }
      else if (nextRow == 4) {
        if (fanOn && rpm >= 0) number(text, PSTR("RPM: "), rpm);
        else strcpy_P(text, PSTR("RPM: --"));
      } else if (nextRow == 7 && maintenance) {
        strcpy_P(text, !fanOn ? PSTR("TURN FAN ON") :
                       rpm > 0 ? PSTR("CONNECTED") : PSTR("NO RPM SIGNAL"));
      }
    }
    row(nextRow, text);
    nextRow = (nextRow + 1) % 8;
  }
  void end() {
    if (active) power(false);
    active = false; haveReading = false;
  }
 private:
  byte oledAddress = 0;
  bool hidden = false;
  bool active = false, haveReading = false, shtError = false;
  int16_t temperature = 0;
  uint16_t humidity = 0;
  unsigned long sampleAt = 0, lastPaint = 0;
  byte nextRow = 0, page = 0;
  unsigned long pageAt = 0;
  bool speedVisible = false;
  unsigned long speedAt = 0;
  char previous[8][17] = {};
  void speedRow(char* text, bool fanOn, byte duty) {
    if (nextRow == 0) strcpy_P(text, PSTR("   FAN SPEED"));
    else if (nextRow == 3) {
      number(text, PSTR("    "), duty);
      strcat_P(text, PSTR("% PWM"));
    } else if (nextRow == 5) {
      const byte filled = (uint16_t(duty) * 14 + 50) / 100;
      text[0] = '[';
      for (byte i = 0; i < 14; ++i) text[i + 1] = i < filled ? '#' : '-';
      text[15] = ']';
    } else if (nextRow == 7) {
      strcpy_P(text, fanOn ? PSTR("     FAN ON") : PSTR("     FAN OFF"));
    }
  }
  static const char* assess(long v, bool configured, long minimum, long maximum) {
    if (!configured || minimum > maximum) return PSTR("SET LIMITS");
    return v >= minimum && v <= maximum ? PSTR("NORMAL") : PSTR("NOT NORMAL");
  }
  static void appendNumber(char* text, long value) {
    ltoa(value, text + strlen(text), 10);
  }
  static void number(char* text, PGM_P label, long value) {
    strcpy_P(text, label);
    appendNumber(text, value);
  }
  static PGM_P gasStatus(const GasReading& gas, unsigned long now) {
    if (!gas.haveGas) return PSTR("WAITING");
    if (now - gas.gasAt >= 3500UL || gas.gasState == GasReading::STALE) return PSTR("STALE");
    if (gas.gasState == GasReading::ERROR) return PSTR("READ ERROR");
    if (gas.gasState == GasReading::WARMING) return PSTR("WARMUP");
    if (gas.gasState == GasReading::CONDITIONING) return PSTR("CONDITIONING");
    return PSTR("WAITING");
  }
  bool commands(const byte* bytes, byte length) {
    Wire.beginTransmission(oledAddress);
    Wire.write(byte(0));
    while (length--) Wire.write(pgm_read_byte(bytes++));
    return Wire.endTransmission() == 0;
  }
  void power(bool on) {
    Wire.beginTransmission(oledAddress);
    Wire.write(byte(0));
    Wire.write(on ? 0xAF : 0xAE);
    Wire.endTransmission();
  }
  bool responds(byte address) {
    Wire.clearWireTimeoutFlag(); Wire.beginTransmission(address);
    return Wire.endTransmission() == 0 && !Wire.getWireTimeoutFlag();
  }
  void row(byte y, const char* text) {
    char padded[17]; memset(padded, ' ', 16);
    size_t length = strlen(text); if (length > 16) length = 16;
    memcpy(padded, text, length); padded[16] = '\0';
    // Paint at most one changed row per tick to keep UART processing responsive.
    if (!strcmp(previous[y], padded)) return;
    Wire.clearWireTimeoutFlag();
    Wire.beginTransmission(oledAddress);
    Wire.write(byte(0));
    Wire.write(0x21); Wire.write(byte(0)); Wire.write(127);
    Wire.write(0x22); Wire.write(y); Wire.write(y);
    if (Wire.endTransmission()) return;
    for (byte start = 0; start < 16; start += 3) {
      Wire.beginTransmission(oledAddress);
      Wire.write(0x40);
      for (byte col = start; col < start + 3 && col < 16; ++col) {
        const byte code = padded[col];
        const byte first = pgm_read_byte(u8x8_font_chroma48medium8_r);
        const byte last = pgm_read_byte(u8x8_font_chroma48medium8_r + 1);
        for (byte bit = 0; bit < 8; ++bit) {
          const byte pixel = code >= first && code <= last ?
            pgm_read_byte(u8x8_font_chroma48medium8_r + 4 + (code - first) * 8 + bit) : 0;
          Wire.write(pixel);
        }
      }
      if (Wire.endTransmission()) return;
    }
    if (!Wire.getWireTimeoutFlag()) memcpy(previous[y], padded, sizeof(padded));
  }
};
