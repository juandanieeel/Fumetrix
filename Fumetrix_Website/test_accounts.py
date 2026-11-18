import re
import sqlite3
import tempfile
import time
import unittest
from pathlib import Path
from werkzeug.security import generate_password_hash
from app import create_app

PASSWORD='test-password-12345'
def add_user(path,username='admin',role='admin'):
    with sqlite3.connect(path) as db:
        db.execute('INSERT INTO users(username,password_hash,role,created_at) VALUES(?,?,?,?)',
                   (username,generate_password_hash(PASSWORD),role,'2026-11-09'))
def login(client,username='admin'):
    page=client.get('/login')
    csrf=re.search(r'name="csrf" value="([^"]+)"',page.text).group(1)
    return client.post('/login',data=dict(username=username,password=PASSWORD,csrf=csrf))
def token(client):
    with client.session_transaction() as s:return s['csrf']

class AccountTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)/'test.db'
        self.app=create_app(self.path);self.c=self.app.test_client();add_user(self.path)
    def tearDown(self):self.temp.cleanup()
    def post(self,url,**data):return self.c.post(url,data=dict(csrf=token(self.c),**data))
    def test_anonymous_blocked(self):
        self.assertEqual(self.c.get('/').status_code,302)
        for path in ['/api/status','/api/history','/api/export']:
            self.assertEqual(self.c.get(path).status_code,401)
    def test_login_logout_revokes_token(self):
        self.assertEqual(login(self.c).status_code,302)
        with self.c.session_transaction() as s: old=dict(s)
        self.assertEqual(self.c.get('/admin').status_code,200)
        self.post('/logout')
        with self.c.session_transaction() as s:s.update(old)
        self.assertEqual(self.c.get('/api/status').status_code,401)
    def test_viewer_cannot_admin_or_command(self):
        add_user(self.path,'viewer','viewer');login(self.c,'viewer')
        self.assertEqual(self.c.get('/').status_code,200)
        self.assertEqual(self.c.get('/admin').status_code,403)
        self.assertEqual(self.post('/admin',action='create').status_code,403)
        r=self.c.post('/api/command',json={'action':'sleep'},headers={'X-CSRF-Token':token(self.c)})
        self.assertEqual(r.status_code,403)
    def test_csrf_and_admin_self_protection(self):
        login(self.c)
        self.assertEqual(self.c.post('/admin',data={'action':'create'}).status_code,403)
        self.post('/admin',action='update',id='1',role='viewer',active='0')
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT role,active FROM users WHERE id=1').fetchone(),('admin',1))
    def test_user_lifecycle_and_session_revocation(self):
        login(self.c)
        self.post('/admin',action='create',username='operator',password=PASSWORD,role='operator')
        other=self.app.test_client();login(other,'operator')
        self.assertEqual(other.get('/api/status').status_code,200)
        self.post('/admin',action='update',id='2',role='viewer',active='0')
        self.assertEqual(other.get('/api/status').status_code,401)
        self.post('/admin',action='update',id='2',role='viewer',active='1')
        login(other,'operator');self.assertEqual(other.get('/api/status').status_code,200)
        self.post('/admin',action='reset',id='2',password='different-password-123')
        self.assertEqual(other.get('/api/status').status_code,401)
    def test_change_password_and_existing_data_preserved(self):
        with sqlite3.connect(self.path) as db:db.execute("INSERT INTO readings(recorded_at,mode,temperature) VALUES('2026-11-09','AUTO',22)")
        login(self.c);self.post('/account',old_password=PASSWORD,password='changed-password-123',confirm='changed-password-123')
        self.assertEqual(self.c.get('/api/status').status_code,401)
        create_app(self.path)
        with sqlite3.connect(self.path) as db:self.assertEqual(db.execute('SELECT temperature FROM readings').fetchone()[0],22)
    def test_cookie_flags_and_expiry(self):
        public=self.c.get('/login',base_url='https://myfumetrix.online')
        cookie=public.headers['Set-Cookie']
        for flag in ['Secure','HttpOnly','SameSite=Lax']:self.assertIn(flag,cookie)
        login(self.c)
        with sqlite3.connect(self.path) as db:db.execute('UPDATE auth_sessions SET expires=?',(time.time()-1,))
        self.assertEqual(self.c.get('/api/status').status_code,401)
    def test_login_rate_limit(self):
        self.c.get('/login');csrf=token(self.c)
        for _ in range(10):self.c.post('/login',data={'username':'admin','password':'wrong','csrf':csrf})
        self.assertEqual(self.c.post('/login',data={'username':'admin','password':PASSWORD,'csrf':csrf}).status_code,429)
    def test_password_hash_and_html_escaping(self):
        login(self.c)
        self.post('/admin',action='create',username='<script>',password=PASSWORD,role='admin')
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM users').fetchone()[0],1)
            self.assertNotEqual(db.execute('SELECT password_hash FROM users').fetchone()[0],PASSWORD)
        self.assertEqual(self.c.get('/account').status_code,200)

    def test_signup_approval_and_revocation(self):
        visitor = self.app.test_client()
        self.assertIn('Create an account', visitor.get('/login').text)
        visitor.get('/signup')
        data = dict(username='newuser', password=PASSWORD, confirm=PASSWORD,
                    role='admin', approved='1')
        self.assertEqual(visitor.post('/signup', data=data).status_code, 403)
        data['csrf'] = token(visitor)
        self.assertEqual(visitor.post('/signup', data=data).location, '/login')
        self.assertEqual(visitor.post('/signup', data=data).status_code, 409)
        self.assertEqual(login(visitor, 'newuser').location, '/pending')
        self.assertIn('Waiting for admin approval', visitor.get('/pending').text)
        self.assertEqual(visitor.get('/').location, '/pending')
        self.assertEqual(visitor.get('/account').status_code, 200)
        self.assertEqual(visitor.get('/admin').status_code, 403)
        for path in ('status', 'history', 'export', 'thresholds'):
            self.assertEqual(visitor.get('/api/' + path).status_code, 403)
        for path in ('command', 'sensor-check', 'thresholds'):
            response = visitor.post('/api/' + path, json={}, headers={'X-CSRF-Token': token(visitor)})
            self.assertEqual(response.status_code, 403)
        login(self.c)
        self.assertIn('Pending approval', self.c.get('/admin').text)
        self.post('/admin', action='update', id=2, role='viewer', active='1', approved='1')
        self.assertEqual(visitor.get('/api/status').status_code, 401)
        self.assertEqual(login(visitor, 'newuser').location, '/')
        self.assertEqual(visitor.get('/api/status').status_code, 200)
        self.assertEqual(visitor.post('/api/command', json={}, headers={'X-CSRF-Token': token(visitor)}).status_code, 403)
        self.post('/admin', action='update', id=2, role='operator', active='1', approved='1')
        login(visitor, 'newuser')
        self.assertEqual(visitor.post('/api/command', json={}, headers={'X-CSRF-Token': token(visitor)}).status_code, 503)
        self.post('/admin', action='update', id=2, role='operator', active='1', approved='0')
        self.assertEqual(visitor.get('/api/status').status_code, 401)
        self.assertEqual(login(visitor, 'newuser').location, '/pending')
        self.assertEqual(visitor.get('/api/status').status_code, 403)

    def test_removal_revokes_sessions_and_keeps_audit(self):
        add_user(self.path, 'viewer', 'viewer')
        login(self.c)
        visitor = self.app.test_client()
        login(visitor, 'viewer')
        self.post('/admin', action='delete', id=2, confirm_delete='wrong')
        self.assertEqual(visitor.get('/api/status').status_code, 200)
        self.post('/admin', action='delete', id=2, confirm_delete='viewer')
        self.assertEqual(visitor.get('/api/status').status_code, 401)
        self.post('/admin', action='delete', id=1, confirm_delete='admin')
        with sqlite3.connect(self.path) as db:
            self.assertEqual(db.execute('SELECT username FROM users').fetchall(), [('admin',)])
            self.assertEqual(db.execute("SELECT target FROM audit_log WHERE action='user_delete'").fetchall(),
                             [('viewer',)])

    def test_migration_retains_access_for_existing_accounts(self):
        with sqlite3.connect(self.path) as db:
            db.execute('ALTER TABLE users DROP COLUMN approved')
            db.execute('INSERT INTO users(username,password_hash,role,created_at) VALUES(?,?,?,?)',
                       ('existing', generate_password_hash(PASSWORD), 'operator', '2026-11-09'))
        visitor = create_app(self.path).test_client()
        self.assertEqual(login(visitor, 'existing').location, '/')
        self.assertEqual(visitor.get('/api/status').status_code, 200)
