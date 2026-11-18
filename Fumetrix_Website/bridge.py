"""Poll the ESP bridge and relay authenticated device commands."""
from thresholds import load_limits
from d1_store import Unavailable
import secrets
from sd_sync import sync_sd
import ipaddress
import math
import re
import threading
import time
import urllib.request
from datetime import datetime, timezone

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None

def parse_status(text):
    first = text.splitlines()[0] if text else ''
    states = {'Arduino: AWAKE':'AWAKE',
              'Arduino: SLEEP preparation acknowledged':'SLEEPING',
              'Arduino: Waiting for sleep acknowledgement':'SLEEP_PENDING',
              'Arduino: Waiting for wake acknowledgement':'WAKING',
              'Arduino: UNKNOWN (no confirmed Arduino state)':'UNKNOWN'}
    if first not in states:
        raise ValueError('Unexpected ESP status response')
    result = dict(connected=True, state=states[first], temperature=None, humidity=None,
                  sample_age=None, message=text[:1500])
    t = re.search(r'^Temperature: (-?\d+(?:\.\d+)?) C$',text,re.M)
    h = re.search(r'^Humidity: (\d+(?:\.\d+)?) %$',text,re.M)
    age = re.search(r'^Reading age: (\d+) s$',text,re.M)
    if result['state']=='AWAKE' and t and h and age:
        temp, humidity, seconds = float(t[1]), float(h[1]), int(age[1])
        if math.isfinite(temp) and -45<=temp<=130 and 0<=humidity<=100 and seconds<6:
            result.update(temperature=temp,humidity=humidity,sample_age=seconds)
    fan = re.search(r'^Fan relay: (ON|OFF|PENDING|UNKNOWN)$',text,re.M)
    result['fan_supported']=fan is not None
    result['fan_relay']=fan[1] if fan else 'UNKNOWN'
    speed = re.search(r'^Fan speed: WEB,([0-9]{2,3})$',text,re.M)
    rpm = re.search(r'^Fan RPM: ([0-9]{1,5})$',text,re.M)
    result['fan_duty'] = None
    result['fan_rpm'] = None
    if result['state']=='AWAKE':
        if speed and 20 <= int(speed[1]) <= 100:
            result['fan_duty'] = int(speed[1])
        if result['fan_relay']=='ON' and rpm and int(rpm[1]) <= 30000:
            result['fan_rpm'] = int(rpm[1])
    result.update(voc_index=None, nox_index=None, gas_status='NOT_LINKED', gas_compensation=None)
    gas = re.search(r'^SGP41: (READY|WARMING|CONDITIONING|ERROR|WAITING|OFF|STALE)$', text, re.M)
    if gas:
        result['gas_status'] = gas[1]
    gas_age = re.search(r'^Gas age: ([0-9]{1,6}) ms$', text, re.M)
    comp = re.search(r'^Gas compensation: (SHT40|DEFAULT)$', text, re.M)
    if result['state'] == 'AWAKE' and gas_age and int(gas_age[1]) < 3000:
        result['gas_compensation'] = comp[1] if comp else None
        voc = re.search(r'^VOC index: ([0-9]{1,3})$', text, re.M)
        nox = re.search(r'^NOx index: ([0-9]{1,3})$', text, re.M)
        if result['gas_status'] == 'READY' and voc and nox and 1 <= int(voc[1]) <= 500 and 1 <= int(nox[1]) <= 500:
            result.update(voc_index=int(voc[1]), nox_index=int(nox[1]))
    if result['gas_status'] == 'READY' and result['voc_index'] is None:
        result['gas_status'] = 'STALE'
    result.update(pm1=None, pm25=None, pm10=None, pm_status='NOT_LINKED')
    pm = re.search(r'^PMS7003: (READY|WARMING|ERROR|MISSING|STALE|OFF|WAITING)$', text, re.M)
    if pm:
        result['pm_status'] = pm[1]
    age = re.search(r'^PM age: ([0-9]{1,10}) ms$', text, re.M)
    values = [re.search(r'^' + re.escape(label) + r': ([0-9]{1,5})$', text, re.M)
              for label in ('PM1.0', 'PM2.5', 'PM10')]
    if (result['state'] == 'AWAKE' and result['pm_status'] == 'READY' and
            age and int(age[1]) < 5000 and all(values) and
            all(int(v[1]) <= 65535 for v in values)):
        result.update(zip(('pm1', 'pm25', 'pm10'), (int(v[1]) for v in values)))
    if result['pm_status'] == 'READY' and result['pm25'] is None:
        result['pm_status'] = 'STALE'
    mode = re.search(r'^Mode: (AUTO|MAINTENANCE|SLEEP|UNKNOWN)$', text, re.M)
    oled = re.search(r'^OLED: (ON|OFF|MISSING|DISABLED|UNKNOWN)$', text, re.M)
    result['mode'] = mode[1] if mode else 'UNKNOWN'
    result['oled'] = oled[1] if oled else 'UNKNOWN'
    if result['state'] not in ('AWAKE', 'SLEEPING'):
        result['mode'] = result['oled'] = 'UNKNOWN'
    limits = re.search(r'^OLED limits: (UNSET|-?[0-9,.\-]{1,40})$', text, re.M)
    result['oled_limits'] = limits[1] if limits else None
    result['buzzer_supported'] = 'Buzzer: READY' in text.splitlines()
    buzzer = re.search(r'^Buzzer ACK: (test|stop|birthday|jingle|twinkle|ode)$', text, re.M)
    result['buzzer_ack'] = buzzer[1] if buzzer else None
    sd = re.search(r'^SD log: (ERROR|[0-9]{1,10},[0-9]{1,4})$', text, re.M)
    row = re.search(r'^SD row: ([-0-9,]{1,90})$', text, re.M)
    result['sd_reset_supported'] = 'SD reset: supported' in text.splitlines()
    result['sd_log'] = sd[1] if sd else None
    result['sd_row'] = row[1] if row else None
    override = re.search(r'^Fan override: (AUTO|ON|OFF|MANUAL)$', text, re.M)
    result['fan_override'] = override[1] if override and result['mode'] == 'AUTO' else None
    sht = re.search(r'^SHT40: (READY|MISSING|ERROR|WAITING|STALE|OFF)$', text, re.M)
    result['sht_status'] = ('READY' if result['temperature'] is not None else
                            sht[1] if sht and sht[1] != 'READY' else 'WAITING')
    return result

