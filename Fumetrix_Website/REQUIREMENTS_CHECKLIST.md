# CpE 414 final-output checklist
Reviewed against CpE 414 Final Output(3).pdf supplied November 9, 2026.
This tracks implementation evidence, not a promised grade.

## Website / database
- Login (+2 listed extra credit): implemented in this update; test after installing.
- Admin/user roles: implemented; additional credit is subject to instructor approval.
- AUTO: SHT40 live display and local 3-minute logs work in test firmware.
  Remaining: all sensors, all output states, threshold recommendations and final automatic operation.
- MAINTENANCE: UI placeholder only. Must pause automatic operation, read each sensor
  on request and test every actuator. Still needs firmware + website integration.
- SLEEP: website sleep/wake and local sleep logs implemented; user reported test done.
  Preserve Arduino Power-Down/hardware interrupt and BOD timing. Final wake must return AUTO.
- Database: local SQLite implemented. Separate online database, offline storage during
  internet/server outage and automatic synchronization after recovery still pending.
  A Cloudflare Tunnel to local SQLite is not an online database implementation.
- If the laptop server stops, current logging stops; device-side buffering/replay pending.
- PDF log generation (+3): pending. CSV export already works but does not meet PDF feature.
- AP web configuration of EEPROM STA credentials (+5): firmware implemented;
  AP fallback tested by user. Verify settings persist across power loss.
- All 3 main modes must work: each missing/incomplete mode deducts one third of the
  total accumulated score. Database failure makes AUTO and SLEEP count as missing.

## Firmware / low power
- Pro Mini + ESP-01 choice is listed for +10; validate final power-saving behavior.
- Disabled unused pins and sensor power control: combined maximum +10.
- Power-down BOD disable must be correctly timed immediately before sleeping;
  incorrect/missing BOD disable deducts 5.
- One OOP sensor library for all sensors (+15), separate OOP communication library (+5).
- millis() timing, no delay() longer than 50ms except sleep (+3).
- enum + switch/case SLEEP/AUTO/MAINTENANCE (+3), hardware interrupt and web switching (+2), organized main .ino (+2).
- Final integration and final library organization remain pending.

## Hardware
- Complete schematic: all sensors, power, ground and signals.
- PCB starts at 20: each jumper -2; missing pin labels -5; ungrouped sensor pins -5.
- Verify actual final hardware; no score assumed from preliminary tests.

## Testing / progress
- 36 total real testing hours; sessions need not be continuous.
- Record each session's date, start and end. Do not infer test hours from uptime alone.
- 3-minute logs earn full testing points at >=36 hours; 4–5 minutes earns 18.
- Under 4 hours earns 0. Preserve genuine sensor and sleep records.
- Three weekly updates with evidence and each member's actual contribution.
- Each missed weekly update deducts 30% of total accumulated score.
- Version control required; establish repository and track changes.

## Documentation
11 chapters plus title and contents: overview; components; schematic/PCB; low power;
hardware setup; website guide; local/online databases and sync; additional features;
testing; complete code or repository; weekly progress.
Include component images/specifications, thresholds/interpretations, every web control,
actual tests, sleep durations and genuine contributions.
Documentation starts at 20: missing section -10; incomplete section -4; can go negative.
Long bond 8.5x13in, 1in margins; Calisto MT 11pt body/13pt bold chapter titles;
single spacing; numbered/captioned figures and tables; bottom-left page numbers.
