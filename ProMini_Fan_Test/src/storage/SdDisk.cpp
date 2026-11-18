#include "SdJournal.h"
#include "diskio.h"

// A3 (PC3) selects the card; D10 is already an output for the fan relay.
namespace {
bool blockAddressing;
uint16_t writeLeft;
byte exchange(byte value = 255) {
  SPDR = value;
  while (!(SPSR & _BV(SPIF))) {}
  return SPDR;
}
void release() {
  PORTC |= _BV(PC3);
  exchange();
}
byte command(byte cmd, uint32_t arg) {
  release();
  PORTC &= ~_BV(PC3);
  exchange();
  exchange(0x40 | cmd);
  for (byte n = 4; n; --n) exchange(arg >> ((n - 1) * 8));
  exchange(cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01);
  for (byte n = 0; n < 10; ++n) {
    const byte reply = exchange();
    if (!(reply & 0x80)) return reply;
  }
  return 255;
}
bool waitFor(byte token, uint16_t timeout) {
  const unsigned long start = millis();
  do {
    if (exchange() == token) return true;
  } while (millis() - start < timeout);
  return false;
}
}

void sdReleaseBus() {
  // Release A3 and D11–D13 before cutting the peripheral rails.
  // sdBegin() reinitializes the SPI peripheral and pins after wake.
  constexpr uint8_t spiPins = _BV(PB3) | _BV(PB4) | _BV(PB5);
  SPCR = 0;
  DDRC &= ~_BV(PC3);
  PORTC &= ~_BV(PC3);
  DDRB &= ~spiPins;
  PORTB &= ~spiPins;
}

DSTATUS disk_initialize() {
  digitalWrite(A3, HIGH);
  pinMode(A3, OUTPUT);
  pinMode(MOSI, OUTPUT);
  pinMode(SCK, OUTPUT);
  pinMode(MISO, INPUT);
  SPCR = _BV(SPE) | _BV(MSTR) | _BV(SPR1); // 250 kHz for card setup.
  SPSR = 0;
  for (byte i = 0; i < 10; ++i) exchange();
  // CMD8 is optional on older standard-capacity SD cards.
  bool ok = command(0, 0) == 1;
  bool version2 = false;
  blockAddressing = false;
  if (ok) {
    const byte reply = command(8, 0x1AA);
    version2 = reply == 1;
    if (version2) {
      exchange(); exchange();
      const byte voltage = exchange();
      ok = exchange() == 0xAA && voltage == 1;
    } else {
      ok = reply == 0x05; // Idle plus illegal command on SD v1.
    }
  }
  if (ok) {
    const unsigned long start = millis();
    byte reply;
    do {
      reply = command(55, 0);
      if (reply > 1) break;
      reply = command(41, version2 ? 0x40000000UL : 0);
    } while (reply == 1 && millis() - start < 1000);
    ok = reply == 0;
    if (ok && version2) {
      ok = command(58, 0) == 0;
      if (ok) {
        blockAddressing = (exchange() & 0x40) != 0;
        exchange(); exchange(); exchange();
      }
    }
    if (ok && !blockAddressing) ok = command(16, 512) == 0;
  }
  release();
  SPCR = _BV(SPE) | _BV(MSTR);
  return ok ? 0 : STA_NOINIT;
}

DRESULT disk_readp(BYTE* buff, DWORD sector, UINT offset, UINT count) {
  if (offset + count > 512 || !buff) return RES_PARERR;
  if (command(17, blockAddressing ? sector : sector * 512UL) || !waitFor(0xFE, 100)) {
    release();
    return RES_ERROR;
  }
  for (UINT i = 0; i < 512; ++i) {
    const byte value = exchange();
    if (i >= offset && count) { *buff++ = value; --count; }
  }
  exchange(); exchange();
  release();
  return RES_OK;
}

DRESULT disk_writep(const BYTE* buff, DWORD arg) {
  if (buff) {
    if (arg > writeLeft) return RES_PARERR;
    writeLeft -= arg;
    while (arg--) exchange(*buff++);
    return RES_OK;
  }
  if (arg) {
    if (command(24, blockAddressing ? arg : arg * 512UL)) {
      release();
      return RES_ERROR;
    }
    exchange(); exchange(0xFE);
    writeLeft = 512;
    return RES_OK;
  }
  while (writeLeft) { exchange(0); --writeLeft; }
  exchange(0); exchange(0);
  const bool accepted = (exchange() & 31) == 5;
  const bool ready = waitFor(255, 600);
  release();
  return accepted && ready ? RES_OK : RES_ERROR;
}
