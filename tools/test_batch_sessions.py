import csv
import io
import http.client
import json
from pathlib import Path
import tempfile
import sqlite3
import threading
import unittest
import zipfile
import time
import queue
from contextlib import contextmanager

from batch_sessions import SessionRecorder, SessionError
from usb_dashboard import make_server


FIELDS = 'seq,ts,id,raw_x,raw_y,raw_z,raw_m,st,ref_mm,elapsed_ms,trial_start_ms,valid,saturated,temp_c,quality'


class Board:
    def __init__(self):
        self.started = 0
        self.ref = 0
        self.fail_export = False
        self.gate = threading.Event()
        self.gate.set()
        self.posts = 0
        self.corrupt_csv = False
        self.hold_after_start = False
        self.paths = []
        self.stream_seq = 0
        self.stream_ts = 0

    @contextmanager
    def events(self, maxsize=32):
        events=queue.Queue(maxsize=maxsize);stop=threading.Event()
        epoch=time.monotonic()
        def produce():
            while not stop.wait(.01):
                self.stream_seq+=1
                self.stream_ts=int((time.monotonic()-epoch)*1000)
                events.put({'t':'sensors','seq':self.stream_seq,'ts':self.stream_ts,'s':[{'id':i,'rx':3,'ry':-12,'rz':4,'rm':13,'st':'OK','sat':False,'q':1} for i in range(4)]})
        thread=threading.Thread(target=produce,daemon=True);thread.start()
        try:yield events
        finally:stop.set();thread.join(1)

    def request(self, path, method='GET', body='', token='', **kwargs):
        self.paths.append(path)
        if path.endswith('/sensors'):
            data={'seq':self.stream_seq,'ts':self.stream_ts,'s':[{'st':'OK','sat':False}]}
        elif path.endswith('/record'):
            if token != 'valid':
                return 401, 'application/json', b'{"error":"unauthorized","detail":"bad token"}'
            self.ref = json.loads(body)['refMm']
            self.started += 100
            self.posts += 1
            if self.hold_after_start:
                self.gate.clear()
            data = {'active': True, 'startedMs': self.started}
        elif path.endswith('/measurements'):
            self.gate.wait(2)
            data = {'record': {'active': False, 'count': 2 if self.started else 0,
                              'refMm': self.ref, 'startedMs': self.started, 'remainingMs': 0}}
        elif path.endswith('/export.csv'):
            if self.fail_export:
                raise ConnectionError('test download interrupted')
            rows = [FIELDS]
            for seq in range(2):
                for sensor in range(4):
                    rows.append(f'{seq},{self.started+seq*20},{sensor},3,-12,4,13,OK,{self.ref},{seq*20},{self.started},1,0,25,1')
            if self.corrupt_csv:
                rows.pop()
            return 200, 'text/csv', ('# test peer\n'+'\n'.join(rows)+'\n').encode()
        else:
            raise AssertionError(path)
        return 200, 'application/json', json.dumps(data).encode()


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'sessions.sqlite3'
        self.board = Board()
        self.recorder = SessionRecorder(self.board, self.path)
        self.session = self.recorder.snapshot()['session']['id']

    def record(self, distance):
        self.recorder.start(self.session, distance, 'valid')
        self.recorder.worker.join(3)
        self.assertFalse(self.recorder.worker.is_alive())
        self.assertIsNone(self.recorder.snapshot()['capture'])

    def test_pc_stream_capture_never_uses_board_record_or_export(self):
        distance=self.recorder.add_distance(self.session,2)
        self.recorder.start_stream(self.session,distance)
        self.recorder.worker.join(5)
        snapshot=self.recorder.snapshot()
        self.assertIsNone(snapshot['capture'])
        self.assertGreater(snapshot['session']['distances'][0]['trials'][0]['frames'],120)
        self.assertEqual(self.board.posts,0)
        self.assertFalse(any(p.endswith(('/record','/export.csv')) for p in self.board.paths))

    def test_manual_distances_and_trial_numbers_persist_independently(self):
        a = self.recorder.add_distance(self.session, '2.0')
        self.assertEqual(a, self.recorder.add_distance(self.session, '2.00'))
        b = self.recorder.add_distance(self.session, '2.1')
        self.record(a)
        self.record(a)
        self.record(b)
        restored = SessionRecorder(self.board, self.path).snapshot()['session']
        self.assertEqual([d['distanceCm'] for d in restored['distances']], [2.0, 2.1])
        self.assertEqual([[t['number'] for t in d['trials']] for d in restored['distances']], [[1, 2], [1]])

    def test_zip_contains_one_csv_per_recorded_distance_with_all_trials(self):
        a = self.recorder.add_distance(self.session, 2)
        b = self.recorder.add_distance(self.session, 2.1)
        self.recorder.add_distance(self.session, 9)
        self.record(a)
        self.record(a)
        self.record(b)
        with zipfile.ZipFile(io.BytesIO(self.recorder.export_all(self.session))) as archive:
            self.assertEqual(sorted(archive.namelist()), ['distance_2.0cm.csv', 'distance_2.1cm.csv'])
            rows = list(csv.DictReader(io.StringIO(archive.read('distance_2.0cm.csv').decode())))
        self.assertEqual(len(rows), 16)
        self.assertEqual({r['trial_no'] for r in rows}, {'1', '2'})
        self.assertEqual({r['distance_cm'] for r in rows}, {'2.0'})
        self.assertTrue(all(r['raw_y'] == '-12' and r['raw_m'] == '13' for r in rows))

    def test_excluding_bad_trial_is_reversible_and_does_not_renumber(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.record(distance)
        trial = self.recorder.snapshot()['session']['distances'][0]['trials'][0]['id']
        self.recorder.include_trial(self.session, trial, False)
        self.record(distance)
        rows = list(csv.DictReader(io.StringIO(self.recorder.export_distance(self.session, distance).decode())))
        self.assertEqual({r['trial_no'] for r in rows}, {'2'})
        self.recorder.include_trial(self.session, trial, True)
        self.assertEqual(len(list(csv.DictReader(io.StringIO(self.recorder.export_distance(self.session, distance).decode())))), 16)

    def test_failed_save_blocks_overwrite_and_retry_never_repeats_record_command(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.board.fail_export = True
        self.recorder.start(self.session, distance, 'valid')
        self.recorder.worker.join(3)
        self.assertEqual(self.recorder.snapshot()['capture']['state'], 'error')
        with self.assertRaises(SessionError):
            self.recorder.start(self.session, distance, 'valid')
        self.board.fail_export = False
        restored = SessionRecorder(self.board, self.path)
        restored.retry(self.session)
        restored.worker.join(3)
        self.assertIsNone(restored.snapshot()['capture'])
        self.assertEqual(self.board.posts, 1)
        self.assertEqual(len(restored.snapshot()['session']['distances'][0]['trials']), 1)

    def test_unauthorized_record_does_not_create_a_trial_or_pending_capture(self):
        distance = self.recorder.add_distance(self.session, 2)
        with self.assertRaises(SessionError) as error:
            self.recorder.start(self.session, distance, 'wrong')
        self.assertEqual(error.exception.status, 401)
        self.assertIsNone(self.recorder.snapshot()['capture'])
        self.assertEqual(self.board.posts, 0)

    def test_validation_and_session_isolation(self):
        for value in ('', 'nan', 'inf', 51, -51, True, [], '2.00001'):
            with self.subTest(value=value), self.assertRaises(SessionError):
                self.recorder.add_distance(self.session, value)
        distance = self.recorder.add_distance(self.session, 2)
        other = self.recorder.create_session('Second dataset')
        with self.assertRaises(SessionError):
            self.recorder.start(other, distance, 'valid')
        with self.assertRaises(SessionError):
            self.recorder.export_all(other)

    def test_incomplete_csv_never_becomes_a_saved_trial(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.board.corrupt_csv = True
        self.recorder.start(self.session, distance, 'valid')
        self.recorder.worker.join(3)
        snapshot = self.recorder.snapshot()
        self.assertEqual(snapshot['capture']['state'], 'error')
        self.assertEqual(snapshot['session']['distances'][0]['trials'], [])

    def test_new_session_does_not_duplicate_retained_board_data(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.record(distance)
        other = self.recorder.create_session('New session')
        restored = SessionRecorder(self.board, self.path)
        distance = restored.add_distance(other, 2.1)
        restored.start(other, distance, 'valid')
        restored.worker.join(3)
        groups = restored.snapshot(other)['session']['distances']
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0]['trials'][0]['number'], 1)

    def test_rename_updates_labels_and_trial_name_without_changing_raw_data(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.record(distance)
        trial = self.recorder.snapshot()['session']['distances'][0]['trials'][0]['id']
        self.recorder.rename_session(self.session, 'Ruler session')
        self.recorder.rename_trial(self.session, trial, 'Magnet facing left')
        self.recorder.rename_distance(self.session, distance, '2.1')
        rows = list(csv.DictReader(io.StringIO(self.recorder.export_distance(self.session, distance).decode())))
        self.assertEqual({r['distance_cm'] for r in rows}, {'2.1'})
        self.assertEqual({float(r['ref_mm']) for r in rows}, {21})
        self.assertEqual({r['trial_name'] for r in rows}, {'Magnet facing left'})
        graph = self.recorder.trial_data(self.session, trial)
        self.assertEqual(graph['trial']['number'], 1)
        self.assertEqual(graph['trial']['distanceCm'], 2.1)
        self.assertEqual([f['elapsedMs'] for f in graph['frames']], [0,20])
        self.assertEqual(graph['frames'][0]['s'][0]['ry'], -12)
        self.assertEqual(graph['frames'][0]['s'][0]['rm'], 13)

    def test_removed_trial_is_not_reimported_and_numbers_and_ids_are_not_reused(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.record(distance)
        trial = self.recorder.snapshot()['session']['distances'][0]['trials'][0]['id']
        self.recorder.remove_trial(self.session, trial)
        self.assertEqual(self.recorder.snapshot()['session']['distances'][0]['nextTrial'], 2)
        self.recorder = SessionRecorder(self.board, self.path)
        self.record(distance)
        trials = self.recorder.snapshot()['session']['distances'][0]['trials']
        self.assertEqual([t['number'] for t in trials], [2])
        self.assertGreater(trials[0]['id'], trial)
        self.recorder.remove_distance(self.session, distance)
        replacement = self.recorder.add_distance(self.session, 2)
        self.assertNotEqual(replacement, distance)
        self.assertEqual(self.recorder.snapshot()['session']['distances'][0]['trials'], [])

    def test_remove_session_preserves_other_sessions_and_always_leaves_a_workspace(self):
        other = self.recorder.create_session('Other')
        self.recorder.remove_session(self.session)
        self.assertEqual(self.recorder.snapshot()['session']['id'], other)
        self.recorder.remove_session(other)
        self.assertEqual(len(self.recorder.snapshot()['sessions']), 1)

    def test_distance_rename_conflict_does_not_merge_or_lose_trials(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.recorder.add_distance(self.session, 2.1)
        self.record(distance)
        with self.assertRaises(SessionError):
            self.recorder.rename_distance(self.session, distance, 2.1)
        self.assertEqual(len(self.recorder.snapshot()['session']['distances'][0]['trials']), 1)

    def test_existing_database_migration_keeps_trials_and_advances_counters(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.record(distance)
        self.record(distance)
        with sqlite3.connect(self.path) as db:
            db.execute('ALTER TABLE trials DROP COLUMN name')
            db.execute('ALTER TABLE distances DROP COLUMN next_number')
            db.execute('DROP TABLE id_sequences')
            db.execute('DROP TABLE known_captures')
        db.close()
        restored = SessionRecorder(self.board, self.path)
        group = restored.snapshot()['session']['distances'][0]
        self.assertEqual([t['number'] for t in group['trials']], [1,2])
        self.assertEqual(group['nextTrial'], 3)
        self.assertEqual(restored.trial_data(self.session, group['trials'][0]['id'])['frames'][0]['s'][0]['ry'], -12)

    def test_trial_names_escape_spreadsheet_formula_prefixes(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.record(distance)
        trial = self.recorder.snapshot()['session']['distances'][0]['trials'][0]['id']
        self.recorder.rename_trial(self.session, trial, '=SUM(1,2)')
        rows = list(csv.DictReader(io.StringIO(self.recorder.export_distance(self.session, distance).decode())))
        self.assertEqual(rows[0]['trial_name'], "'=SUM(1,2)")

    def test_capture_blocks_destructive_or_label_edits_until_saved(self):
        distance = self.recorder.add_distance(self.session, 2)
        self.record(distance)
        trial = self.recorder.snapshot()['session']['distances'][0]['trials'][0]['id']
        self.board.hold_after_start = True
        self.recorder.start(self.session, distance, 'valid')
        try:
            for action in (lambda:self.recorder.remove_session(self.session),
                           lambda:self.recorder.remove_distance(self.session, distance),
                           lambda:self.recorder.remove_trial(self.session, trial),
                           lambda:self.recorder.rename_distance(self.session, distance, 2.1)):
                with self.assertRaises(SessionError):
                    action()
        finally:
            self.board.gate.set()
            self.recorder.worker.join(3)


class HttpSessionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.board = Board()
        self.server = make_server(self.board, 0, Path(self.temp.name)/'sessions.sqlite3')
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.session = self.server.sessions.snapshot()['session']['id']

    def request(self, path, body=None, headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=3)
        try:
            connection.request('GET' if body is None else 'POST', '/api/v1/sessions'+path,
                               None if body is None else json.dumps(body), headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_http_session_record_and_per_distance_download(self):
        status, _, data = self.request('/'+self.session+'/distances', {'distanceCm':'2.0'})
        self.assertEqual(status, 200)
        distance = json.loads(data)['selectedDistanceId']
        status, _, _ = self.request('/'+self.session+'/record', {'distanceId':distance}, {'X-SCRATCHSOR-Token':'valid'})
        self.assertEqual(status, 200)
        self.server.sessions.worker.join(5)
        status, headers, data = self.request('/'+self.session+'/distances/'+str(distance)+'.csv')
        self.assertEqual(status, 200)
        self.assertIn('distance_2.0cm.csv', headers['Content-Disposition'])
        self.assertGreater(len(list(csv.DictReader(io.StringIO(data.decode())))), 480)
        status, headers, data = self.request('/'+self.session+'/export.zip')
        self.assertEqual(status, 200)
        self.assertEqual(headers['Content-Type'], 'application/zip')
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            self.assertEqual(archive.namelist(), ['distance_2.0cm.csv'])

    def test_local_changes_reject_foreign_origins_and_require_valid_shapes(self):
        before = self.server.sessions.snapshot()
        self.assertEqual(self.request('', {'name':'Injected'}, {'Origin':'https://evil.example'})[0], 403)
        self.assertEqual(self.request('', [1,2])[0], 400)
        self.assertEqual(len(self.server.sessions.snapshot()['sessions']), len(before['sessions']))
        self.assertEqual(self.request('/'+self.session+'/record', {'distanceId':True})[0], 400)

    def test_http_rename_remove_and_stored_graph_routes(self):
        distance = self.server.sessions.add_distance(self.session, 2)
        self.server.sessions.start(self.session, distance, 'valid')
        self.server.sessions.worker.join(3)
        trial = self.server.sessions.snapshot()['session']['distances'][0]['trials'][0]['id']
        base = '/'+self.session
        self.assertEqual(self.request(base+'/rename', {'name':'Renamed session'})[0], 200)
        self.assertEqual(self.request(base+'/trials/'+str(trial)+'/rename', {'name':'Facing left'})[0], 200)
        self.assertEqual(self.request(base+'/distances/'+str(distance)+'/rename', {'distanceCm':2.1})[0], 200)
        status, _, data = self.request(base+'/trials/'+str(trial))
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data)['trial']['name'], 'Facing left')
        self.assertEqual(json.loads(data)['frames'][0]['s'][0]['ry'], -12)
        self.assertEqual(self.request(base+'/trials/'+str(trial)+'/remove', {})[0], 200)
        self.assertEqual(self.request(base+'/trials/'+str(trial))[0], 404)
        self.assertEqual(self.request(base+'/distances/'+str(distance)+'/remove', {})[0], 200)
        self.assertEqual(self.request(base+'/remove', {})[0], 200)


if __name__ == '__main__':
    unittest.main()
