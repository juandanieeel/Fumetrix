#include "FumetrixProtocol.h"
#include <string.h>
#include <avr/pgmspace.h>

const char* FumetrixProtocol::speedSetting() const { return data + 6; }

bool FumetrixProtocol::poll() {
    if ((used || dropping) && millis() - lastByte > 500UL) reset();
    while (uart.available()) {
      char c = uart.read();
      lastByte = millis();
      if (c == '\r') continue;
      if (c == '\n') {
        bool complete = !dropping && used > 0;
        data[used] = '\0';
        used = 0;
        dropping = false;
        if (complete) return true;
      } else if (c < 32 || c > 126 || used >= sizeof(data) - 1) {
        dropping = true;
      } else if (!dropping) {
        data[used++] = c;
      }
    }
    return false;
  }

const char* FumetrixProtocol::line() const { return data; }

void FumetrixProtocol::reset() { used = 0; dropping = false; }

FumetrixProtocol::Command FumetrixProtocol::nextCommand() {
  if (!poll()) return Command::None;
  // Ordered as Command, starting at Sleep. A trailing comma marks a prefix.
  const static char names[] PROGMEM =
    "SLEEP\0FAN_ON\0FAN_OFF\0SPEED,\0STATUS\0OLEDGAS,\0MODE,AUTO\0"
    "MODE,MAINTENANCE\0OLED,ON\0OLED,OFF\0LIMITS,\0SHUTDOWN\0"
    "BUZZER,\0SDGET,\0SDRESET,\0OLEDWIFI,\0AUTOFAN,\0";
  const char* entry = names;
  for (byte command = byte(Command::Sleep); command <= byte(Command::AutoFanOverride); ++command) {
    const size_t length = strlen_P(entry);
    if (!strncmp_P(data, entry, length) &&
        (!data[length] || pgm_read_byte(entry + length - 1) == ',')) {
      return Command(command);
    }
    entry += length + 1;
  }
  return Command::None;
}

void FumetrixProtocol::rpm(unsigned long value) {
  uart.print(F("RPM,"));
  uart.println(value);
}

void FumetrixProtocol::speed(byte duty) {
  uart.print(F("SPEED,WEB,"));
  uart.println(duty);
}

void FumetrixProtocol::fan(bool on) {
  uart.println(on ? F("FAN,ON") : F("FAN,OFF"));
}

void FumetrixProtocol::awake(bool wakeHigh) {
  uart.println(F("STATE,AWAKE"));
  uart.println(wakeHigh ? F("PIN,HIGH") : F("PIN,LOW"));
}

void FumetrixProtocol::sleeping() { uart.println(F("STATE,SLEEP")); }
void FumetrixProtocol::wakeHeldLow() { uart.println(F("ERR,WAKE_LOW")); }
void FumetrixProtocol::flush() { uart.flush(); }

void FumetrixProtocol::discardInput() {
  while (uart.available()) uart.read();
  reset();
}

static void printHundredths(Stream& uart, uint16_t value) {
  uart.print(value / 100);
  uart.write('.');
  uart.write('0' + value / 10 % 10);
  uart.write('0' + value % 10);
}

void FumetrixProtocol::climate(int16_t temp100, uint16_t rh100) {
  uart.print(F("DATA,"));
  if (temp100 < 0) uart.write('-');
  printHundredths(uart, abs(temp100));
  uart.write(',');
  printHundredths(uart, rh100);
  uart.println();
}

void FumetrixProtocol::climateError(bool readError) {
  uart.println(readError ? F("ERR,SHT_READ") : F("ERR,SHT_MISSING"));
}

void FumetrixProtocol::gasStatus(const __FlashStringHelper* status) {
  uart.println(status);
}

void FumetrixProtocol::gasSample(uint16_t sequence, uint16_t voc, uint16_t nox, bool compensated) {
  uart.print(F("GASRAW,"));
  uart.print(sequence);
  uart.print(',');
  uart.print(voc);
  uart.print(',');
  uart.print(nox);
  uart.print(',');
  uart.println(compensated ? 1 : 0);
}

void FumetrixProtocol::particleSample(uint16_t pm1, uint16_t pm25, uint16_t pm10) {
  uart.print(F("PMDATA,"));
  uart.print(pm1);
  uart.print(',');
  uart.print(pm25);
  uart.print(',');
  uart.println(pm10);
}

void FumetrixProtocol::particleStatus(const char* status) {
  uart.print(F("PMSTATE,"));
  uart.println(status);
}

void FumetrixProtocol::mode(bool maintenance) {
  uart.println(maintenance ? F("MODE,MAINTENANCE") : F("MODE,AUTO"));
}
void FumetrixProtocol::oled(const char* status) {
  uart.print(F("OLED,")); uart.println(status);
}
