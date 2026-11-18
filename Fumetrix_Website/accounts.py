"""Local accounts, revocable sessions, role checks and audit history."""
import re
import secrets
import sqlite3
import time
from datetime import datetime, timezone
from flask import request, session, g, redirect, render_template, jsonify, abort, flash
from werkzeug.security import generate_password_hash, check_password_hash
from flask.sessions import SecureCookieSessionInterface

class CookiePolicy(SecureCookieSessionInterface):
    def get_cookie_secure(self, app):
        # The public site uses HTTPS; loopback HTTP remains usable offline.
        return request.host.split(':')[0] not in ('localhost', '127.0.0.1', '[::1]')

def stamp():
    return datetime.now(timezone.utc).isoformat()

def validate(username, password, role):
    if not re.fullmatch(r'[a-z0-9_.-]{3,32}', username):
        raise ValueError('Username: 3–32 lowercase letters, numbers, dots, underscores or hyphens.')
    if not 12 <= len(password) <= 128:
        raise ValueError('Password must contain 12–128 characters.')
    if role not in ('admin', 'operator', 'viewer'):
        raise ValueError('Choose a valid role.')

def initialize(connect):
    with connect() as db:
        db.executescript('''
        CREATE TABLE IF NOT EXISTS users (
          id INTEGER PRIMARY KEY, username TEXT UNIQUE NOT NULL, password_hash TEXT NOT NULL,
          role TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 1, version INTEGER NOT NULL DEFAULT 1,
          created_at TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS audit_log (
          id INTEGER PRIMARY KEY, occurred_at TEXT NOT NULL, actor TEXT NOT NULL,
          action TEXT NOT NULL, target TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS auth_sessions (
          token TEXT PRIMARY KEY, user_id INTEGER NOT NULL, version INTEGER NOT NULL, expires REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS login_attempts (occurred REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS signup_attempts (occurred REAL NOT NULL);
        ''')
        db.execute('BEGIN IMMEDIATE')
        if 'approved' not in {row[1] for row in db.execute('PRAGMA table_info(users)')}:
            db.execute('ALTER TABLE users ADD COLUMN approved INTEGER NOT NULL DEFAULT 1')

def record(db, actor, action, target=''):
    db.execute('INSERT INTO audit_log(occurred_at,actor,action,target) VALUES(?,?,?,?)',
               (stamp(), actor, action, target))

