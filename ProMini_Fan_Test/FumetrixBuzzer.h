#pragma once
#include <Arduino.h>

enum class BuzzerSound : byte {
  Startup, Sleep, Shutdown, Maintenance, Auto, Test, Birthday, Jingle, Twinkle, Ode, FanStart, FanStop
};

void buzzerBegin();
void buzzerPlay(BuzzerSound sound);
void buzzerStop();
bool buzzerPlaying();
void buzzerUpdate();

// Used by the existing 25 kHz Timer1 fan interrupt.
extern volatile byte buzzerPeriod;
extern volatile byte buzzerTicks;

inline __attribute__((always_inline)) void buzzerTimerTick() {
  if (buzzerPeriod && --buzzerTicks == 0) {
    PINB = _BV(PB1); // Toggle D9 only.
    buzzerTicks = buzzerPeriod;
  }
}
