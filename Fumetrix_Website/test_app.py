import sqlite3
import tempfile
import unittest
from pathlib import Path
from app import create_app
from test_accounts import add_user, login, token

class WebsiteTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.db = Path(self.folder.name) / 'test.db'
        self.client = create_app(self.db).test_client()
        add_user(self.db);login(self.client)

    def tearDown(self):
        self.folder.cleanup()

    def test_empty_database_and_disconnected_device(self):
        state = self.client.get('/api/status').get_json()
        self.assertFalse(state['connected'])
        self.assertEqual(state['state'], 'UNKNOWN')
        self.assertEqual(state['reading_count'], 0)
        self.assertEqual(self.client.get('/api/history').get_json()['rows'], [])

    def test_philippine_date_boundary_and_csv(self):
        with sqlite3.connect(self.db) as db:
            db.executemany('INSERT INTO readings(recorded_at,mode,temperature) VALUES(?,?,?)', [
                ('2026-11-08T15:59:00+00:00','AUTO',20),
                ('2026-11-08T16:00:00+00:00','AUTO',21)])
        query = '?start=2026-11-09&end=2026-11-09'
        rows = self.client.get('/api/history'+query).get_json()['rows']
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['temperature'], 21)
        csv = self.client.get('/api/export'+query)
        self.assertIn('attachment;', csv.headers['Content-Disposition'])
        self.assertIn('2026-11-08T16:00:00+00:00', csv.text)
        self.assertNotIn('2026-11-08T15:59:00+00:00', csv.text)

    def test_invalid_queries_rejected(self):
        for query in ('?kind=invalid', '?start=bad', '?start=2026-11-10&end=2026-11-09'):
            self.assertEqual(self.client.get('/api/history'+query).status_code, 400)
            self.assertEqual(self.client.get('/api/export'+query).status_code, 400)

    def test_sleep_history_and_empty_export(self):
        with sqlite3.connect(self.db) as db:
            db.execute('INSERT INTO sleep_sessions(sleep_at,wake_at,duration_seconds) VALUES(?,?,?)',
                       ('2026-11-09T01:00:00+00:00','2026-11-09T01:01:00+00:00',60))
        self.assertEqual(self.client.get('/api/history?kind=sleep_sessions').get_json()['rows'][0]['duration_seconds'],60)
        self.assertEqual(len(self.client.get('/api/export').text.splitlines()), 1)

    def test_controls_cannot_send_commands(self):
        self.assertEqual(self.client.post('/api/command', json={'command':'SLEEP'}, headers={'X-CSRF-Token':token(self.client)}).status_code, 503)

    def test_page_assets_and_private_files(self):
        for path in ('/', '/static/style.css', '/static/app.js'):
            response = self.client.get(path)
            self.assertEqual(response.status_code, 200)
            self.assertIn('Content-Security-Policy', response.headers)
            response.close()
        for path in ('/data/fumetrix.db','/app.py','/static/../app.py'):
            self.assertEqual(self.client.get(path).status_code,404)

if __name__ == '__main__':
    unittest.main()
