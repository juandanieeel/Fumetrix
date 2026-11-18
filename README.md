# Fumetrix

Fumetrix monitors air quality, controls an extraction fan, and records sensor readings to microSD and Cloudflare D1.

| Folder | Contents |
| --- | --- |
| `ProMini_Fan_Test/` | Arduino Pro Mini 5 V / 16 MHz firmware, sensor and protocol libraries, and SD logging |
| `ESP01_UART_Wake/` | ESP-01S (ESP8266) firmware for Wi-Fi setup, dashboard commands, and Arduino wake |
| `Fumetrix_Website/` | Python dashboard, D1 integration, and CSV and PDF history exports |

## Firmware

- Open `ProMini_Fan_Test/ProMini_Fan_Test.ino` with the Arduino AVR core for the Arduino Pro Mini ATmega328P, 5 V / 16 MHz. Install the U8g2 library for the OLED's U8x8 interface.
- Open `ESP01_UART_Wake/ESP01_UART_Wake.ino` with the ESP8266 Arduino core. Copy `SdResetKey.h.example` to `SdResetKey.h` in the sketch folder and fill in the setup password and SD reset key before compiling for the ESP-01S. The website's optional SD reset action needs the matching key in `Fumetrix_Website/data/sd-reset-key.txt`. Keep both local files out of Git.

## Website on Windows

Run these commands in PowerShell from the repository root with Python 3.11 installed.

```powershell
cd .\Fumetrix_Website
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe .\d1_store.py configure
.\.venv\Scripts\python.exe .\configure.py
.\START_FUMETRIX.bat
```

Enter the project D1 account ID, database ID and API token when prompted. Enter the ESP-01S LAN IP when running `configure.py`. The website prompts for an admin account when the database has none. Open http://127.0.0.1:5500 on the host. The files created in `Fumetrix_Website/data/` stay local.

## Source and test history

This package comes from the team's submitted Windows source snapshot. It includes the SD elapsed CSV/PDF export update, which was prepared after the first two test exports. Treat this as the final source snapshot for submission. Any claim that a specific earlier test used this exact revision needs matching source backups or deployment records for that test.

For Chapter 10, link to the GitHub repository and the commit used for the final demonstration. Record the firmware and website revision alongside the test logs.
