#pragma once
#include <stdint.h>

class AutoFan {
 public:
  static constexpr uint8_t KEEP_RUNNING_DUTY = 40;
  // pm25 == -1 and gas indices == 0 mean no fresh reading.
  uint8_t update(uint32_t now, long pm25, unsigned voc, unsigned nox,
                 long pmLimit, long vocLimit, long noxLimit, bool keepRunning = false) {
    if (!keepRunning && pm25 < 0 && !voc && !nox) {
      qualifying = clearing = reducing = false;
      return duty; // Keep extracting if readings disappear during a run.
    }

    // Use a quieter first step for PM2.5 while keeping the full-speed threshold.
    const uint8_t pmDuty = pm25 >= 0 ? level(pm25, pmLimit, 26, 35, 40) : 0;
    uint8_t requested = pmDuty;
    const uint8_t vocDuty = voc ? level(voc, vocLimit, 50, 150) : 0;
    const uint8_t noxDuty = nox ? level(nox, noxLimit, 19, 49) : 0;
    if (vocDuty > requested) requested = vocDuty;
    if (noxDuty > requested) requested = noxDuty;
    if (keepRunning) return requested > KEEP_RUNNING_DUTY ? requested : KEEP_RUNNING_DUTY;

    const uint8_t sources = (pmDuty ? 1 : 0) | (vocDuty ? 2 : 0) | (noxDuty ? 4 : 0);
    if (duty) activeSources |= sources;
    const uint8_t available = (pm25 >= 0 ? 1 : 0) | (voc ? 2 : 0) | (nox ? 4 : 0);
    const bool lostSource = (activeSources & available) != activeSources;
    if (!duty) {
      if (!requested) qualifying = false;
      else if (!qualifying) { qualifying = true; changedAt = now; }
      else if (now - changedAt >= 2000) {
        qualifying = false;
        duty = requested;
        activeSources = sources;
        startedAt = now;
      }
    } else if (lostSource) {
      clearing = reducing = false;
      if (requested > duty) duty = requested;
    } else if (!requested) {
      reducing = false;
      if (!clearing) { clearing = true; changedAt = now; }
      else if (now - changedAt >= 45000) { duty = 0; clearing = false; activeSources = 0; }
      else if (now - changedAt >= 10000 && duty > 60) duty = 60;
    } else {
      clearing = false;
      if (requested >= duty) {
        duty = requested;
        reducing = false;
      } else if (!reducing || lowerDuty != requested) {
        reducing = true;
        lowerDuty = requested;
        changedAt = now;
      } else if (now - changedAt >= 10000) {
        duty = requested;
        reducing = false;
      }
    }
    return duty && now - startedAt < 1000 ? 100 : duty;
  }

 private:
  static uint8_t level(long reading, long limit, long mediumRise, long highRise,
                       uint8_t lowDuty = 60) {
    const long rise = reading - limit;
    if (rise >= highRise) return 100;
    if (rise >= mediumRise) return 80;
    return rise > 0 ? lowDuty : 0;
  }

  uint32_t startedAt = 0, changedAt = 0;
  uint8_t duty = 0, lowerDuty = 0, activeSources = 0;
  bool qualifying = false, clearing = false, reducing = false;
};
