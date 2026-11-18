#pragma once
#include <stdlib.h>

// Temperature values are stored in tenths of a degree.
// Wire order: temperature min/max, humidity min/max, PM2.5 max, VOC max, NOx max.
struct FumetrixLimits {
  bool configured = false;
  long values[7] = {};

  bool parse(const char* input) {
    long parsed[7];
    for (unsigned i = 0; i < 7; ++i) {
      if (i < 2 && *input == '-') {
        if (input[1] < '0' || input[1] > '9') return false;
      } else if (*input < '0' || *input > '9') {
        return false;
      }
      char* end;
      parsed[i] = strtol(input, &end, 10);
      if (end - input > 6) return false;
      if (i < 2) {
        const bool negative = *input == '-';
        parsed[i] *= 10;
        if (*end == '.') {
          ++end;
          if (*end < '0' || *end > '9') return false;
          const long tenth = *end++ - '0';
          parsed[i] += negative ? -tenth : tenth;
        }
      }
      if (*end != (i == 6 ? '\0' : ',')) return false;
      input = end + (i != 6);
    }
    if (parsed[0] < -450 || parsed[1] > 1300 || parsed[0] > parsed[1] ||
        parsed[2] < 0 || parsed[3] > 100 || parsed[2] > parsed[3] ||
        parsed[4] < 0 || parsed[4] > 65535L ||
        parsed[5] < 1 || parsed[5] > 500 || parsed[6] < 1 || parsed[6] > 500) return false;
    for (unsigned i = 0; i < 7; ++i) values[i] = parsed[i];
    configured = true;
    return true;
  }
};
