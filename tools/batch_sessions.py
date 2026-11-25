"""Durable PC-side recording sessions; the ESP32 still captures each 3-second trial."""
from contextlib import closing, contextmanager
import csv
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import hashlib
import io
import json
import math
import queue
from pathlib import Path
import sqlite3
import threading
import time
import uuid
import zipfile

RAW_FIELDS = 'seq ts id raw_x raw_y raw_z raw_m st ref_mm elapsed_ms trial_start_ms valid saturated temp_c quality'.split()
EXPORT_FIELDS = ['session_id', 'distance_cm', 'trial_no', 'trial_name', 'saved_at'] + RAW_FIELDS


class SessionError(Exception):
    def __init__(self, detail, status=400, code='session_error'):
        super().__init__(detail)
        self.status, self.code = status, code


def distance_um(value):
    try:
        if isinstance(value, bool) or not isinstance(value, (str, int, float)) or len(str(value)) > 32:
            raise ValueError()
        cm = Decimal(str(value))
        if not cm.is_finite() or not -50 <= cm <= 50:
            raise ValueError()
        scaled = cm * 10000
        if scaled != scaled.to_integral_value():
            raise ValueError()
        return int(scaled)
    except (ValueError, InvalidOperation, OverflowError):
        raise SessionError('Enter a distance from −50 to +50 cm, with at most 4 decimal places.') from None


