"""D1 storage for the Fumetrix web server and one-time local history import."""
import argparse
from contextlib import closing
from getpass import getpass
import json
from pathlib import Path
import re
import sqlite3
import threading
from tempfile import NamedTemporaryFile
from urllib.error import HTTPError
from urllib.request import HTTPRedirectHandler, Request, build_opener


class Unavailable(OSError):
    """An operation could not be confirmed by D1."""


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Row(dict):
    def __getitem__(self, key):
        return tuple(self.values())[key] if isinstance(key, int) else super().__getitem__(key)


class Cursor:
    def __init__(self, connection):
        self.connection = connection
        self.result = None

    def ready(self):
        if self.result is None:
            self.connection.flush()
        return self.result

    def fetchall(self):
        return [Row(row) for row in self.ready().get("results", [])]

    def fetchone(self):
        rows = self.fetchall()
        return rows[0] if rows else None

    def __iter__(self):
        return iter(self.fetchall())

    @property
    def rowcount(self):
        return self.ready()["meta"]["changes"]

    @property
    def lastrowid(self):
        return self.ready()["meta"]["last_row_id"]


class Connection:
    """Batch pending statements when a result is required or the scope commits."""
    def __init__(self, store):
        self.store = store
        self.pending = []

    def __enter__(self):
        self.store.lock.acquire()
        return self

    def __exit__(self, error_type, error, traceback):
        try:
            if error_type is None:
                self.flush()
        finally:
            self.pending.clear()
            self.store.lock.release()

    def execute(self, sql, params=()):
        sql = sql.strip()
        if sql.upper() in ("BEGIN", "BEGIN IMMEDIATE"):
            return None  # The single website process serializes D1 access.
        cursor = Cursor(self)
        if params:
            # The REST API accepts strings. JSON preserves SQL NULL and numeric types.
            positions = iter(range(len(params)))
            sql = re.sub(r"\?", lambda _: f"json_extract(?1, '$[{next(positions)}]')", sql)
            params = [json.dumps(params, ensure_ascii=True, allow_nan=False)]
        self.pending.append(({"sql": sql, "params": params}, cursor))
        return cursor

    def executescript(self, sql):
        for statement in sql.split(";"):
            if statement.strip():
                self.execute(statement)

    def flush(self):
        if not self.pending:
            return
        pending, self.pending = self.pending, []
        for (_, cursor), result in zip(pending, self.store.query([item[0] for item in pending])):
            cursor.result = result


class D1:
    def __init__(self, config):
        account, database, token = (config[key] for key in ("account_id", "database_id", "api_token"))
        if not re.fullmatch(r"[a-fA-F0-9]{32}", account):
            raise ValueError("Expected a 32-character Cloudflare Account ID.")
        if not re.fullmatch(r"[a-fA-F0-9]{8}(?:-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12}", database):
            raise ValueError("Expected a D1 Database ID (UUID).")
        if not re.fullmatch(r"[A-Za-z0-9_-]+", token):
            raise ValueError("Expected a Cloudflare API token.")
        self.url = f"https://api.cloudflare.com/client/v4/accounts/{account}/d1/database/{database}/query"
        self.token = token
        self.lock = threading.RLock()
        self.opener = build_opener(NoRedirect())

    def connect(self):
        return Connection(self)

    def query(self, statements):
        body = statements[0] if len(statements) == 1 else {"batch": statements}
        request = Request(self.url, data=json.dumps(body).encode(), method="POST",
                          headers={"Authorization": "Bearer " + self.token,
                                   "Content-Type": "application/json"})
        try:
            with self.opener.open(request, timeout=4) as response:
                result = json.load(response)
        except HTTPError as error:
            code = error.code
            try:
                details = json.load(error)
            except (OSError, ValueError):
                details = {}
            finally:
                error.close()
            if "UNIQUE constraint failed: users.username" in json.dumps(details):
                raise sqlite3.IntegrityError("Username already exists.") from None
            raise Unavailable(f"D1 returned HTTP {code}. Check connection, IDs, and D1 permission.") from None
        except (OSError, ValueError):
            raise Unavailable("Cloudflare D1 is unreachable.") from None

        if isinstance(result, dict) and not result.get("success"):
            if "UNIQUE constraint failed: users.username" in json.dumps(result.get("errors", [])):
                raise sqlite3.IntegrityError("Username already exists.")
        rows = result.get("result") if isinstance(result, dict) and result.get("success") else None
        if not isinstance(rows, list) or len(rows) != len(statements) or not all(
                isinstance(row, dict) and row.get("success") is True for row in rows):
            raise Unavailable("Cloudflare D1 did not confirm the operation.")
        return rows


def load_store(folder):
    path = folder / "cloud-d1.json"
    if not path.is_file():
        raise SystemExit("Set up D1 first. Run: py -3.11 .\\d1_store.py configure")
    return D1(json.loads(path.read_text(encoding="utf-8")))