class Bridge:
    def __init__(self, ip, connect, transport=None, cloud=False):
        address=ipaddress.IPv4Address(ip)
        if not address.is_private or address.is_loopback or address.is_link_local:
            raise ValueError('ESP IP must be a private LAN IPv4 address.')
        self.url='http://'+str(address)
        self.connect=connect
        self.cloud = cloud
        self.cloud_retry_at = 0
        self.cloud_sleep = {}
        self.cloud_readings = {}
        self.cached_limits = None
        self.next_limits_read = 0
        self.lock=threading.RLock()
        self.io_lock=threading.Lock()
        self.poll_requested=threading.Event()
        self.opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect())
        self.transport=transport or self.fetch
        self.data=dict(connected=False,state='UNKNOWN',temperature=None,humidity=None,sample_age=None,message='Connecting to ESP…')
        self.seen_at=0
        self.changed_at=None
        self.confirmed_power_state=None
        self.last_log=0
        self.last_command=0
        self.next_limit_attempt = 0
        self.next_sd_request = 0
        self.last_synced_sd = None
        self.pending_until=0
        self.pending_target=None
        self.command_message=""
        self.sleep_id=None
        self.sleep_start=None
        self.sleep_mono=None
        self.stop=threading.Event()
        with self.connect() as db:
            db.execute("UPDATE sleep_sessions SET quality='interrupted: server restarted' WHERE wake_at IS NULL AND quality='observed'")

    def fetch(self,path,method='GET',headers=None):
        req=urllib.request.Request(self.url+path,data=b'' if method=='POST' else None,method=method,headers=headers or {})
        with self.opener.open(req,timeout=2) as response:
            raw=response.read(4097)
            if len(raw)>4096:raise ValueError('ESP response too large')
            return raw.decode('utf-8',errors='strict')

    def snapshot(self):
        with self.lock:
            result=dict(self.data)
            result.update(last_state_change=self.changed_at,bridge_ready=True)
            if time.monotonic()-self.seen_at>6:
                result.update(pm1=None,pm25=None,pm10=None,pm_status='STALE')
                result.update(mode='UNKNOWN',oled='UNKNOWN',connected=False,state='UNKNOWN',temperature=None,humidity=None,fan_relay='UNKNOWN',fan_rpm=None,fan_duty=None,voc_index=None,nox_index=None,gas_status='STALE',gas_compensation=None)
            self.expire_pending()
            result['command_pending']=self.pending_target is not None
            result['command_target']=self.pending_target
            result['command_message']=self.command_message
            return result

    def expire_pending(self):
        # Called with the state lock held. A timeout is not a confirmation.
        if self.pending_target and time.monotonic() >= self.pending_until:
            self.pending_target=None
            self.pending_until=0
            self.command_message='Confirmation timed out. Check Arduino state before retrying.'

    def update(self):
        # Serialize device requests, but never hold the state lock over network I/O.
        # Browsers can still read the last snapshot while the ESP is unreachable.
        with self.io_lock:
            try:
                current=parse_status(self.transport('/status'))
            except Exception:
                current=None
            now=time.monotonic()
            stamp=datetime.now(timezone.utc).isoformat()
            with self.lock:
                self.expire_pending()
                if current is not None:
                    self.apply_status(current,now,stamp)
                else:
                    self.apply_offline()
                awaiting_mode = (
                    self.pending_target in ('MODE:AUTO', 'MODE:MAINTENANCE', 'SLEEPING', 'AWAKE')
                    or self.data['state'] in ('WAKING', 'SLEEP_PENDING')
                )
            # Poll the ESP again promptly during a mode change, even if D1 is slow.
            if self.cloud and awaiting_mode:
                return
            if not self.cloud or now >= self.cloud_retry_at:
                try:
                    if self.cloud:
                        self.flush_cloud_logs()
                    if current and current['state'] == 'AWAKE' and current['sd_log'] and current['sd_log'] != 'ERROR':
                        sync_sd(self, current, now, stamp)
                    if current and current['state'] == 'AWAKE' and current['oled_limits'] is not None:
                        if not self.cloud or now >= self.next_limits_read:
                            with self.connect() as db:
                                self.cached_limits = load_limits(db)
                            self.next_limits_read = now + 10
                        if self.cached_limits and current['oled_limits'] != self.cached_limits and now >= self.next_limit_attempt:
                            self.next_limit_attempt = now + 2
                            try:
                                self.transport('/limits?values=' + self.cached_limits, 'POST')
                            except (OSError, ValueError):
                                pass
                except Unavailable:
                    self.cloud_retry_at = now + 10

    def flush_cloud_logs(self):
        if not (self.cloud_sleep or self.cloud_readings):
            return
        sleep = list(self.cloud_sleep.items())[:20]
        readings = list(self.cloud_readings.items())[:20]
        with self.connect() as db:
            for key, values in sleep:
                db.execute("""INSERT INTO sleep_sessions
                    (event_key,sleep_at,wake_at,duration_seconds,quality)
                    VALUES(?,?,?,?,?) ON CONFLICT(event_key) DO UPDATE SET
                    wake_at=excluded.wake_at,duration_seconds=excluded.duration_seconds,quality=excluded.quality""",
                    (key, *values))
            for key, values in readings:
                db.execute("""INSERT INTO readings
                    (live_key,recorded_at,mode,temperature,humidity,voc_index,nox_index,pm1,pm25,pm10)
                    VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(live_key) DO NOTHING""", (key, *values))
        for key, _ in sleep:
            del self.cloud_sleep[key]
        for key, _ in readings:
            del self.cloud_readings[key]

    def queue_cloud_sleep_end(self, wake_at=None, seconds=None, quality='observed'):
        self.cloud_sleep[self.sleep_id] = (self.sleep_start, wake_at, seconds, quality)
        self.sleep_id = None

    def queue_cloud_logs(self, current, previous, now, stamp):
        state = current['state']
        if state == 'SLEEPING' and previous in ('AWAKE', 'SLEEP_PENDING') and self.sleep_id is None:
            self.sleep_id = secrets.token_hex(16)
            self.sleep_start = stamp
            self.sleep_mono = now
            self.cloud_sleep[self.sleep_id] = (stamp, None, None, 'observed')
        if self.sleep_id and state == 'AWAKE':
            self.queue_cloud_sleep_end(stamp, max(0, int(now - self.sleep_mono)))
        elif self.sleep_id and state == 'UNKNOWN':
            self.queue_cloud_sleep_end(quality='interrupted: Arduino state unknown')
        sd = current.get('sd_log')
        if state != 'AWAKE' or (sd not in (None, 'ERROR') and not sd.endswith(',4096')):
            return
        if current.get('mode') == 'MAINTENANCE' or (self.last_log and now - self.last_log < 180):
            return
        if not any(current.get(key) is not None for key in ('temperature', 'voc_index', 'pm25')):
            return
        mode = 'MANUAL' if current.get('fan_override') == 'MANUAL' else current.get('mode')
        if mode not in ('AUTO', 'MANUAL'):
            mode = 'AWAKE_TEST'
        self.cloud_readings[secrets.token_hex(16)] = (stamp, mode, *(current.get(key) for key in
            ('temperature', 'humidity', 'voc_index', 'nox_index', 'pm1', 'pm25', 'pm10')))
        self.last_log = now

    def apply_status(self,current,now,stamp):
        previous=self.data['state']
        previous_mode = self.data.get('mode')
        self.seen_at=time.monotonic()
        state = current['state']
        if state in ('AWAKE', 'SLEEPING') and state != self.confirmed_power_state:
            self.confirmed_power_state = state
            self.changed_at = stamp
        if self.cloud:
            self.queue_cloud_logs(current, previous, now, stamp)
        else:
            with self.connect() as db:
                if current['state']=='SLEEPING' and previous in ('AWAKE','SLEEP_PENDING') and self.sleep_id is None:
                    cursor=db.execute("INSERT INTO sleep_sessions(sleep_at,quality) VALUES(?,'observed')",(stamp,))
                    self.sleep_id=cursor.lastrowid;self.sleep_start=stamp;self.sleep_mono=now
                if self.sleep_id and current['state']=='AWAKE':
                    db.execute('UPDATE sleep_sessions SET wake_at=?,duration_seconds=? WHERE id=?',
                               (stamp,max(0,int(now-self.sleep_mono)),self.sleep_id))
                    self.sleep_id=None
                if self.sleep_id and current['state']=='UNKNOWN':
                    db.execute("UPDATE sleep_sessions SET quality='interrupted: Arduino state unknown' WHERE id=?",(self.sleep_id,))
                    self.sleep_id=None
                if (current.get('sd_log') in (None, 'ERROR') or current['sd_log'].endswith(',4096')) and current.get('mode') != 'MAINTENANCE' and (current['temperature'] is not None or current.get('voc_index') is not None or current.get('pm25') is not None) and (self.last_log==0 or now-self.last_log>=180):
                    mode = current.get('mode')
                    if mode == 'AUTO' and current.get('fan_override') == 'MANUAL':
                        mode = 'MANUAL'
                    db.execute('INSERT INTO readings(recorded_at,mode,temperature,humidity,voc_index,nox_index,pm1,pm25,pm10) VALUES(?,?,?,?,?,?,?,?,?)',
                               (stamp,mode if mode in ('AUTO', 'MANUAL') else 'AWAKE_TEST',current['temperature'],current['humidity'],current.get('voc_index'),current.get('nox_index'),current.get('pm1'),current.get('pm25'),current.get('pm10')))
                    self.last_log=now
        current['observed_at'] = stamp
        self.data=current
        if not self.pending_target and state == 'AWAKE' and current.get('mode') == 'AUTO':
            if previous == 'SLEEPING':
                self.command_message = 'Arduino woke and reported AUTO. No website wake was pending.'
            elif previous_mode == 'MAINTENANCE':
                self.command_message = 'Arduino reported a change from MAINTENANCE to AUTO.'
        if (self.pending_target and self.pending_target.startswith('MODE:')
                and current.get('mode') == self.pending_target[5:]
                and current.get('fan_override') != 'MANUAL'):
            self.command_message = current['mode'] + ' confirmed by Arduino.'
            self.pending_target = None
            self.pending_until = 0
        if self.pending_target and self.pending_target.startswith('OLED:'):
            if current.get('oled') == self.pending_target[5:] or current.get('oled') in ('MISSING','DISABLED'):
                self.command_message = 'OLED: ' + current['oled'] + ' (reported).'
                self.pending_target = None
                self.pending_until = 0
        if self.pending_target in ('FAN_ON','FAN_OFF') and current.get('fan_relay')==self.pending_target[4:]:
            self.command_message='Fan relay '+current['fan_relay']+' acknowledged (output command, not RPM).'
            self.pending_target=None
            self.pending_until=0
        if self.pending_target and self.pending_target.startswith('FAN_OVERRIDE:'):
            setting = self.pending_target.split(':', 1)[1]
            if current.get('fan_override') == setting:
                self.command_message = ('Automatic fan control resumed.' if setting == 'AUTO'
                                        else 'Fan override ' + setting + ' confirmed.')
                self.pending_target = None
                self.pending_until = 0
        if self.pending_target and self.pending_target.startswith('FAN_SPEED:'):
            setting = self.pending_target.split(':',1)[1]
            if str(current.get('fan_duty'))==setting:
                self.command_message = 'PWM duty set to '+setting+'%.'
                self.pending_target=None
                self.pending_until=0
        if (current.get('buzzer_ack') and
                self.pending_target == 'BUZZER:' + current['buzzer_ack']):
            self.command_message = 'Arduino acknowledged buzzer command.'
            self.pending_target = None
            self.pending_until = 0
        if self.pending_target == current['state']:
            self.command_message=('Sleep acknowledged. Sensor power is off.' if self.pending_target=='SLEEPING'
                                  else 'Awake acknowledged. Waiting for fresh sensor readings.')
            self.pending_target=None
            self.pending_until=0
        elif self.pending_target=='SLEEPING' and 'Sleep refused:' in current['message']:
            self.command_message='Sleep refused: D2 is LOW. Check the wake circuit.'
            self.pending_target=None
            self.pending_until=0

    def apply_offline(self):
        if self.sleep_id and self.cloud:
            self.queue_cloud_sleep_end(quality='interrupted: connection lost')
        elif self.sleep_id:
            with self.connect() as db:
                db.execute("UPDATE sleep_sessions SET quality='interrupted: connection lost' WHERE id=?",(self.sleep_id,))
            self.sleep_id=None
        self.data=dict(connected=False,state='UNKNOWN',temperature=None,humidity=None,sample_age=None,
                       message='ESP unreachable or invalid response. Check power, Wi-Fi and configured IP.')

    def reset_sd_log(self, key):
        # Serialize with journal imports and other device commands.
        with self.io_lock:
            if self.snapshot()['command_pending']:
                raise ValueError('Wait for the previous command to finish.')
            status = parse_status(self.transport('/status'))
            if status['state'] != 'AWAKE' or not status.get('sd_reset_supported'):
                raise ValueError('Wake the device and install the ESP SD reset update.')
            expected = status.get('sd_log')
            if expected in (None, 'ERROR'):
                raise ValueError('The SD log is unavailable.')
            card, total = map(int, expected.split(','))
            if not 0 < card <= 0xFFFFFFFF or not 0 < total <= 4096:
                raise ValueError('The SD log has no records to reset.')
            with self.connect() as db:
                db.execute('BEGIN')
                cleared = db.execute('SELECT next_slot FROM sd_cleared WHERE sd_card = ?', (card,)).fetchone()
                first = min(total, cleared[0]) if cleared else 0
                synced = db.execute(
                    'SELECT COUNT(*) FROM readings WHERE sd_card = ? AND sd_slot >= ? AND sd_slot < ?',
                    (card, first, total),
                ).fetchone()[0]
                if synced != total - first:
                    raise ValueError('Wait for every SD reading to sync before resetting the log.')
                replacement = card
                while replacement == card or db.execute(
                    'SELECT 1 FROM readings WHERE sd_card = ? UNION ALL '
                    'SELECT 1 FROM sd_cleared WHERE sd_card = ? LIMIT 1',
                    (replacement, replacement),
                ).fetchone():
                    replacement = secrets.randbelow(0xFFFFFFFF) + 1

            # Each request is conditional on the exact old generation and slot count.
            # If a new reading arrives first, the Arduino preserves the journal.
            self.transport(
                f'/sd-reset?expected={expected}&replacement={replacement}', 'POST',
                {'X-Fumetrix-SD-Key': key},
            )
            try:
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    time.sleep(0.2)
                    confirmed = parse_status(self.transport('/status'))
                    journal = confirmed.get('sd_log')
                    if journal and journal.startswith(str(replacement) + ','):
                        return
                    if journal == 'ERROR':
                        raise ValueError('The SD card reported a write or read error. Saved online history was kept.')
                raise ValueError('Reset was not confirmed. Check SD status before retrying.')
            finally:
                self.poll_requested.set()

    def command(self,action,value=None):
        if action not in ('sleep','wake','fan_on','fan_off','fan_speed','fan_override','mode_auto','mode_maintenance','oled_on','oled_off','buzzer','shutdown'):
            raise ValueError('Unsupported command.')
        buzzer_action = action == 'buzzer'
        if buzzer_action and value not in ('test', 'stop', 'birthday', 'jingle', 'twinkle', 'ode'):
            raise ValueError('Choose an available buzzer sound.')
        sleep_action = action in ('sleep', 'shutdown')
        override_action = action == 'fan_override'
        if override_action and value not in ('ON', 'OFF', 'AUTO'):
            raise ValueError('Choose ON, OFF, or resume automatic control.')
        speed_action=action=='fan_speed'
        if speed_action and (type(value) is not int or not 20 <= value <= 100):
            raise ValueError('Select a whole-number duty from 20 to 100.')
        with self.io_lock:
            with self.lock:
                snap=self.snapshot();now=time.monotonic()
                if not snap['connected']:raise ValueError('ESP is offline. No command sent.')
                if snap['command_pending'] or snap['state'] in ('SLEEP_PENDING','WAKING'):
                    raise ValueError('Wait for the previous command to finish.')
                if buzzer_action and (snap['state'] != 'AWAKE' or not snap.get('buzzer_supported')):
                    raise ValueError('Buzzer requires an awake Arduino with the buzzer update.')
                if buzzer_action and value == 'test' and snap['mode'] != 'MAINTENANCE':
                    raise ValueError('Select MAINTENANCE to test the buzzer.')
                if action == 'shutdown' and not snap.get('buzzer_supported'):
                    raise ValueError('Install the buzzer update before using Prepare power off.')
                if sleep_action and snap['state']!='AWAKE':
                    raise ValueError('Arduino must be confirmed awake.')
                if action=='wake' and snap['state']=='AWAKE':
                    raise ValueError('Arduino is already awake.')
                mode_action=action in ('mode_auto','mode_maintenance')
                oled_action=action in ('oled_on','oled_off')
                if mode_action and snap['state'] != 'AWAKE':
                    raise ValueError('Wake the Arduino first, then select MAINTENANCE.')
                if override_action and (snap['state'] != 'AWAKE' or snap.get('mode') != 'AUTO'
                                        or snap.get('fan_override') is None):
                    raise ValueError('Wait for AUTO and fan status before overriding the fan.')
                if override_action and snap.get('fan_override') == 'MANUAL':
                    raise ValueError('The A0 knob controls the fan in Manual.')
                if ((action.startswith('fan_') and not override_action) or oled_action) and snap.get('mode') != 'MAINTENANCE':
                    raise ValueError('Select MAINTENANCE and wait for Arduino confirmation first.')
                fan_action=action in ('fan_on','fan_off')
                if fan_action and (snap['state']!='AWAKE' or not snap.get('fan_supported')):
                    raise ValueError('Fan test requires an awake Arduino and updated ESP firmware.')
                if snap.get('fan_relay')=='PENDING':
                    raise ValueError('Wait for the local fan command acknowledgement.')
                if speed_action and (snap['state']!='AWAKE' or snap.get('fan_duty') is None):
                    raise ValueError('Fan speed requires an awake Arduino and the PWM firmware.')
                self.last_command=now
                self.pending_until=now+6
                self.pending_target=action.upper() if fan_action else ('SLEEPING' if sleep_action else 'AWAKE')
                self.command_message='Sending '+action+' request…'
                if buzzer_action:
                    self.pending_target = 'BUZZER:' + value
                elif mode_action:
                    self.pending_target='MODE:'+('AUTO' if action=='mode_auto' else 'MAINTENANCE')
                elif oled_action:
                    self.pending_target='OLED:'+('ON' if action=='oled_on' else 'OFF')
                elif speed_action:
                    self.pending_target='FAN_SPEED:'+str(value)
                elif override_action:
                    self.pending_target = 'FAN_OVERRIDE:' + value
                elif fan_action:
                    self.data['fan_relay']='PENDING'
                    self.data['fan_rpm']=None
                else:
                    self.data['state']='SLEEP_PENDING' if sleep_action else 'WAKING'
                    self.data['temperature']=self.data['humidity']=None
                    self.data.update(pm1=None,pm25=None,pm10=None,pm_status='WAITING')
                    self.data.update(voc_index=None,nox_index=None,gas_status='WAITING',gas_compensation=None)
            try:
                # Never retry a POST: a timeout can mean it reached the ESP.
                path={'fan_on':'/fan/on','fan_off':'/fan/off'}.get(action,'/'+action)
                if buzzer_action:
                    path = '/buzzer?value=' + value
                elif action == 'shutdown':
                    path = '/sleep?shutdown=1'
                elif mode_action:
                    path='/mode?value='+('AUTO' if action=='mode_auto' else 'MAINTENANCE')
                elif oled_action:
                    path='/oled?value='+('ON' if action=='oled_on' else 'OFF')
                elif speed_action:
                    path='/fan/speed?value='+str(value)
                elif override_action:
                    path = '/fan/override?value=' + value
                result=self.transport(path,'POST')
                with self.lock:
                    self.command_message='Request sent. Waiting for Arduino acknowledgement…'
                return result
            except Exception:
                with self.lock:
                    self.command_message='Delivery uncertain. Checking Arduino status; no command retry sent.'
                raise
            finally:
                self.poll_requested.set()

    def start(self):
        def run():
            while not self.stop.is_set():
                self.poll_requested.clear()
                try:self.update()
                except Exception:pass # Stale snapshots become offline.
                with self.lock:
                    self.expire_pending()
                    pending=self.pending_target is not None or self.data['state'] in ('WAKING','SLEEP_PENDING')
                # Fast confirmation checks only during a transition.
                self.poll_requested.wait(0.2 if pending else 1.0)
        threading.Thread(target=run,name='fumetrix-esp',daemon=True).start()
