"""Local console bootstrap and administrator recovery."""
import getpass
import sqlite3
from pathlib import Path
from d1_store import initialize as initialize_d1, load_store
from werkzeug.security import generate_password_hash
from accounts import initialize, validate, stamp, record

def ensure_admin(path, recovery=False, cloud=None):
    if cloud is not None:
        connect = cloud.connect
    else:
        def connect(): return sqlite3.connect(path)
        initialize(connect)
    with connect() as db:
        exists=db.execute("SELECT count(*) FROM users WHERE active=1 AND approved=1 AND role='admin'").fetchone()[0]
    if exists and not recovery: return
    print('Fumetrix administrator setup. Password input is hidden.')
    while True:
        username=input('Admin username (lowercase): ').strip().lower()
        password=getpass.getpass('Password (12–128 characters): ')
        confirm=getpass.getpass('Confirm password: ')
        try:
            validate(username,password,'admin')
            if password!=confirm: raise ValueError('Passwords do not match.')
            with connect() as db:
                existing=db.execute('SELECT id FROM users WHERE username=?',(username,)).fetchone()
                if existing:
                    if not recovery: raise ValueError('Username exists. Choose another.')
                    if input('Reset this account as an active admin? Type RESET: ')!='RESET': return
                    db.execute("UPDATE users SET password_hash=?,role='admin',active=1,approved=1,version=version+1 WHERE id=?",(generate_password_hash(password),existing[0]))
                else:
                    db.execute("INSERT INTO users(username,password_hash,role,created_at) VALUES(?,?,'admin',?)",(username,generate_password_hash(password),stamp()))
                record(db,'local-console','admin_setup',username)
            print('Admin saved. Start the website and sign in with this account.')
            return
        except ValueError as error: print(error)

if __name__=='__main__':
    path=Path(__file__).resolve().parent/'data'/'fumetrix.db'
    path.parent.mkdir(parents=True,exist_ok=True)
    store = load_store(path.parent)
    initialize_d1(store)
    ensure_admin(path, recovery=True, cloud=store)
