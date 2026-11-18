import unittest
import test_bridge
from test_bridge import AWAKE, SLEEP
AWAKE += "\nMode: MAINTENANCE"
from test_accounts import token,add_user,login
from bridge import parse_status

class FanTests(unittest.TestCase):
    setUp=test_bridge.BridgeTests.setUp
    tearDown=test_bridge.BridgeTests.tearDown
    rows=test_bridge.BridgeTests.rows
    def test_fan_ack_keeps_awake_and_unlocks(self):
        self.reply=AWAKE+'\nFan relay: OFF';self.bridge.update()
        r=self.client.post('/api/command',json={'action':'fan_on'},headers={'X-CSRF-Token':token(self.client)})
        self.assertEqual(r.status_code,202);self.assertEqual(self.posts,['/fan/on'])
        self.assertEqual(self.bridge.snapshot()['state'],'AWAKE')
        self.bridge.update();self.assertTrue(self.bridge.snapshot()['command_pending'])
        self.reply=AWAKE+'\nFan relay: ON';self.bridge.update()
        self.assertFalse(self.bridge.snapshot()['command_pending'])
        self.bridge.command('fan_off');self.reply=AWAKE+'\nFan relay: OFF';self.bridge.update()
        self.assertFalse(self.bridge.snapshot()['command_pending'])
        self.assertEqual(self.posts,['/fan/on','/fan/off'])
    def test_sleep_and_legacy_block_fan(self):
        for reply in [AWAKE,SLEEP+'\nFan relay: OFF']:
            self.reply=reply;self.bridge.update()
            with self.assertRaises(ValueError):self.bridge.command('fan_on')
        self.assertEqual(self.posts,[])
    def test_viewer_cannot_fan(self):
        add_user(self.path,'viewer','viewer');self.client=self.app.test_client();login(self.client,'viewer')
        r=self.client.post('/api/command',json={'action':'fan_on'},headers={'X-CSRF-Token':token(self.client)})
        self.assertEqual(r.status_code,403);self.assertEqual(self.posts,[])
    def test_stale_fan_is_unknown(self):
        self.reply=AWAKE+'\nFan relay: ON';self.bridge.update();self.bridge.seen_at-=7
        self.assertEqual(self.bridge.snapshot()['fan_relay'],'UNKNOWN')
    def test_pending_local_fan_blocks_sleep(self):
        self.reply=AWAKE+'\nFan relay: PENDING';self.bridge.update()
        with self.assertRaises(ValueError):self.bridge.command('sleep')
