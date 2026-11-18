#include "SdJournal.h"
#include "pff.h"
#include <stddef.h>

namespace {
constexpr uint16_t capacity = 4096;
constexpr uint16_t magic = 0xF17E;
FATFS volume;
struct LogHeader {
  uint32_t originalId, currentId, inverse;
};
LogHeader header;
uint32_t logId() { return header.currentId ? header.currentId : header.originalId; }
uint16_t nextSlot;
bool ready;

struct Entry {
  uint16_t signature;
  SdReading data;
  uint16_t checksum;
  uint32_t generation;
};

uint16_t checksum(const Entry& entry) {
  const byte* data = reinterpret_cast<const byte*>(&entry);
  uint16_t hash = 0xFFFF;
  for (byte i = 0; i < offsetof(Entry, checksum); ++i) {
    hash ^= data[i];
    for (byte bit = 0; bit < 8; ++bit)
      hash = (hash >> 1) ^ ((hash & 1) ? 0xA001 : 0);
  }
  return hash;
}

bool readSlot(uint16_t slot, Entry& entry) {
  UINT count;
  return pf_lseek((uint32_t(slot) + 1) * 512) == FR_OK &&
         pf_read(&entry, sizeof(entry), &count) == FR_OK && count == sizeof(entry);
}

bool valid(const Entry& entry) {
  // Legacy files have zero padding after the checksum.
  return entry.signature == magic && entry.generation == header.currentId &&
         entry.checksum == checksum(entry);
}

bool writeRecord(uint32_t offset, const void* record, UINT size) {
  UINT count;
  return pf_lseek(offset) == FR_OK &&
         pf_write(record, size, &count) == FR_OK && count == size &&
         pf_write(nullptr, 0, &count) == FR_OK;
}
}

void sdBegin() {
  UINT count;
  ready = pf_mount(&volume) == FR_OK && pf_open("FUMELOG.BIN") == FR_OK &&
          volume.fsize == (uint32_t(capacity) + 1) * 512 &&
          pf_read(&header, sizeof(header), &count) == FR_OK && count == sizeof(header) &&
          header.originalId && (header.currentId ? header.inverse == ~header.currentId : !header.inverse);
  if (!ready) return;
  // The provisioned file is zeroed. Append-only writes give a valid prefix.
  uint16_t lo = 0, hi = capacity;
  Entry entry;
  while (lo < hi) {
    const uint16_t mid = lo + (hi - lo) / 2;
    if (!readSlot(mid, entry)) { ready = false; return; }
    if (valid(entry)) lo = mid + 1;
    else hi = mid;
  }
  nextSlot = lo;
}

void sdAppend(const SdReading& reading) {
  if (!ready || nextSlot >= capacity) return;
  Entry entry{magic, reading, 0, header.currentId};
  entry.checksum = checksum(entry);
  if (!writeRecord((uint32_t(nextSlot) + 1) * 512, &entry, sizeof(entry))) {
    ready = false;
    return;
  }
  ++nextSlot;
}

void sdReport() {
  if (!ready) { Serial.println(F("SD,ERROR")); return; }
  Serial.print(F("SD,"));
  Serial.print(logId());
  Serial.print(',');
  Serial.println(nextSlot);
}

void sdSend(const char* index) {
  if (!ready) return;
  char* end;
  const long slot = strtol(index, &end, 10);
  if (end == index || *end || slot < 0 || slot >= nextSlot) return;
  Entry entry;
  if (!readSlot(slot, entry) || !valid(entry)) {
    ready = false;
    return;
  }
  const auto& r = entry.data;
  Serial.print(F("SDROW,"));
  Serial.print(logId()); Serial.print(',');
  Serial.print(slot); Serial.print(',');
  Serial.print(r.uptime); Serial.print(',');
  Serial.print(r.temperature); Serial.print(',');
  Serial.print(r.humidity); Serial.print(',');
  Serial.print(r.pm1); Serial.print(',');
  Serial.print(r.pm25); Serial.print(',');
  Serial.print(r.pm10); Serial.print(',');
  Serial.print(r.voc); Serial.print(',');
  Serial.print(r.nox); Serial.print(',');
  Serial.print(r.mode); Serial.print(',');
  Serial.println(r.duty);
}


void sdReset(const char* parameters) {
  if (!ready || !nextSlot) return;
  char* end;
  if (strtoul(parameters, &end, 10) != logId() || *end != ',') return;
  parameters = end + 1;
  if (strtoul(parameters, &end, 10) != nextSlot || *end != ',') return;
  parameters = end + 1;
  const uint32_t replacement = strtoul(parameters, &end, 10);
  if (*end || !replacement || replacement == logId() || replacement == header.originalId) return;

  const LogHeader updated{header.originalId, replacement, ~replacement};
  LogHeader confirmed;
  UINT count;
  if (!writeRecord(0, &updated, sizeof(updated)) || pf_lseek(0) != FR_OK ||
      pf_read(&confirmed, sizeof(confirmed), &count) != FR_OK || count != sizeof(confirmed) ||
      confirmed.currentId != replacement || confirmed.originalId != header.originalId ||
      confirmed.inverse != updated.inverse) {
    ready = false;
  } else {
    header = confirmed;
    nextSlot = 0;
  }
  sdReport();
}
