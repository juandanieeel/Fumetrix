"""Fumetrix dashboard, local storage, authenticated controls and ESP bridge."""
import json
from bridge import Bridge
from d1_store import Unavailable, initialize as initialize_d1, load_store
from thresholds import FIELDS, decode, encode, load_limits, validate
from accounts import install
from contextlib import nullcontext
from threading import Lock
from time import monotonic
import csv
import io
import sqlite3
from datetime import date, datetime, timezone
from pathlib import Path
from flask import Flask, g, jsonify, render_template, request, Response

ROOT = Path(__file__).resolve().parent

def create_app(database=None, bridge_config=None, start_bridge=False, cloud=None):
    if database is None and cloud is None:
        cloud = load_store(ROOT / 'data')
    app = Flask(__name__)
    db_path = Path(database) if database else ROOT / 'data' / 'fumetrix.db'
    db_path.parent.mkdir(parents=True, exist_ok=True)

    if cloud is not None:
        connect = cloud.connect
        initialize_d1(cloud)
    else:
        def connect():
            db = sqlite3.connect(db_path)
            db.row_factory = sqlite3.Row
            return db

        with connect() as db:
            db.executescript('''
            CREATE TABLE IF NOT EXISTS oled_thresholds (
              id INTEGER PRIMARY KEY CHECK (id = 1), limits TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS readings (
              id INTEGER PRIMARY KEY, recorded_at TEXT NOT NULL,
              mode TEXT NOT NULL, temperature REAL, humidity REAL,
              pm25 REAL, voc_index REAL, nox_index REAL);
            CREATE TABLE IF NOT EXISTS sleep_sessions (
              id INTEGER PRIMARY KEY, sleep_at TEXT NOT NULL, wake_at TEXT,
              duration_seconds INTEGER);
            ''')

        with connect() as db:
            columns={row[1] for row in db.execute('PRAGMA table_info(sleep_sessions)')}
            if 'quality' not in columns:
                db.execute("ALTER TABLE sleep_sessions ADD COLUMN quality TEXT DEFAULT 'legacy'")
        # Additive migration: historical readings and accounts are retained.
        with connect() as db:
            columns = {row[1] for row in db.execute('PRAGMA table_info(readings)')}
            for column in ('pm1', 'pm10'):
                if column not in columns:
                    db.execute('ALTER TABLE readings ADD COLUMN ' + column + ' REAL')
        with connect() as db:
            columns = {row[1] for row in db.execute('PRAGMA table_info(readings)')}
            for column in ('sd_card', 'sd_slot', 'uptime_s', 'fan_duty'):
                if column not in columns:
                    db.execute('ALTER TABLE readings ADD COLUMN ' + column + ' INTEGER')
            db.execute('CREATE UNIQUE INDEX IF NOT EXISTS unique_sd_sample ON readings(sd_card, sd_slot)')
            db.execute('''CREATE TABLE IF NOT EXISTS sd_cleared (
                sd_card INTEGER PRIMARY KEY, next_slot INTEGER NOT NULL)''')
    bridge = Bridge(bridge_config['esp_ip'], connect, cloud=cloud is not None) if bridge_config else None
    app.extensions['bridge'] = bridge
    app.config['MAX_CONTENT_LENGTH'] = 2048
    if bridge and start_bridge:
        bridge.start()
    install(app, connect, db_path.parent, cloud=cloud is not None)

    @app.errorhandler(Unavailable)
    def database_unavailable(_):
        message = 'Cloud database temporarily unavailable. The device can still write readings to its SD card.'
        return (jsonify(error=message), 503) if request.path.startswith('/api/') else (message, 503)

    @app.after_request
    def headers(response):
        response.headers['Cache-Control'] = 'no-store'
        response.headers['X-Content-Type-Options'] = 'nosniff'
        response.headers['Referrer-Policy'] = 'same-origin'
        response.headers['Content-Security-Policy'] = "default-src 'self'; script-src 'self'; style-src 'self' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; img-src 'self' https://images.unsplash.com; frame-ancestors 'none'; base-uri 'self'; form-action 'self'"
        return response

    @app.get('/')
    def index():
        return render_template('index.html', threshold_fields=FIELDS)

    status_cache_lock = Lock()
    status_cache = None

    def invalidate_status_cache():
        nonlocal status_cache
        with status_cache_lock:
            status_cache = None

    def storage_status(card):
        nonlocal status_cache
        with status_cache_lock:
            if status_cache is not None:
                cached_card, expires, values = status_cache
                if cached_card == card and monotonic() < expires:
                    return values

            with connect() as db:
                count = db.execute('SELECT count(*) FROM readings').fetchone()[0]
                packet = load_limits(db)
                imported = None
                if card is not None:
                    imported = db.execute(
                        '''SELECT MAX(
                            COALESCE((SELECT MAX(sd_slot) + 1 FROM readings WHERE sd_card = ?), 0),
                            COALESCE((SELECT next_slot FROM sd_cleared WHERE sd_card = ?), 0))''',
                        (card, card),
                    ).fetchone()[0]
            values = count, packet, imported
            if cloud is not None:
                status_cache = card, monotonic() + 30, values
            return values

    @app.get('/api/status')
    def status():
        device=bridge.snapshot() if bridge else dict(connected=False,state='UNKNOWN',bridge_ready=False)
        sd_log = device.get('sd_log') if device.get('state') == 'AWAKE' else None
        card, total = None, None
        if sd_log and sd_log != 'ERROR':
            card, total = map(int, sd_log.split(','))
        count, packet, imported = storage_status(card)
        sd_pending = max(0, total - imported) if imported is not None else None
        return jsonify(server_time=datetime.now(timezone.utc).isoformat(),
                       database='ready', reading_count=count, sd_pending=sd_pending,
                       saved_thresholds=decode(packet) if packet else None,
                       threshold_sync=('unset' if packet is None else
                           'applied' if device.get('state') == 'AWAKE' and device.get('oled_limits') == packet
                           else 'pending'),
                       online_database='D1' if cloud is not None else 'test database', log_interval_seconds=180, **device)

    @app.route('/api/thresholds', methods=['GET', 'POST'])
    def save_thresholds():
        if request.method == 'GET':
            with connect() as db:
                packet = load_limits(db)
            return jsonify(values=decode(packet) if packet else None)
        try:
            packet = encode(validate(request.get_json(silent=True)))
        except ValueError as error:
            return jsonify(error=str(error)), 400
        device = bridge.snapshot() if bridge else {}
        if device.get('state') != 'AWAKE' or device.get('mode') != 'MAINTENANCE' or device.get('command_pending'):
            return jsonify(error='Select Maintenance on the awake device before saving.'), 409
        with connect() as db:
            db.execute(
                'INSERT INTO oled_thresholds(id, limits) VALUES (1, ?) '
                'ON CONFLICT(id) DO UPDATE SET limits=excluded.limits', (packet,)
            )
        invalidate_status_cache()
        bridge.next_limits_read = 0
        bridge.poll_requested.set()
        return jsonify(message='Thresholds saved.'), 202

    def history(paged=False, recent=False, include_sd_elapsed=False):
        kind = request.args.get('kind', 'readings')
        if kind not in ('readings', 'sleep_sessions'):
            raise ValueError('Unknown history type.')
        conditions, values = [], []
        column = 'recorded_at' if kind == 'readings' else 'sleep_at'
        # Dates are Philippine calendar dates. Persisted timestamps must be UTC ISO 8601.
        start, end = request.args.get('start', ''), request.args.get('end', '')
        for name, value, operator in [('start', start, '>='), ('end', end, '<=')]:
            if value:
                date.fromisoformat(value)
                conditions.append(f"date({column}, '+8 hours') {operator} ?")
                values.append(value)
        if start and end and start > end:
            raise ValueError('Start date must be on or before end date.')
        where = ' WHERE ' + ' AND '.join(conditions) if conditions else ''
        page_size = 25 if paged else 90 if recent else 1000
        requested = max(1, int(request.args.get('page', 1))) if paged else 1
        fields = 'r.*'
        timeline = ''
        join = ''
        if kind == 'readings' and (paged or include_sd_elapsed):
            # Measure from the first SD sample in each run. A backwards uptime
            # marks an observable reset. Use the saved ticks, including gaps.
            timeline = """
                WITH sd_order AS (
                    SELECT id, sd_card, sd_slot, uptime_s,
                           LAG(uptime_s) OVER (PARTITION BY sd_card ORDER BY sd_slot) AS prior
                    FROM readings WHERE sd_card IS NOT NULL AND uptime_s IS NOT NULL
                ), sd_runs AS (
                    SELECT id, sd_card, uptime_s,
                           MAX(CASE WHEN prior IS NULL OR uptime_s < prior THEN sd_slot END)
                           OVER (PARTITION BY sd_card ORDER BY sd_slot ROWS UNBOUNDED PRECEDING) AS first_slot
                    FROM sd_order
                )
            """
            join = (' LEFT JOIN sd_runs AS run ON run.id = r.id'
                    ' LEFT JOIN readings AS origin ON origin.sd_card = run.sd_card AND origin.sd_slot = run.first_slot')
            fields += ', run.uptime_s - origin.uptime_s AS elapsed_s'
            where = where.replace(f'date({column},', f'date(r.{column},')
        with connect() as db:
            if paged:
                db.execute('BEGIN')
                total = db.execute(f'SELECT COUNT(*) FROM {kind} AS r{where}', values).fetchone()[0]
                pages = max(1, (total + page_size - 1) // page_size)
                page = min(requested, pages)
            else:
                total, pages, page = None, 1, 1
            rows = db.execute(
                f'{timeline} SELECT {fields} FROM {kind} AS r{join}{where}'
                f' ORDER BY r.{column} DESC, r.id DESC LIMIT ? OFFSET ?',
                [*values, page_size, (page - 1) * page_size],
            ).fetchall()
        records = [dict(row) for row in rows]
        if include_sd_elapsed and kind == 'readings':
            for row in records:
                elapsed = row.get('elapsed_s')
                if elapsed is None:
                    row['sd_elapsed'] = None
                else:
                    hours, minutes = divmod(elapsed // 60, 60)
                    row['sd_elapsed'] = f'{hours:02d}:{minutes:02d}'
        return kind, records, dict(page=page, pages=pages, total=total, limit=page_size)

    @app.get('/api/history')
    def get_history():
        try:
            paged = 'page' in request.args
            kind, rows, meta = history(paged=paged, recent=not paged and request.args.get('recent') == '1')
            return jsonify(kind=kind, rows=rows, **meta)
        except ValueError as error:
            return jsonify(error=str(error)), 400

    @app.post('/api/sd/reset')
    def reset_sd():
        if g.user['role'] != 'admin':
            return jsonify(error='Only an administrator can reset the SD log.'), 403
        if not bridge:
            return jsonify(error='Device bridge is not configured.'), 503
        try:
            key = (db_path.parent / 'sd-reset-key.txt').read_text(encoding='ascii').strip()
        except (OSError, UnicodeError):
            return jsonify(error='Install the website SD reset update and its device key.'), 503
        if len(key) != 64 or any(char not in '0123456789abcdef' for char in key):
            return jsonify(error='The website SD reset key is invalid.'), 503
        try:
            bridge.reset_sd_log(key)
        except ValueError as error:
            return jsonify(error=str(error)), 409
        except OSError:
            return jsonify(error='Unable to confirm the SD reset. Refresh status before retrying.'), 502
        return jsonify(message='SD log reset. Saved online records were kept.')

    @app.post('/api/history/clear')
    def clear_history():
        if g.user['role'] != 'admin':
            return jsonify(error='Only an administrator can clear history.'), 403
        payload = request.get_json(silent=True)
        kind = payload.get('kind') if isinstance(payload, dict) else None
        if kind not in ('readings', 'sleep_sessions'):
            return jsonify(error='Unknown history type.'), 400
        lock = bridge.io_lock if bridge and cloud else bridge.lock if bridge and kind == 'sleep_sessions' else nullcontext()
        with lock:
            with connect() as db:
                db.execute('BEGIN IMMEDIATE')
                if kind == 'readings':
                    db.execute("""
                        INSERT INTO sd_cleared(sd_card, next_slot)
                        SELECT sd_card, MAX(sd_slot) + 1 FROM readings
                        WHERE sd_card IS NOT NULL AND sd_slot IS NOT NULL GROUP BY sd_card
                        ON CONFLICT(sd_card) DO UPDATE SET next_slot =
                            MAX(sd_cleared.next_slot, excluded.next_slot)
                    """)
                deleted = db.execute(f'DELETE FROM {kind}').rowcount
            if bridge and cloud:
                if kind == 'sleep_sessions':
                    bridge.cloud_sleep.clear()
                else:
                    bridge.cloud_readings.clear()
            if bridge and kind == 'sleep_sessions':
                bridge.sleep_id = None
        if kind == 'readings':
            invalidate_status_cache()
        return jsonify(deleted=deleted)

    @app.get('/api/export/pdf')
    def export_pdf():
        from history_pdf import render_history_pdf

        try:
            kind, rows, meta = history(include_sd_elapsed=True)
        except ValueError as error:
            return jsonify(error=str(error)), 400
        pdf = render_history_pdf(
            kind, rows, request.args.get('start', ''), request.args.get('end', ''), meta['limit']
        )
        return Response(pdf, mimetype='application/pdf', headers={
            'Content-Disposition': f'attachment; filename="fumetrix-{kind}.pdf"'})

    @app.get('/api/export')
    def export():
        try:
            kind, rows, _ = history(include_sd_elapsed=True)
        except ValueError as error:
            return jsonify(error=str(error)), 400
        fields = (['id', 'recorded_at', 'sd_elapsed', 'mode', 'temperature', 'humidity', 'pm1', 'pm25', 'pm10', 'voc_index', 'nox_index', 'sd_card', 'sd_slot', 'uptime_s', 'fan_duty']
                  if kind == 'readings' else ['id', 'sleep_at', 'wake_at', 'duration_seconds', 'quality'])
        output = io.StringIO(newline='')
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            # Prevent spreadsheet formula interpretation if future inputs contain text.
            writer.writerow({key: ("'" + value if isinstance(value, str) and value.startswith(('=', '+', '-', '@')) else value) for key, value in row.items()})
        return Response(output.getvalue(), mimetype='text/csv', headers={
            'Content-Disposition': f'attachment; filename="fumetrix-{kind}.csv"'})

    @app.post('/api/sensor-check')
    def sensor_check():
        if not bridge:
            return jsonify(error='Device bridge is not configured.'), 503
        before = bridge.snapshot()
        if not before['connected'] or before.get('mode') != 'MAINTENANCE' or before.get('command_pending'):
            return jsonify(error='Select MAINTENANCE and wait for confirmation first.'), 409
        bridge.update() # Request the ESP's latest valid measurements; do not restart gas conditioning.
        result = bridge.snapshot()
        if not result['connected'] or result.get('mode') != 'MAINTENANCE':
            return jsonify(error='Device unavailable or maintenance mode ended. Try again.'), 409
        return jsonify(device=result, checked_at=result.get('observed_at'))

    @app.post('/api/command')
    def command():
        if not bridge:
            return jsonify(error='Device bridge is not configured. No command sent.'), 503
        if not request.is_json:
            return jsonify(error='JSON body required.'), 415
        payload=request.get_json(silent=True)
        if not isinstance(payload,dict):
            return jsonify(error='Invalid request.'), 400
        action=payload.get('action')
        if action not in ('sleep','wake','fan_on','fan_off','fan_speed','fan_override','mode_auto','mode_maintenance','oled_on','oled_off','buzzer','shutdown'):
            return jsonify(error='Unsupported device command.'), 400
        try:
            bridge.command(action,payload.get('value'))
            return jsonify(message='Request sent. Waiting for Arduino acknowledgement.', device=bridge.snapshot()), 202
        except ValueError as error:
            return jsonify(error=str(error)), 409
        except Exception:
            return jsonify(error='Command delivery uncertain. Check device status before trying again.'), 502

    return app

if __name__ == '__main__':
    from waitress import serve
    print('Fumetrix: http://127.0.0.1:5500', flush=True)
    print('Keep this window open. Press Ctrl+C to stop.', flush=True)
    config_path=ROOT/'data'/'bridge-config.json'
    config=json.loads(config_path.read_text()) if config_path.exists() else None
    if not config: print('Run configure.py to enable the ESP bridge.',flush=True)
    store = load_store(ROOT/'data')
    from setup_admin import ensure_admin
    try:
        application = create_app(bridge_config=config, start_bridge=True, cloud=store)
        ensure_admin(ROOT/'data'/'fumetrix.db', cloud=store)
    except Unavailable as error:
        raise SystemExit(str(error)) from None
    serve(application, host='127.0.0.1', port=5500, threads=4)