def initialize(store):
    with store.connect() as db:
        db.execute("CREATE TABLE IF NOT EXISTS fumetrix_migration(id INTEGER PRIMARY KEY, state TEXT NOT NULL)")
        db.execute("""CREATE TABLE IF NOT EXISTS readings(
            id INTEGER PRIMARY KEY, recorded_at TEXT NOT NULL, mode TEXT NOT NULL,
            temperature REAL, humidity REAL, pm1 REAL, pm25 REAL, pm10 REAL, voc_index REAL,
            nox_index REAL, sd_card INTEGER, sd_slot INTEGER, uptime_s INTEGER, fan_duty INTEGER,
            live_key TEXT UNIQUE)""")
        db.execute("CREATE UNIQUE INDEX IF NOT EXISTS unique_sd_sample ON readings(sd_card, sd_slot)")
        db.execute("CREATE INDEX IF NOT EXISTS readings_by_recorded_at ON readings(recorded_at, id)")
        db.execute("""CREATE TABLE IF NOT EXISTS sleep_sessions(
            id INTEGER PRIMARY KEY, sleep_at TEXT NOT NULL, wake_at TEXT,
            duration_seconds INTEGER, quality TEXT DEFAULT 'legacy', event_key TEXT UNIQUE)""")
        db.execute("CREATE TABLE IF NOT EXISTS sd_cleared(sd_card INTEGER PRIMARY KEY,next_slot INTEGER NOT NULL)")
        db.execute("CREATE TABLE IF NOT EXISTS oled_thresholds(id INTEGER PRIMARY KEY CHECK(id=1),limits TEXT NOT NULL)")
        db.execute("""CREATE TABLE IF NOT EXISTS users(
            id INTEGER PRIMARY KEY, username TEXT UNIQUE NOT NULL, password_hash TEXT NOT NULL,
            role TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 1, version INTEGER NOT NULL DEFAULT 1,
            created_at TEXT NOT NULL, approved INTEGER NOT NULL DEFAULT 1)""")
        db.execute("""CREATE TABLE IF NOT EXISTS audit_log(
            id INTEGER PRIMARY KEY, occurred_at TEXT NOT NULL, actor TEXT NOT NULL,
            action TEXT NOT NULL, target TEXT NOT NULL)""")
        db.execute("""CREATE TABLE IF NOT EXISTS auth_sessions(
            token TEXT PRIMARY KEY, user_id INTEGER NOT NULL, version INTEGER NOT NULL, expires REAL NOT NULL)""")
        db.execute("CREATE TABLE IF NOT EXISTS login_attempts(occurred REAL NOT NULL)")
        db.execute("CREATE TABLE IF NOT EXISTS signup_attempts(occurred REAL NOT NULL)")
    with store.connect() as db:
        row = db.execute("SELECT state FROM fumetrix_migration WHERE id=1").fetchone()
        if row and row[0].startswith("importing:"):
            raise Unavailable("D1 history import is incomplete. Rerun d1_store.py import-history.")
        # Reject the earlier mirror schema, which uses (source_id, local_id).
        db.execute("SELECT id,live_key FROM readings LIMIT 0")
        db.execute("SELECT id,event_key FROM sleep_sessions LIMIT 0").fetchall()


def import_history(store, backup):
    tables = ("readings", "sleep_sessions", "sd_cleared", "oled_thresholds", "users", "audit_log")
    backup = backup.resolve(strict=True)
    with closing(sqlite3.connect(backup.as_uri() + "?mode=ro", uri=True)) as source:
        source_tables = {row[0] for row in source.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        if not source_tables.intersection(tables):
            raise ValueError("The local file does not contain Fumetrix tables.")
        marker = "importing:" + str(backup)
        with store.connect() as db:
            state = db.execute("SELECT state FROM fumetrix_migration WHERE id=1").fetchone()
            if state and state[0] == "complete":
                print("The history import is already complete.")
                return
            if state and state[0] != marker:
                raise ValueError("Resume the import with the original SQLite file.")
            if not state:
                for table in tables:
                    if db.execute(f"SELECT 1 FROM {table} LIMIT 1").fetchone():
                        raise ValueError("Import into a new, empty D1 database before starting the website.")
                db.execute("INSERT INTO fumetrix_migration VALUES(1,?)", (marker,))
        for table in tables:
            if table not in source_tables:
                continue
            known = {
                "readings": "id recorded_at mode temperature humidity pm1 pm25 pm10 voc_index nox_index sd_card sd_slot uptime_s fan_duty",
                "sleep_sessions": "id sleep_at wake_at duration_seconds quality",
                "sd_cleared": "sd_card next_slot", "oled_thresholds": "id limits",
                "users": "id username password_hash role active version created_at approved",
                "audit_log": "id occurred_at actor action target",
            }
            existing = {row[1] for row in source.execute(f"PRAGMA table_info({table})")}
            columns = [name for name in known[table].split() if name in existing]
            names = ", ".join(columns)
            cursor = source.execute(f"SELECT {names} FROM {table}")
            while rows := cursor.fetchmany(20):
                with store.connect() as db:
                    for row in rows:
                        db.execute(f"INSERT INTO {table}({names}) VALUES ({', '.join('?' for _ in columns)}) "
                                   "ON CONFLICT DO NOTHING", row)
        with store.connect() as db:
            db.execute("UPDATE fumetrix_migration SET state='complete' WHERE id=1")
    print("History, settings and accounts imported into D1. Existing users sign in again.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("configure", "import-history"))
    parser.add_argument("--from-file", type=Path)
    args = parser.parse_args()
    folder = Path(__file__).resolve().parent / "data"
    try:
        if args.action == "import-history":
            store = load_store(folder)
            import_history(store, args.from_file or folder / "fumetrix.db")
            return
        config = {
            "account_id": input("Cloudflare Account ID: ").strip(),
            "database_id": input("D1 Database ID: ").strip(),
            "api_token": getpass("Cloudflare D1 API token (hidden): ").strip(),
        }
        initialize(D1(config))
        folder.mkdir(exist_ok=True)
        with NamedTemporaryFile("w", encoding="utf-8", dir=folder, delete=False) as file:
            temporary = Path(file.name)
            json.dump(config, file)
        try:
            temporary.replace(folder / "cloud-d1.json")
        finally:
            temporary.unlink(missing_ok=True)
    except (OSError, ValueError, sqlite3.Error) as error:
        raise SystemExit(str(error)) from None
    print("D1 connection verified. Import existing history before starting the website.")


if __name__ == "__main__":
    main()
