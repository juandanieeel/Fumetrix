import sqlite3
import tempfile
import time
import unittest
import threading
from pathlib import Path
from werkzeug.security import generate_password_hash
from app import create_app
from bridge import parse_status
from test_accounts import add_user, login, token

AWAKE='Arduino: AWAKE\nLast reported D2: HIGH\nTemperature: 22.56 C\nHumidity: 56.56 %\nReading age: 0 s'
SLEEP='Arduino: SLEEP preparation acknowledged\nTemperature: --\nHumidity: --\nSensor power is OFF.'

class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.path=Path(self.tmp.name)/'test.db'
        self.app=create_app(self.path,bridge_config=dict(esp_ip='192.168.100.206',password_hash=generate_password_hash('test-only-password')))
        self.client=self.app.test_client();self.bridge=self.app.extensions['bridge']
        add_user(self.path);login(self.client)
        self.reply=AWAKE;self.posts=[]
        def transport(path,method='GET'):
            if method=='POST':self.posts.append(path);return 'Command sent.'
            if isinstance(self.reply,Exception):raise self.reply
            return self.reply
        self.bridge.transport=transport
    def tearDown(self):self.tmp.cleanup()
    def rows(self,table):
        with sqlite3.connect(self.path) as db:return db.execute('SELECT * FROM '+table).fetchall()
    def test_fresh_stale_missing_and_invalid(self):
        self.assertEqual(parse_status(AWAKE)['temperature'],22.56)
        self.assertIsNone(parse_status(AWAKE.replace('0 s','9 s'))['temperature'])
        self.assertIsNone(parse_status(SLEEP)['temperature'])
        with self.assertRaises(ValueError):parse_status('<html>Error</html>')
    def test_logging_interval_and_offline(self):
        self.bridge.update();self.bridge.update()
        self.assertEqual(len(self.rows('readings')),1)
        self.bridge.last_log-=181;self.bridge.update()
        self.assertEqual(len(self.rows('readings')),2)
        self.reply=TimeoutError();self.bridge.update()
        snap=self.bridge.snapshot()
        self.assertFalse(snap['connected']);self.assertIsNone(snap['temperature'])
    def test_sleep_records_only_continuous_observations(self):
        self.bridge.update();self.reply=SLEEP;self.bridge.update()
        self.bridge.sleep_mono-=10
        self.reply=AWAKE;self.bridge.update()
        row=self.rows('sleep_sessions')[0]
        self.assertGreaterEqual(row[3],10);self.assertEqual(row[4],'observed')
        self.reply=SLEEP;self.bridge.update();self.reply=TimeoutError();self.bridge.update()
        self.reply=AWAKE;self.bridge.update()
        row=self.rows('sleep_sessions')[1]
        self.assertIsNone(row[2]);self.assertIsNone(row[3]);self.assertIn('interrupted',row[4])
    def test_first_observation_asleep_has_no_invented_start(self):
        self.reply=SLEEP;self.bridge.update();self.assertEqual(self.rows('sleep_sessions'),[])
    def test_password_and_unsupported_commands(self):
        self.bridge.update()
        r=self.client.post('/api/command',json={'action':'sleep'})
        self.assertEqual(r.status_code,403);self.assertEqual(self.posts,[])
        r=self.client.post('/api/command',json={'action':'fan','password':'test-only-password'},headers={'X-CSRF-Token':token(self.client)})
        self.assertEqual(r.status_code,400)
        r=self.client.post('/api/command',json={'action':'sleep','password':'test-only-password'},headers={'X-CSRF-Token':token(self.client)})
        self.assertEqual(r.status_code,202);self.assertEqual(self.posts,['/sleep'])
        self.assertEqual(self.bridge.snapshot()['state'],'SLEEP_PENDING')
    def test_timeout_never_retries_command(self):
        self.bridge.update()
        def fail(path,method='GET'):
            self.posts.append(path);raise TimeoutError()
        self.bridge.transport=fail
        response=self.client.post('/api/command',json={'action':'sleep','password':'test-only-password'},headers={'X-CSRF-Token':token(self.client)})
        self.assertEqual(response.status_code,502);self.assertEqual(self.posts,['/sleep'])
        self.assertTrue(self.bridge.snapshot()['command_pending'])
    def test_private_config_not_served(self):
        self.assertEqual(self.client.get('/data/bridge-config.json').status_code,404)

    def test_acknowledgement_unlocks_without_six_second_wait(self):
        self.bridge.update()
        self.bridge.command('sleep')
        self.assertTrue(self.bridge.snapshot()['command_pending'])
        self.reply=SLEEP;self.bridge.update()
        self.assertFalse(self.bridge.snapshot()['command_pending'])
        # Wake is accepted immediately after acknowledgement, even <2s later.
        self.bridge.command('wake')
        self.reply=AWAKE;self.bridge.update()
        snap=self.bridge.snapshot()
        self.assertFalse(snap['command_pending'])
        self.assertIn('Awake acknowledged',snap['command_message'])
        self.assertEqual(self.posts,['/sleep','/wake'])

    def test_wrong_state_does_not_confirm_and_timeout_does_not_retry(self):
        self.bridge.update();self.bridge.command('sleep')
        self.bridge.update() # Old AWAKE response is not confirmation of sleep.
        self.assertTrue(self.bridge.snapshot()['command_pending'])
        with self.assertRaises(ValueError):self.bridge.command('sleep')
        self.bridge.pending_until=time.monotonic()-1
        snap=self.bridge.snapshot()
        self.assertFalse(snap['command_pending'])
        self.assertIn('timed out',snap['command_message'])
        self.assertEqual(self.posts,['/sleep'])

    def test_refused_sleep_is_not_reported_as_success(self):
        self.bridge.update();self.bridge.command('sleep')
        self.reply=AWAKE+'\nSleep refused: D2 is LOW.'
        self.bridge.update();snap=self.bridge.snapshot()
        self.assertFalse(snap['command_pending'])
        self.assertIn('Sleep refused',snap['command_message'])
        self.assertEqual(self.rows('sleep_sessions'),[])

    def test_snapshot_not_blocked_by_network_io(self):
        self.bridge.update()
        entered=threading.Event();release=threading.Event();read=threading.Event()
        def slow(path,method='GET'):
            entered.set();release.wait(2);return AWAKE
        self.bridge.transport=slow
        worker=threading.Thread(target=self.bridge.update);worker.start()
        try:
            self.assertTrue(entered.wait(1))
            reader=threading.Thread(target=lambda:(self.bridge.snapshot(),read.set()))
            reader.start()
            self.assertTrue(read.wait(.5),'Snapshot waited for ESP network I/O')
        finally:
            release.set();worker.join();reader.join()

    def test_command_response_exposes_pending_not_confirmed(self):
        self.bridge.update()
        response=self.client.post('/api/command',json={'action':'sleep'},headers={'X-CSRF-Token':token(self.client)})
        self.assertEqual(response.status_code,202)
        data=response.get_json()['device']
        self.assertEqual(data['state'],'SLEEP_PENDING')
        self.assertTrue(data['command_pending'])
        self.assertEqual(data['command_target'],'SLEEPING')

if __name__=='__main__':unittest.main()
