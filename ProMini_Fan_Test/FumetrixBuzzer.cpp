#include "FumetrixBuzzer.h"
#include <avr/pgmspace.h>
#include <util/atomic.h>

// Set true for an active-high active buzzer. It plays rhythms at its own pitch.
constexpr bool ACTIVE_BUZZER = false;
volatile byte buzzerPeriod = 0;
volatile byte buzzerTicks = 0;

namespace {
// Store the timer period and five-millisecond units directly in flash.
struct Note { byte period; byte duration; };
constexpr byte notePeriod(uint16_t hz) { return hz ? (12500UL + hz / 2) / hz : 0; }
#define N(hz, ms) {notePeriod(hz), (ms) / 5}
const Note startup[] PROGMEM = {N(523,90),N(659,90),N(784,145)};
const Note sleep[] PROGMEM = {N(659,110),N(440,170)};
const Note shutdown[] PROGMEM = {N(784,90),N(659,90),N(523,145)};
const Note maintenance[] PROGMEM = {N(698,95),N(0,65),N(698,160)};
const Note autoMode[] PROGMEM = {N(523,90),N(698,150)};
const Note fanStart[] PROGMEM = {N(880,100),N(1175,160)};
const Note fanStop[] PROGMEM = {N(1175,100),N(880,160)};
const Note test[] PROGMEM = {N(784,220)};
const Note birthday[] PROGMEM = {
  N(392,180),N(392,180),N(440,360),N(392,360),N(523,360),N(494,680),
  N(392,180),N(392,180),N(440,360),N(392,360),N(587,360),N(523,680),
  N(392,180),N(392,180),N(784,360),N(659,360),N(523,360),N(494,360),N(440,680),
  N(698,180),N(698,180),N(659,360),N(523,360),N(587,360),N(523,760)
};
const Note jingle[] PROGMEM = {
  N(330,200),N(330,200),N(330,400),N(330,200),N(330,200),N(330,400),
  N(330,200),N(392,200),N(262,300),N(294,100),N(330,600),
  N(349,200),N(349,200),N(349,300),N(349,100),N(349,200),N(330,200),
  N(330,200),N(330,100),N(330,100),N(294,200),N(294,200),N(330,200),N(294,400),N(392,400)
};
const Note twinkle[] PROGMEM = {
  N(262,280),N(262,280),N(392,280),N(392,280),N(440,280),N(440,280),N(392,560),
  N(349,280),N(349,280),N(330,280),N(330,280),N(294,280),N(294,280),N(262,560)
};
const Note ode[] PROGMEM = {
  N(330,230),N(330,230),N(349,230),N(392,230),N(392,230),N(349,230),N(330,230),N(294,230),
  N(262,230),N(262,230),N(294,230),N(330,230),N(330,340),N(294,120),N(294,460)
};
#undef N

const Note* notes = nullptr;
byte remaining = 0;
uint32_t nextNoteAt = 0;
uint32_t silenceAt = 0;
bool sounding = false;

void setPitch(byte pitch) {
  const byte period = ACTIVE_BUZZER ? 0 : pitch;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    buzzerPeriod = buzzerTicks = period;
    if (ACTIVE_BUZZER && pitch) PORTB |= _BV(PB1);
    else PORTB &= ~_BV(PB1);
    if (period) {
      if (!(TIMSK1 & _BV(OCIE1A))) TIFR1 = _BV(OCF1A);
      TIMSK1 |= _BV(OCIE1A);
    } else if (!(TIMSK1 & _BV(OCIE1B))) {
      TIMSK1 &= ~_BV(OCIE1A);
    }
  }
  sounding = pitch != 0;
}

void start(const Note* melody, byte count) {
  notes = melody;
  remaining = count;
  nextNoteAt = millis();
  buzzerUpdate();
}
} // namespace

void buzzerBegin() {
  digitalWrite(9, LOW);
  pinMode(9, OUTPUT);
}

void buzzerStop() {
  notes = nullptr;
  remaining = 0;
  setPitch(0);
}

bool buzzerPlaying() { return notes != nullptr; }

void buzzerUpdate() {
  if (!notes) return;
  const uint32_t now = millis();
  if (sounding && int32_t(now - silenceAt) >= 0) setPitch(0);
  if (int32_t(now - nextNoteAt) < 0) return;
  if (!remaining) {
    buzzerStop();
    return;
  }
  const byte pitch = pgm_read_byte(&notes->period);
  const uint16_t duration = uint16_t(pgm_read_byte(&notes->duration)) * 5;
  ++notes;
  --remaining;
  nextNoteAt = now + duration;
  silenceAt = nextNoteAt - 25;
  setPitch(pitch);
}

void buzzerPlay(BuzzerSound sound) {
  switch (sound) {
    case BuzzerSound::Startup: start(startup, sizeof(startup) / sizeof(Note)); break;
    case BuzzerSound::Sleep: start(sleep, sizeof(sleep) / sizeof(Note)); break;
    case BuzzerSound::Shutdown: start(shutdown, sizeof(shutdown) / sizeof(Note)); break;
    case BuzzerSound::Maintenance: start(maintenance, sizeof(maintenance) / sizeof(Note)); break;
    case BuzzerSound::Auto: start(autoMode, sizeof(autoMode) / sizeof(Note)); break;
    case BuzzerSound::FanStart: start(fanStart, sizeof(fanStart) / sizeof(Note)); break;
    case BuzzerSound::FanStop: start(fanStop, sizeof(fanStop) / sizeof(Note)); break;
    case BuzzerSound::Test: start(test, sizeof(test) / sizeof(Note)); break;
    case BuzzerSound::Birthday: start(birthday, sizeof(birthday) / sizeof(Note)); break;
    case BuzzerSound::Jingle: start(jingle, sizeof(jingle) / sizeof(Note)); break;
    case BuzzerSound::Twinkle: start(twinkle, sizeof(twinkle) / sizeof(Note)); break;
    case BuzzerSound::Ode: start(ode, sizeof(ode) / sizeof(Note)); break;
  }
}
