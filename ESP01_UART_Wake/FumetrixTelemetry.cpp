#include "FumetrixTelemetry.h"
#include <stdlib.h>
#include <string.h>

bool FumetrixLimits::parse(const char* input) {
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

bool LineReceiver::poll(Stream &port) {
    if ((used || dropping) && millis() - lastByte > 500UL) reset();
    while (port.available()) {
      char c = port.read();
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

const char * LineReceiver::line() const { return data; }

void LineReceiver::reset() { used = 0; dropping = false; }

void GasIndices::reset(const char* reason) {
    GasIndexAlgorithm_init(&vocAlgorithm, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
    GasIndexAlgorithm_init(&noxAlgorithm, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);
    voc = nox = 0; haveSample = false; status = reason;
  }

void GasIndices::receive(const char* line) {
    if (!strcmp(line, "GAS,RESET")) { reset("WARMING"); return; }
    if (!strcmp(line, "GAS,CONDITIONING")) { reset("CONDITIONING"); return; }
    if (!strcmp(line, "GAS,ERROR")) { reset("ERROR"); return; }
    if (strncmp(line, "GASRAW,", 7)) return;
    const char* p = line + 7;
    uint32_t fields[4];
    for (byte i=0;i<4;++i) {
      if (*p < '0' || *p > '9') return;
      char* end; fields[i] = strtoul(p, &end, 10);
      if (fields[i] > (i==3 ? 1UL : 65535UL)) return;
      if ((i<3 && *end != ',') || (i==3 && *end != '\0')) return;
      p = end + (i<3 ? 1 : 0);
    }
    // Zero/full-scale raw values are not valid input for the algorithm.
    if (!fields[1] || fields[1] >= 65000 || !fields[2] || fields[2] >= 65000) {
      reset("ERROR"); return;
    }
    const unsigned long now = millis();
    if (haveSample && fields[0] == sequence) return; // Never process a sample twice.
    if (!haveSample || (uint16_t)(sequence + 1) != fields[0] || now - sampleAt > 2500UL) reset("WARMING");
    GasIndexAlgorithm_process(&vocAlgorithm, fields[1], &voc);
    GasIndexAlgorithm_process(&noxAlgorithm, fields[2], &nox);
    sequence = fields[0]; sampleAt = now; haveSample = true;
    compensated = fields[3] != 0;
    status = voc > 0 && nox > 0 ? "READY" : "WARMING";
  }

void GasIndices::appendStatus(String &out, bool awake, bool sleeping) const {
    const bool fresh = awake && haveSample && millis() - sampleAt < 3000UL;
    const char* label = statusLabel(awake, sleeping, fresh);
    out += "\nSGP41: "; out += label;
    if (fresh) {
      out += "\nGas age: "; out += String(millis() - sampleAt); out += " ms";
      out += "\nGas compensation: "; out += compensated ? "SHT40" : "DEFAULT";
      if (voc > 0 && nox > 0) {
        out += "\nVOC index: "; out += String(voc);
        out += "\nNOx index: "; out += String(nox);
      }
    }
  }

void GasIndices::sendToOled(Stream& port) const {
    const bool fresh = haveSample && millis() - sampleAt < 3000UL;
    const char* label = statusLabel(true, false, fresh);
    const bool ready = !strcmp(label, "READY");
    if (ready && (voc < 1 || voc > 500 || nox < 1 || nox > 500)) return;
    port.print(F("OLEDGAS,"));
    port.print(label);
    port.print(',');
    port.print(ready ? voc : 0);
    port.print(',');
    port.println(ready ? nox : 0);
  }

const char* GasIndices::statusLabel(bool awake, bool sleeping, bool fresh) const {
    return sleeping ? "OFF" : !awake ? "WAITING" : haveSample && !fresh ? "STALE" : status;
  }

void ParticleReadings::reset() { have=false; label="WAITING"; }

void ParticleReadings::receive(const char* line) {
    if(!strncmp(line,"PMSTATE,",8)) {
      const char* s=line+8;
      const char* labels[]={"READY","WARMING","ERROR","MISSING","STALE","OFF"};
      for(const char* item:labels) if(!strcmp(s,item)) {
        label=item; if(strcmp(item,"READY")) have=false; return;
      }
    } else if(!strncmp(line,"PMDATA,",7)) {
      uint16_t values[3];const char* p=line+7;
      for(byte i=0;i<3;++i) {
        if(*p<'0'||*p>'9')return;
        char* end;unsigned long v=strtoul(p,&end,10);
        if(v>65535UL || *end!=(i==2?'\0':',')) return;
        values[i]=v;p=end+1;
      }
      pm1=values[0];pm25=values[1];pm10=values[2];
      sampleAt=millis();have=true;label="READY";
    }
  }

void ParticleReadings::appendStatus(String& out,bool awake,bool asleep) const {
    const unsigned long age=millis()-sampleAt;
    const bool fresh=awake && have && !strcmp(label,"READY") && age<5000UL;
    const char* s=asleep?"OFF":!awake?"WAITING":
      (!strcmp(label,"READY")&&!fresh)?"STALE":label;
    out+="\nPMS7003: ";out+=s;
    if(fresh) {
      out+="\nPM age: ";out+=String(age);out+=" ms";
      out+="\nPM1.0: ";out+=String(pm1);
      out+="\nPM2.5: ";out+=String(pm25);
      out+="\nPM10: ";out+=String(pm10);
    }
  }
