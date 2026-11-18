import time
import unittest
import test_bridge
from test_bridge import AWAKE, SLEEP
from test_accounts import add_user,login,token

AUTO=AWAKE+'\nMode: AUTO\nOLED: ON\nFan relay: OFF\nFan speed: WEB,60'
MAINT=AUTO.replace('Mode: AUTO','Mode: MAINTENANCE')

class MaintenanceTests(unittest.TestCase):
    setUp=test_bridge.BridgeTests.setUp
    tearDown=test_bridge.BridgeTests.tearDown
    rows=test_bridge.BridgeTests.rows
    def test_mode_requires_ack_and_keeps_awake(self):
        self.reply=AUTO;self.bridge.update();self.bridge.command('mode_maintenance')
        self.assertEqual(self.posts,['/mode?value=MAINTENANCE'])
        self.assertEqual(self.bridge.snapshot()['state'],'AWAKE')
        self.bridge.update();self.assertTrue(self.bridge.snapshot()['command_pending'])
        self.reply=MAINT;self.bridge.update()
        self.assertFalse(self.bridge.snapshot()['command_pending'])
        self.assertEqual(self.bridge.snapshot()['mode'],'MAINTENANCE')
        self.bridge.command('mode_auto');self.reply=AUTO;self.bridge.update()
        self.assertFalse(self.bridge.snapshot()['command_pending'])
    def test_manual_controls_blocked_in_auto_and_sleep(self):
        for reply in [AUTO,SLEEP+'\nMode: SLEEP']:
            self.reply=reply;self.bridge.update()
            for action in ['fan_on','fan_off','fan_speed','oled_on','oled_off']:
                with self.assertRaises(ValueError):self.bridge.command(action,50)
        self.assertEqual(self.posts,[])
    def test_oled_ack(self):
        self.reply=MAINT;self.bridge.update();self.bridge.command('oled_off')
        self.bridge.update();self.assertTrue(self.bridge.snapshot()['command_pending'])
        self.reply=MAINT.replace('OLED: ON','OLED: OFF');self.bridge.update()
        self.assertFalse(self.bridge.snapshot()['command_pending'])
        self.assertEqual(self.posts,['/oled?value=OFF'])
    def test_maintenance_no_periodic_logs_and_requested_check(self):
        self.reply=MAINT;self.bridge.update();self.assertEqual(self.rows('readings'),[])
        r=self.client.post('/api/sensor-check',headers={'X-CSRF-Token':token(self.client)})
        self.assertEqual(r.status_code,200)
        self.assertEqual(r.json['device']['temperature'],22.56)
        self.assertTrue(r.json['checked_at'])
        self.assertEqual(self.rows('readings'),[])
        self.reply=AUTO;self.bridge.update();self.assertEqual(len(self.rows('readings')),1)
    def test_viewer_and_csrf_checks(self):
        self.reply=MAINT;self.bridge.update()
        self.assertEqual(self.client.post('/api/sensor-check').status_code,403)
        add_user(self.path,'viewer','viewer');self.client=self.app.test_client();login(self.client,'viewer')
        for path,payload in [('/api/sensor-check',{}),('/api/command',{'action':'mode_maintenance'})]:
            r=self.client.post(path,json=payload,headers={'X-CSRF-Token':token(self.client)})
            self.assertEqual(r.status_code,403)
    def test_status_json_and_stale_mode(self):
        self.reply=MAINT;self.bridge.update()
        r=self.client.get('/api/status');self.assertEqual(r.status_code,200)
        self.assertEqual(r.json['mode'],'MAINTENANCE')
        self.bridge.seen_at-=7;self.assertEqual(self.bridge.snapshot()['mode'],'UNKNOWN')
    def test_check_failure_does_not_return_old_sample(self):
        self.reply=MAINT;self.bridge.update();self.reply=TimeoutError()
        r=self.client.post('/api/sensor-check',headers={'X-CSRF-Token':token(self.client)})
        self.assertEqual(r.status_code,409);self.assertNotIn('device',r.json)
    def test_mode_timeout_does_not_claim_success(self):
        self.reply=AUTO;self.bridge.update();self.bridge.command('mode_maintenance')
        self.bridge.pending_until=time.monotonic()-1
        self.assertEqual(self.bridge.snapshot()['mode'],'AUTO')
        self.assertFalse(self.bridge.snapshot()['command_pending'])
