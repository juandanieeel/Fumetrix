# Fan test update
See ../START_HERE.md. Upload BOTH included sketches before testing website Fan ON/OFF.
Fan power control on D10 is implemented here; PWM/RPM and full MAINTENANCE remain pending.

# Fumetrix — Single-choice modes and faster sleep/wake

## What changed in this update
- One radio selector: AUTO / MAINTENANCE / SLEEP, separate from Dashboard/History.
- AUTO wakes the Arduino and resumes the existing SHT40 test; SLEEP requests power-down.
- MAINTENANCE is visibly unavailable until the firmware actually supports it.
  The selector does not claim final automatic fan control is implemented.
- A click immediately shows a pending selection. Confirmed state changes only
  after the ESP reports the Arduino acknowledgement. Offline/unknown is not AUTO.
- Removed the unconditional 6-second lock and 2-second post-command cooldown.
  The bridge unlocks as soon as the requested state is acknowledged.
- Bridge polls every ~1 second normally and every ~200 ms while a command is pending,
  plus network request time. Browser status checks use ~1 second / ~250 ms respectively.
- Device snapshots no longer wait behind an ESP network timeout. Charts/history
  refresh separately every 30 seconds and when opening/filtering History.
- The remaining 6-second timeout is for missing confirmation, not a mandatory wait.
  Commands are never automatically repeated after uncertain delivery.
- Website only: no ESP/Arduino upload, DNS change, account reset or database migration
  beyond the existing additive authentication setup is required.

## Install this update (existing website)
1. Stop the website with Ctrl+C. Leave Cloudflared running.
2. Back up the entire existing Fumetrix_Website folder while the server is stopped.
3. Extract the new ZIP separately. Copy the CONTENTS of its Fumetrix_Website folder
   into your existing working folder, replacing matching source files.
4. Keep the existing data/ and .venv/ folders. This ZIP includes neither.
5. Double-click START_FUMETRIX.bat. On the first run, the terminal asks you to
   create an admin username and password (12–128 characters, entered twice).
   Password input is hidden. There is no default account/password.
6. Keep the terminal open. Visit https://myfumetrix.online and press Ctrl+F5.
7. Sign in using the new account. Click Admin at the top to manage users.

Subsequent starts: START_FUMETRIX.bat. Admin setup is not repeated.
Forgot the admin password? Run SETUP_ADMIN.bat locally, enter the existing username,
choose a new password, and type RESET when prompted. This revokes old sessions.
If the ESP address changes, stop the server and run CONNECT_ESP.bat.
The old shared control password is no longer used by the website.

## Accounts and roles
- Admin: dashboard, history/export, sleep/wake, create accounts, update roles,
  disable/re-enable accounts, reset passwords, view recent activity and system summary.
- Operator: dashboard, history/export, sleep/wake, own password change.
- Viewer: dashboard, history/export, own password change. No device commands.
- All pages/API data require login. Admins create accounts; no public registration.
- My account changes your password; all your sessions then require sign-in again.
- Logout revokes that session. Sessions expire after 8 hours.
- Disabling, resetting or changing an account's role revokes its active sessions.
- An admin cannot disable/demote their own account; the last active admin is protected.
- Passwords are stored as scrypt hashes. Forms/commands have CSRF protection.
- Global login throttle: at most 10 attempts per minute across the server.
- Public-site cookies are Secure, HttpOnly and SameSite=Lax. Loopback HTTP is supported.
- User, session and activity tables are added to the existing SQLite database;
  readings and sleep_sessions are preserved. Never delete data/ to update.
- Activity includes login/logout, account changes and command requests/results.
  Request success does not independently prove physical sleep/wake.
- accounts.py owns authentication; setup_admin.py handles local setup/recovery.

Both laptop and ESP must be reachable on the same LAN. Phone can view the public
HTTPS site from another network. Do not enter 192.168.4.1 as the ESP address when
laptop is on Dorm Wi-Fi. Do not change the Cloudflare route from 127.0.0.1:5500.
If IP changes after reboot, get the new address from ESP /wifi or router clients.