def install(app, connect, folder, cloud=False):
    if not cloud:
        initialize(connect)
    secret_file=folder/'session-secret.txt'
    try:
        with secret_file.open('x') as file: file.write(secrets.token_hex(32))
    except FileExistsError:
        pass
    app.secret_key=secret_file.read_text().strip()
    app.config.update(SESSION_COOKIE_HTTPONLY=True, SESSION_COOKIE_SAMESITE='Lax',
                      SESSION_COOKIE_NAME='fumetrix_session')
    app.session_interface=CookiePolicy()
    dummy_hash=generate_password_hash(secrets.token_hex(24))

    @app.context_processor
    def context():
        if 'csrf' not in session: session['csrf']=secrets.token_urlsafe(32)
        return dict(current_user=g.get('user'), csrf=session['csrf'])

    @app.before_request
    def protect():
        g.user=None
        if session.get('token'):
            with connect() as db:
                g.user=db.execute('''SELECT u.* FROM users u JOIN auth_sessions s ON s.user_id=u.id
                   WHERE s.token=? AND s.expires>? AND u.active=1 AND u.version=s.version''',
                   (session['token'],time.time())).fetchone()
        if request.endpoint is None or request.endpoint=='static': return
        public = ('login', 'signup')
        if request.endpoint not in public and not g.user:
            if request.path.startswith('/api/'): return jsonify(error='Please sign in.'),401
            return redirect('/login')
        if request.endpoint == 'admin' and (g.user['role'] != 'admin' or not g.user['approved']):
            abort(403)
        if (g.user and not g.user['approved']
                and request.endpoint not in (*public, 'account', 'logout', 'pending')):
            if request.path.startswith('/api/'):
                return jsonify(error='Device access is awaiting admin approval.'), 403
            return redirect('/pending')
        if request.method=='POST':
            token=request.headers.get('X-CSRF-Token') or request.form.get('csrf','')
            if not token or not secrets.compare_digest(token,session.get('csrf','')):
                return (jsonify(error='Session form expired. Refresh the page.'),403) if request.path.startswith('/api/') else ('Form expired. Go back and refresh.',403)
        if (request.endpoint in ('command','sensor_check') or
                (request.endpoint == 'save_thresholds' and request.method == 'POST')) and g.user['role'] not in ('admin','operator'):
            return jsonify(error='Viewer accounts cannot control the device.'),403

    def admin_configured(db):
        return db.execute("SELECT 1 FROM users WHERE active=1 AND approved=1 AND role='admin' LIMIT 1").fetchone() is not None

    @app.route('/signup', methods=['GET', 'POST'])
    def signup():
        if g.user:
            return redirect('/')
        with connect() as db:
            if not admin_configured(db):
                return redirect('/login')
        username = ''
        if request.method == 'POST':
            username = request.form.get('username', '').strip().lower()
            password = request.form.get('password', '')
            try:
                validate(username, password, 'viewer')
                if password != request.form.get('confirm'):
                    raise ValueError('Passwords do not match.')
                with connect() as db:
                    db.execute('BEGIN IMMEDIATE')
                    now = time.time()
                    db.execute('DELETE FROM signup_attempts WHERE occurred < ?', (now - 60,))
                    if db.execute('SELECT count(*) FROM signup_attempts').fetchone()[0] >= 10:
                        return render_template('signup.html', username=username,
                                               error='Too many signup attempts. Wait one minute.'), 429
                    db.execute('INSERT INTO signup_attempts VALUES (?)', (now,))
                password_hash = generate_password_hash(password)
                with connect() as db:
                    db.execute('INSERT INTO users(username,password_hash,role,approved,created_at) '
                               "VALUES (?,?,'viewer',0,?)", (username, password_hash, stamp()))
                    record(db, username, 'signup_requested')
            except ValueError as error:
                return render_template('signup.html', username=username, error=str(error)), 400
            except sqlite3.IntegrityError:
                return render_template('signup.html', username=username, error='That username already exists.'), 409
            flash('Account created. Sign in to check your approval status.')
            return redirect('/login')
        return render_template('signup.html', username=username)

    @app.get('/pending')
    def pending():
        if g.user['approved']:
            return redirect('/')
        return render_template('pending.html')

    @app.route('/login', methods=['GET','POST'])
    def login():
        if g.user: return redirect('/')
        with connect() as db:
            configured=admin_configured(db)
        error=None
        if request.method=='POST':
            username=request.form.get('username','').strip().lower()
            password=request.form.get('password','')
            with connect() as db:
                db.execute('BEGIN IMMEDIATE')
                db.execute('DELETE FROM login_attempts WHERE occurred<?',(time.time()-60,))
                if db.execute('SELECT count(*) FROM login_attempts').fetchone()[0]>=10:
                    return render_template('login.html', error='Too many attempts. Wait one minute.', configured=configured),429
                db.execute('INSERT INTO login_attempts VALUES(?)',(time.time(),))
                user=db.execute('SELECT * FROM users WHERE username=?',(username,)).fetchone()
            valid= len(password)<=128 and check_password_hash(user['password_hash'] if user else dummy_hash,password)
            if user and user['active'] and valid:
                login_token = secrets.token_urlsafe(32)
                with connect() as db:
                    db.execute('BEGIN IMMEDIATE')
                    current = db.execute('SELECT * FROM users WHERE id=?', (user['id'],)).fetchone()
                    if not current or not current['active'] or current['version'] != user['version'] or current['password_hash'] != user['password_hash']:
                        return render_template('login.html', error='Account changed. Please sign in again.',
                                               configured=configured), 403
                    db.execute('DELETE FROM auth_sessions WHERE expires<?',(time.time(),))
                    db.execute('INSERT INTO auth_sessions VALUES(?,?,?,?)',(login_token,user['id'],user['version'],time.time()+8*3600))
                    record(db,username,'login')
                session.clear()
                session['token'] = login_token
                session['csrf'] = secrets.token_urlsafe(32)
                return redirect('/' if current['approved'] else '/pending')
            error='Invalid username or password, or account disabled.'
        return render_template('login.html',error=error,configured=configured)

    @app.post('/logout')
    def logout():
        with connect() as db:
            db.execute('DELETE FROM auth_sessions WHERE token=?',(session.get('token'),))
            record(db,g.user['username'],'logout')
        session.clear()
        return redirect('/login')

    @app.route('/account',methods=['GET','POST'])
    def account():
        if request.method=='POST':
            try:
                password=request.form.get('password','')
                if not check_password_hash(g.user['password_hash'],request.form.get('old_password','')):
                    raise ValueError('Current password is incorrect.')
                validate(g.user['username'],password,g.user['role'])
                if password!=request.form.get('confirm'): raise ValueError('New passwords do not match.')
                password_hash = generate_password_hash(password)
                with connect() as db:
                    changed = db.execute(
                        'UPDATE users SET password_hash=?,version=version+1 WHERE id=? AND active=1 '
                        'AND version=? AND EXISTS (SELECT 1 FROM auth_sessions '
                        'WHERE token=? AND user_id=users.id AND expires>?)',
                        (password_hash, g.user['id'], g.user['version'], session.get('token'), time.time()),
                    )
                    if changed.rowcount:
                        record(db,g.user['username'],'password_changed')
                session.clear()
                return redirect('/login')
            except ValueError as error: flash(str(error))
        return render_template('account.html')

    @app.route('/admin', methods=['GET','POST'])
    def admin():
        if request.method=='POST':
            try:
                action=request.form.get('action')
                with connect() as db:
                    db.execute('BEGIN IMMEDIATE')
                    authorized = db.execute(
                        "SELECT 1 FROM users u JOIN auth_sessions s ON s.user_id=u.id "
                        "WHERE s.token=? AND s.expires>? AND u.active=1 AND u.approved=1 "
                        "AND u.role='admin' AND u.version=s.version",
                        (session.get('token'), time.time()),
                    ).fetchone()
                    if not authorized:
                        abort(403)
                    if action=='create':
                        username=request.form.get('username','').strip().lower()
                        password=request.form.get('password','');role=request.form.get('role')
                        validate(username,password,role)
                        db.execute('INSERT INTO users(username,password_hash,role,created_at) VALUES(?,?,?,?)',
                                   (username,generate_password_hash(password),role,stamp()))
                        record(db,g.user['username'],'user_created',username)
                    elif action in ('update','reset','delete'):
                        user=db.execute('SELECT * FROM users WHERE id=?',(request.form.get('id'),)).fetchone()
                        if not user: raise ValueError('User does not exist.')
                        if action == 'delete':
                            if user['id'] == g.user['id']:
                                raise ValueError('You cannot remove your own admin account.')
                            if request.form.get('confirm_delete') != user['username']:
                                raise ValueError('Type the exact username to confirm removal.')
                            db.execute('DELETE FROM auth_sessions WHERE user_id=?', (user['id'],))
                            db.execute('DELETE FROM users WHERE id=?', (user['id'],))
                        elif action == 'update':
                            role = request.form.get('role')
                            active = int(request.form.get('active') == '1')
                            access = request.form.get('approved', str(user['approved']))
                            if role not in ('admin', 'operator', 'viewer') or access not in ('0', '1'):
                                raise ValueError('Choose a valid role and device access setting.')
                            approved = int(access)
                            if role == 'admin' and not approved:
                                raise ValueError('Administrators require approved device access.')
                            if user['id'] == g.user['id'] and (not active or role != 'admin'):
                                raise ValueError('You cannot disable or demote your own admin account.')
                            if (role, active, approved) != (user['role'], user['active'], user['approved']):
                                db.execute('UPDATE users SET role=?,active=?,approved=?,version=version+1 WHERE id=?',
                                           (role, active, approved, user['id']))
                                if approved != user['approved']:
                                    record(db, g.user['username'],
                                           'device_access_approved' if approved else 'device_access_revoked',
                                           user['username'])
                        else:
                            password=request.form.get('password','');validate(user['username'],password,user['role'])
                            db.execute('UPDATE users SET password_hash=?,version=version+1 WHERE id=?',(generate_password_hash(password),user['id']))
                        record(db,g.user['username'],'user_'+action,user['username'])
                    else: raise ValueError('Invalid operation.')
                flash('Saved. Changed accounts must sign in again.')
            except (ValueError,sqlite3.IntegrityError) as error:
                flash('That username already exists.' if isinstance(error,sqlite3.IntegrityError) else str(error))
            return redirect('/admin')
        with connect() as db:
            users=db.execute('SELECT id,username,role,active,approved,created_at FROM users ORDER BY approved, id').fetchall()
            events=db.execute('SELECT * FROM audit_log ORDER BY id DESC LIMIT 100').fetchall()
            reading_count=db.execute('SELECT count(*) FROM readings').fetchone()[0]
        bridge=app.extensions.get('bridge')
        return render_template('admin.html',users=users,events=events,reading_count=reading_count,
                               device=bridge.snapshot() if bridge else {},database_name='Cloudflare D1' if cloud else 'test database')

    @app.after_request
    def audit_command(response):
        if request.endpoint in ('command','sensor_check') and response.status_code < 500 and g.get('user'):
            payload=request.get_json(silent=True) or {}
            action=payload.get('action','unknown') if isinstance(payload,dict) else 'unknown'
            with connect() as db:
                record(db,g.user['username'],'command_request',str(action)[:20]+' HTTP '+str(response.status_code))
        return response