def cm_label(um):
    text = format(Decimal(um) / 10000, 'f').rstrip('0').rstrip('.') if um % 10000 else str(um // 10000)
    return text if '.' in text else text + '.0'


def valid_name(value, allow_empty=False):
    if not isinstance(value, str) or len(value.strip()) > 80 or any(ord(c)<32 for c in value) or (not allow_empty and not value.strip()):
        raise SessionError('Name must contain at most 80 printable characters and cannot be blank.')
    return value.strip()


def parse_trial(data, record):
    try:
        reader = csv.DictReader(line for line in data.decode('utf-8').splitlines() if not line.startswith('#'))
        if reader.fieldnames != RAW_FIELDS:
            raise ValueError('Unexpected CSV columns')
        rows = list(reader)
        frames = {}
        healthy = 0
        for row in rows:
            if None in row or any(row[key] is None for key in RAW_FIELDS):
                raise ValueError('Incomplete CSV row')
            seq, sensor, elapsed = int(row['seq']), int(row['id']), int(row['elapsed_ms'])
            int(row['ts'])
            float(row['temp_c'])
            if sensor not in range(4) or not 0 <= elapsed < 3000 or sensor in frames.setdefault(seq, {}):
                raise ValueError('Invalid or duplicate sample')
            if int(row['trial_start_ms']) != record['startedMs'] or not math.isclose(float(row['ref_mm']), record['refMm'], abs_tol=.0001):
                raise ValueError('Trial label or start time does not match')
            if row['valid'] not in ('0', '1') or row['saturated'] not in ('0', '1'):
                raise ValueError('Invalid sensor flags')
            if row['st'] not in ('UNKNOWN', 'OK', 'NOT_FOUND', 'NACK', 'TIMEOUT', 'SATURATED', 'COMM_ERROR', 'DISABLED'):
                raise ValueError('Invalid sensor status')
            if not all(math.isfinite(float(row[k])) for k in ('raw_x', 'raw_y', 'raw_z', 'raw_m', 'quality')) or float(row['raw_m']) < 0:
                raise ValueError('Invalid magnetic values')
            frames[seq][sensor] = elapsed
            healthy += row['valid'] == '1' and row['saturated'] == '0' and row['st'] == 'OK'
        if not frames or len(frames) != record['count'] or any(set(f) != set(range(4)) or len(set(f.values())) != 1 for f in frames.values()):
            raise ValueError('Downloaded frame count does not match the completed trial')
        elapsed = sorted(next(iter(f.values())) for f in frames.values())
        hz = (len(elapsed)-1)*1000/(elapsed[-1]-elapsed[0]) if len(elapsed)>1 and elapsed[-1]>elapsed[0] else 0
        return rows, len(frames), healthy, hz
    except (ValueError, KeyError, TypeError, UnicodeError) as error:
        raise SessionError(f'Trial was not saved: {error}. Retry saving; do not start a new recording.', 409) from error


class SessionRecorder:
    def __init__(self, bridge, path):
        self.bridge, self.path = bridge, Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.worker = None
        self.capture = None
        self.last_board = None
        with self.database() as db:
            db.executescript('''
                CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,name TEXT NOT NULL,created TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS distances(id INTEGER PRIMARY KEY,session_id TEXT NOT NULL REFERENCES sessions(id),um INTEGER NOT NULL,UNIQUE(session_id,um));
                CREATE TABLE IF NOT EXISTS trials(id INTEGER PRIMARY KEY,distance_id INTEGER NOT NULL REFERENCES distances(id),number INTEGER NOT NULL,saved TEXT NOT NULL,csv TEXT NOT NULL,sha TEXT NOT NULL UNIQUE,frames INTEGER NOT NULL,healthy INTEGER NOT NULL,hz REAL NOT NULL,included INTEGER NOT NULL DEFAULT 1,UNIQUE(distance_id,number));
                CREATE TABLE IF NOT EXISTS pending(slot INTEGER PRIMARY KEY CHECK(slot=1),session_id TEXT NOT NULL,distance_id INTEGER NOT NULL,before_ms INTEGER NOT NULL,started_ms INTEGER);
                CREATE TABLE IF NOT EXISTS id_sequences(name TEXT PRIMARY KEY,next_id INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS known_captures(sha TEXT PRIMARY KEY);
            ''')
            if 'name' not in {r['name'] for r in db.execute('PRAGMA table_info(trials)')}:
                db.execute("ALTER TABLE trials ADD COLUMN name TEXT NOT NULL DEFAULT ''")
            if 'next_number' not in {r['name'] for r in db.execute('PRAGMA table_info(distances)')}:
                db.execute('ALTER TABLE distances ADD COLUMN next_number INTEGER NOT NULL DEFAULT 1')
                db.execute('UPDATE distances SET next_number=(SELECT COALESCE(MAX(number),0)+1 FROM trials WHERE distance_id=distances.id)')
            for name, table in [('distance', 'distances'), ('trial', 'trials')]:
                next_id = db.execute(f'SELECT COALESCE(MAX(id),0)+1 FROM {table}').fetchone()[0]
                db.execute('INSERT OR IGNORE INTO id_sequences VALUES(?,?)', (name, next_id))
                db.execute('UPDATE id_sequences SET next_id=MAX(next_id,?) WHERE name=?', (next_id, name))
            db.execute('INSERT OR IGNORE INTO known_captures SELECT sha FROM trials')
            if not db.execute('SELECT 1 FROM sessions').fetchone():
                self._new_session(db, 'Magnet session')
            pending = db.execute('SELECT * FROM pending').fetchone()
            if pending:
                self.capture = dict(pending) | {'state': 'error', 'detail': 'An interrupted capture is waiting. Retry saving from the board.'}
            else:
                for path in sorted(self.path.parent.glob('capture_*.jsonl')):
                    parts = path.stem.split('_')
                    if len(parts)==4 and parts[2].isdigit() and db.execute('SELECT 1 FROM distances WHERE session_id=? AND id=?',(parts[1],int(parts[2]))).fetchone():
                        self.capture={'session_id':parts[1],'distance_id':int(parts[2]),'state':'error','detail':'An interrupted PC capture is retained. Retry saving, or clear a partial attempt.','pc_file':str(path)}
                        break

    @contextmanager
    def database(self):
        with self.lock, closing(sqlite3.connect(self.path)) as db:
            db.row_factory = sqlite3.Row
            db.execute('PRAGMA foreign_keys=ON')
            with db:
                yield db

    def _new_session(self, db, name):
        session = uuid.uuid4().hex
        db.execute('INSERT INTO sessions VALUES(?,?,?)', (session, name, datetime.now(timezone.utc).isoformat()))
        return session

    def create_session(self, name):
        with self.database() as db:
            return self._new_session(db, valid_name(name))

    def _next_id(self, db, name):
        value = db.execute('SELECT next_id FROM id_sequences WHERE name=?', (name,)).fetchone()[0]
        db.execute('UPDATE id_sequences SET next_id=next_id+1 WHERE name=?', (name,))
        return value

    def _editing_allowed(self):
        if self.capture:
            raise SessionError('Finish saving the current trial before renaming or removing saved data.', 409)

    def _session(self, db, session):
        row = db.execute('SELECT * FROM sessions WHERE id=?', (session,)).fetchone()
        if not row:
            raise SessionError('Session not found.', 404)
        return row

    def _distance(self, db, session, distance):
        row = db.execute('SELECT * FROM distances WHERE session_id=? AND id=?', (session, distance)).fetchone()
        if not row:
            raise SessionError('Distance section not found in this session.', 404)
        return row

    def add_distance(self, session, cm):
        um = distance_um(cm)
        with self.database() as db:
            self._session(db, session)
            if not db.execute('SELECT 1 FROM distances WHERE session_id=? AND um=?', (session, um)).fetchone():
                db.execute('INSERT INTO distances(id,session_id,um) VALUES(?,?,?)', (self._next_id(db, 'distance'), session, um))
            return db.execute('SELECT id FROM distances WHERE session_id=? AND um=?', (session, um)).fetchone()[0]

    def snapshot(self, session=None):
        with self.database() as db:
            sessions = [dict(row) for row in db.execute('SELECT * FROM sessions ORDER BY rowid DESC')]
            selected = dict(self._session(db, session or sessions[0]['id']))
            selected['distances'] = []
            for row in db.execute('SELECT * FROM distances WHERE session_id=? ORDER BY um', (selected['id'],)):
                distance = dict(row)
                distance['distanceCm'] = row['um']/10000
                distance['label'] = cm_label(row['um'])
                distance['nextTrial'] = row['next_number']
                distance['trials'] = [dict(t) for t in db.execute('SELECT id,number,name,saved,frames,healthy,hz,included FROM trials WHERE distance_id=? ORDER BY number', (row['id'],))]
                selected['distances'].append(distance)
            return {'sessions': sessions, 'session': selected, 'capture': dict(self.capture) if self.capture else None}

    def include_trial(self, session, trial, included):
        if type(included) is not bool:
            raise SessionError('included must be boolean.')
        with self.database() as db:
            result = db.execute('UPDATE trials SET included=? WHERE id=? AND distance_id IN (SELECT id FROM distances WHERE session_id=?)', (included, trial, session))
            if not result.rowcount:
                raise SessionError('Trial not found in this session.', 404)

    def _trial(self, db, session, trial):
        row = db.execute('SELECT t.*,d.um,d.session_id FROM trials t JOIN distances d ON d.id=t.distance_id WHERE t.id=? AND d.session_id=?', (trial, session)).fetchone()
        if not row:
            raise SessionError('Trial not found in this session.', 404)
        return row

    def rename_session(self, session, name):
        with self.database() as db:
            self._editing_allowed()
            self._session(db, session)
            db.execute('UPDATE sessions SET name=? WHERE id=?', (valid_name(name), session))

    def rename_distance(self, session, distance, cm):
        um = distance_um(cm)
        with self.database() as db:
            self._editing_allowed()
            self._distance(db, session, distance)
            if db.execute('SELECT 1 FROM distances WHERE session_id=? AND um=? AND id<>?', (session, um, distance)).fetchone():
                raise SessionError('That distance already has a section. Select it instead; trials were not merged.', 409)
            db.execute('UPDATE distances SET um=? WHERE id=?', (um, distance))

    def rename_trial(self, session, trial, name):
        with self.database() as db:
            self._editing_allowed()
            self._trial(db, session, trial)
            db.execute('UPDATE trials SET name=? WHERE id=?', (valid_name(name, True), trial))

    def remove_trial(self, session, trial):
        with self.database() as db:
            self._editing_allowed()
            self._trial(db, session, trial)
            db.execute('DELETE FROM trials WHERE id=?', (trial,))

    def remove_distance(self, session, distance):
        with self.database() as db:
            self._editing_allowed()
            self._distance(db, session, distance)
            db.execute('DELETE FROM trials WHERE distance_id=?', (distance,))
            db.execute('DELETE FROM distances WHERE id=?', (distance,))

    def remove_session(self, session):
        with self.database() as db:
            self._editing_allowed()
            self._session(db, session)
            db.execute('DELETE FROM trials WHERE distance_id IN (SELECT id FROM distances WHERE session_id=?)', (session,))
            db.execute('DELETE FROM distances WHERE session_id=?', (session,))
            db.execute('DELETE FROM sessions WHERE id=?', (session,))
            if not db.execute('SELECT 1 FROM sessions').fetchone():
                self._new_session(db, 'Magnet session')

    def trial_data(self, session, trial):
        with self.database() as db:
            row = self._trial(db, session, trial)
            meta = {k: row[k] for k in ('id','number','name','distance_id','included','frames','healthy','hz','saved')}
            meta.update(distanceCm=row['um']/10000, distanceLabel=cm_label(row['um']))
            frames = {}
            reader = csv.DictReader(line for line in row['csv'].splitlines() if not line.startswith('#'))
            for sample in reader:
                seq = int(sample['seq'])
                frame = frames.setdefault(seq, {'seq': seq, 'elapsedMs': int(sample['elapsed_ms']), 's': []})
                frame['s'].append({'id': int(sample['id']), 'rx': float(sample['raw_x']), 'ry': float(sample['raw_y']), 'rz': float(sample['raw_z']), 'rm': float(sample['raw_m']), 'st': sample['st'], 'valid': sample['valid']=='1', 'sat': sample['saturated']=='1'})
            return {'trial': meta, 'frames': sorted(frames.values(), key=lambda f: f['elapsedMs'])}

    def _request(self, path, method='GET', body=None, token=''):
        status, kind, data = self.bridge.request('/api/v1'+path, method, json.dumps(body) if body is not None else '', token, timeout=15)
        if status != 200:
            try:
                error = json.loads(data)
            except ValueError:
                error = {}
            raise SessionError(error.get('detail', 'Device request failed.'), status, error.get('error', 'device_error'))
        return json.loads(data) if kind.split(';')[0] == 'application/json' else data

    def _save(self, session, distance, data, record):
        rows, frames, healthy, hz = parse_trial(data, record)
        digest = hashlib.sha256(data).hexdigest()
        with self.database() as db:
            group = self._distance(db, session, distance)
            if not db.execute('SELECT 1 FROM known_captures WHERE sha=?', (digest,)).fetchone():
                number = group['next_number']
                db.execute('INSERT INTO trials(id,distance_id,number,saved,csv,sha,frames,healthy,hz) VALUES(?,?,?,?,?,?,?,?,?)',
                           (self._next_id(db, 'trial'), distance, number, datetime.now(timezone.utc).isoformat(), data.decode('utf-8'), digest, frames, healthy, hz))
                db.execute('UPDATE distances SET next_number=next_number+1 WHERE id=?', (distance,))
                db.execute('INSERT INTO known_captures VALUES(?)', (digest,))
        self.last_board = (record['startedMs'], record['refMm'], record['count'])

    def _protect_retained(self, session, record):
        if record['active']:
            raise SessionError('A board recording is already active. Wait for it to finish.', 409)
        key = (record['startedMs'], record['refMm'], record['count'])
        if record['count'] and key != self.last_board:
            data = self._request('/export.csv')
            with self.database() as db:
                known = db.execute('SELECT 1 FROM known_captures WHERE sha=?', (hashlib.sha256(data).hexdigest(),)).fetchone()
            if not known:
                distance = self.add_distance(session, str(Decimal(str(record['refMm'])) / 10))
                self._save(session, distance, data, record)
            self.last_board = key

    def start(self, session, distance, token):
        with self.lock:
            if self.capture:
                raise SessionError('Save the current trial before recording another.', 409)
            with self.database() as db:
                group = self._distance(db, session, distance)
            # Reserve before any serial I/O so two browser tabs cannot start together.
            self.capture = {'session_id': session, 'distance_id': distance, 'state': 'starting', 'detail': 'Preparing recording…'}
        attempted = False
        try:
            before = self._request('/measurements')['record']
            self._protect_retained(session, before)
            with self.database() as db:
                db.execute('INSERT INTO pending VALUES(1,?,?,?,NULL)', (session, distance, before['startedMs']))
            attempted = True
            result = self._request('/record', 'POST', {'active': True, 'refMm': group['um']/1000}, token)
            with self.database() as db:
                db.execute('UPDATE pending SET started_ms=? WHERE slot=1', (result['startedMs'],))
                self.capture = dict(db.execute('SELECT * FROM pending').fetchone()) | {'state': 'recording', 'detail': 'Hold the magnet still.'}
            self._launch()
        except SessionError:
            with self.database() as db:
                db.execute('DELETE FROM pending')
                self.capture = None
            raise
        except Exception as error:
            with self.lock:
                if attempted:
                    self.capture.update(state='error', detail=f'Record command outcome is unknown: {error}. Retry saving; the command will not be repeated.')
                else:
                    self.capture = None
            raise SessionError(str(error), 503) from error

    def start_stream(self, session, distance):
        with self.database() as db:
            if self.capture:
                raise SessionError('Save the current trial before recording another.', 409)
            self._distance(db, session, distance)
            self.capture = {'session_id': session, 'distance_id': distance, 'state': 'starting', 'detail': 'Preparing PC stream capture…'}
        self.worker = threading.Thread(target=self._collect_stream, args=(session, distance), daemon=True)
        self.worker.start()

    def _collect_stream(self, session, distance):
        journal = self.path.parent / f'capture_{session}_{distance}_{uuid.uuid4().hex}.jsonl'
        try:
            with self.database() as db:
                group = dict(self._distance(db, session, distance))
            frames = []
            seen = set()
            with self.bridge.events(maxsize=512) as events, journal.open('w', encoding='utf-8') as output:
                baseline = self._request('/sensors')
                if not any(s.get('st') == 'OK' and not s.get('sat') for s in baseline.get('s', [])):
                    raise SessionError('No healthy live sensor data. Check the USB connection.', 409)
                while not events.empty():
                    events.get_nowait()
                started = time.monotonic()
                deadline = started+3
                first_ts = None
                while time.monotonic() < deadline:
                    try:
                        event = events.get(timeout=min(.1, max(.001, deadline-time.monotonic())))
                    except queue.Empty:
                        continue
                    if event.get('t') == 'usb' and event.get('connected') is False:
                        raise SessionError('USB disconnected during PC recording. The partial capture was retained; record a new complete trial.', 409)
                    if event.get('t') != 'sensors':
                        continue
                    seq, ts = event.get('seq'), event.get('ts')
                    if type(seq) is not int or type(ts) is not int:
                        continue
                    delta = (ts-baseline['ts']) & 0xFFFFFFFF
                    if delta == 0 or delta > 10000 or seq in seen:
                        continue
                    first_ts = ts if first_ts is None else first_ts
                    elapsed = (ts-first_ts) & 0xFFFFFFFF
                    if elapsed >= 3000:
                        continue
                    frames.append(event);seen.add(seq)
                    output.write(json.dumps(event)+'\n');output.flush()
                    with self.lock:
                        self.capture.update(state='recording', frames=len(frames), remainingMs=max(0, int((deadline-time.monotonic())*1000)), detail='Recording incoming USB data on this PC.')
            if len(frames)<2 or ((frames[-1]['ts']-frames[0]['ts']) & 0xFFFFFFFF)<2500:
                raise SessionError('The USB stream stalled. Partial data was retained; record a new complete trial.', 409)
            record = {'startedMs': first_ts, 'refMm': group['um']/1000, 'count': len(frames)}
            data = io.StringIO(newline='');writer = csv.writer(data);writer.writerow(RAW_FIELDS)
            for frame in frames:
                samples = {s['id']:s for s in frame['s']}
                for sensor in range(4):
                    s = samples.get(sensor, {})
                    status = s.get('st','NOT_FOUND')
                    writer.writerow([frame['seq'],frame['ts'],sensor,s.get('rx',0),s.get('ry',0),s.get('rz',0),s.get('rm',0),status,record['refMm'],(frame['ts']-first_ts)&0xFFFFFFFF,first_ts,int(status in ('OK','SATURATED')),int(s.get('sat',False)),'nan',s.get('q',0)])
            with self.lock:
                self.capture.update(state='saving',detail='Saving the PC capture…')
            payload = data.getvalue().encode()
            # Keep a complete retry file before committing to SQLite.
            temporary = journal.with_suffix('.tmp')
            temporary.write_text(json.dumps({'record':record,'csv':data.getvalue()}),encoding='utf-8')
            temporary.replace(journal)
            with self.database() as db:
                self._save(session,distance,payload,record)
                self.capture = None
            journal.unlink()
        except Exception as error:
            with self.lock:
                self.capture = {'session_id':session,'distance_id':distance,'state':'error','detail':str(error),'pc_file':str(journal)}

    def _launch(self):
        self.worker = threading.Thread(target=self._collect, daemon=True)
        self.worker.start()

    def retry(self, session):
        with self.lock:
            if not self.capture or self.capture['session_id'] != session or self.capture['state'] != 'error':
                raise SessionError('No failed save in this session.', 409)
            if self.capture.get('pc_file'):
                path = Path(self.capture['pc_file'])
                try:
                    saved = json.loads(path.read_text(encoding='utf-8'))
                    self._save(session,self.capture['distance_id'],saved['csv'].encode(),saved['record'])
                except (ValueError, KeyError):
                    raise SessionError('This is a partial PC capture, not a complete trial. Keep the file and clear the failed attempt before recording again.',409) from None
                self.capture=None;path.unlink();return
            self.capture.update(state='saving', detail='Retrying save without repeating the recording…')
            self._launch()

    def reconcile(self, session):
        with self.lock:
            if not self.capture or self.capture['session_id'] != session or self.capture['state'] != 'error':
                raise SessionError('No failed capture to reconcile.', 409)
            if self.capture.get('pc_file'):
                path=Path(self.capture['pc_file'])
                if path.exists():
                    archive=path.parent/'partial';archive.mkdir(exist_ok=True)
                    path.replace(archive/path.name)
                self.capture=None
                return
            record = self._request('/measurements')['record']
            self._protect_retained(session, record)
            with self.database() as db:
                db.execute('DELETE FROM pending')
                self.capture = None

    def _collect(self):
        try:
            with self.database() as db:
                pending = dict(db.execute('SELECT * FROM pending').fetchone())
                group = self._distance(db, pending['session_id'], pending['distance_id'])
            deadline = time.monotonic()+15
            while True:
                record = self._request('/measurements')['record']
                expected = pending['started_ms']
                if not math.isclose(record['refMm'], group['um']/1000, abs_tol=.0001) or (expected is not None and record['startedMs'] != expected) or (expected is None and record['startedMs'] == pending['before_ms']):
                    raise SessionError('The board no longer holds this trial. Recover available board data before continuing.', 409)
                with self.lock:
                    self.capture.update(state='recording' if record['active'] else 'saving', remainingMs=record.get('remainingMs', 0), frames=record['count'], detail='Hold the magnet still.' if record['active'] else 'Saving raw readings to this computer…')
                if not record['active']:
                    break
                if time.monotonic() > deadline:
                    raise SessionError('Recording did not finish. Retry saving when the board is ready.', 409)
                time.sleep(.2)
            data = self._request('/export.csv')
            with self.database() as db:
                # Data commits first; a surviving recovery marker can be retried idempotently.
                self._save(pending['session_id'], pending['distance_id'], data, record)
                db.execute('DELETE FROM pending')
                self.capture = None
        except Exception as error:
            with self.lock:
                if self.capture:
                    self.capture.update(state='error', detail=str(error))

    def _export_distance(self, db, session, distance):
        group = self._distance(db, session, distance)
        output = io.StringIO(newline='')
        writer = csv.writer(output)
        writer.writerow(EXPORT_FIELDS)
        trials = db.execute('SELECT * FROM trials WHERE distance_id=? AND included=1 ORDER BY number', (distance,)).fetchall()
        if not trials:
            raise SessionError('This distance has no included trials yet.', 409)
        for trial in trials:
            name = trial['name']
            if name.startswith(('=', '+', '-', '@')):
                name = "'"+name
            reader = csv.DictReader(line for line in trial['csv'].splitlines() if not line.startswith('#'))
            for row in reader:
                row['ref_mm'] = format(Decimal(group['um'])/1000, 'f')
                writer.writerow([session, cm_label(group['um']), trial['number'], name, trial['saved']] + [row[k] for k in RAW_FIELDS])
        return output.getvalue().encode('utf-8')

    def export_distance(self, session, distance):
        with self.database() as db:
            return self._export_distance(db, session, distance)

    def export_all(self, session):
        output = io.BytesIO()
        with self.database() as db:
            self._session(db, session)
            groups = db.execute('SELECT DISTINCT d.id,d.um FROM distances d JOIN trials t ON t.distance_id=d.id WHERE d.session_id=? AND t.included=1 ORDER BY d.um', (session,)).fetchall()
            if not groups:
                raise SessionError('Record at least one trial before exporting.', 409)
            with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
                for group in groups:
                    archive.writestr(f"distance_{cm_label(group['um'])}cm.csv", self._export_distance(db, session, group['id']))
        return output.getvalue()