## Implemented in this step
- Live SHT40 values and Arduino acknowledgement state, polled every ~1 second (faster during commands).
- Authenticated sleep/wake POST commands forwarded by the laptop backend.
- Signed-in admin/operator authorization is checked on each command.
- Login throttling, session revocation, role checks and activity logging.
- Device timeouts, missing samples and unknown state shown without fake zero values.
- Real local SQLite sensor logging at 3-minute intervals, independent of open browser tabs.
- First fresh sample is saved immediately on each server start.
- Sleep rows, date filters, CSV export, historical sensor selector and chart.
- No device credentials, DB files or source files served as static content.

Website viewing, history and controls require a signed-in account. Use public HTTPS
or localhost to sign in.
The ESP's original local control page remains unchanged and accessible on LAN.

## Limits / remaining final project tasks
- This uses the current SHT40 test firmware. Logged mode is AWAKE_TEST, not a
  claim that final automatic fan behavior has been implemented.
- Full AUTO/MAINTENANCE and PMS7003/SGP41/fan/buzzer/OLED telemetry remain pending.
- AP/STA setup remains on ESP /wifi and is not changed by this package.
- Sleep detection uses STATE,SLEEP preparation acknowledgement, not a measured
  current or direct proof of CPU power state. Verify physical behavior on hardware.
- Sleep times are when server polls observed transitions, approximately 1 second
  resolution in normal polling, faster during website commands, plus network delay. A successful POST alone does not prove sleep.
- First observation already asleep creates no invented start timestamp.
- If connection/state is lost or server restarts, an open interval is marked
  interrupted with unknown wake time/duration. No duration is fabricated across gaps.
- A very brief sleep/wake between polls may be missed. Durable event IDs and
  device timestamps will be added with final offline/sync protocol.
- Separate online database and offline-to-online synchronization are NOT included.
  Tunnel access to this laptop's SQLite is not a separate online database.
- When laptop server is off, this version logs nothing. SD buffering/replay remains pending.
- PDF exports and 36-hour test-session tracking remain pending.
- Do not overwrite or erase data/fumetrix.db when updating.

## Testing
27 automated tests pass using isolated temporary databases and simulated ESP responses.
They cover authentication, CSRF, role boundaries, revocation, password changes,
rate limiting, cookie flags, migration/data preservation and the existing logging,
command, export and offline behavior. JavaScript syntax check passes.
Single-choice modes, pending/confirmed sleep/wake, and independent History navigation
reviewed in a desktop browser with isolated mock responses. Preview mocks are excluded
from the ZIP. The managed preview lacked Flask, so full backend integration was
checked by automated tests rather than a live browser/server/hardware combination.
Windows startup and the updated interface with your physical ESP require a local check.
Run tests: .venv\Scripts\python.exe -m unittest -v

## Boundaries
These accounts protect the laptop dashboard/API. The ESP's own local AP/STA test
page retains its existing access behavior; website roles do not secure that page.
No hardware firmware change is included. Online database/sync is still pending.
See REQUIREMENTS_CHECKLIST.md for the remaining final-output requirements.

## Sources
Authentication implementation follows Flask session/CSRF guidance:
https://flask.palletsprojects.com/en/stable/web-security/

## Quick acceptance test after updating
1. Sign in as admin/operator. AUTO is selected when Arduino reports awake.
2. Select SLEEP: immediately see Changing mode, then Confirmed: SLEEP.
   Verify the actual A1/A2 rails and OLED power down as before.
3. Select AUTO once confirmation arrives: no extra fixed 6-second lock.
   Confirm awake, rail power and fresh SHT40 readings resume.
4. Open History: the current mode must not change.
5. A viewer cannot switch modes. If ESP goes offline, controls are unavailable.
6. If a command times out, inspect the reported state before trying again.
   Actual latency depends on local Wi-Fi, Cloudflare and device firmware.
