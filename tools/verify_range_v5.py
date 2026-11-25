"""Compare exported float32 inference and capture voting with Python on real data.

Requires NumPy and `pip install ziglang`; no recordings are written.
Run from the project root. Report is written to models/v5/native_verification.json.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
from range_model import RangeModel
from train_range_v5 import data, prepare


def main():
    model = RangeModel('models/v5/model.json')
    trials, inputs, _ = data()
    reference = json.loads(Path('models/v4/no_magnet_reference.json').read_text())
    cache = [prepare(t, reference) for t in trials]
    vectors = [(x, []) for trial in cache for x in trial['X']]
    attempts = [a for trial in cache for a in trial['attempts']]
    vectors += [(a['features'], []) for a in attempts]
    vectors += [(a['features'], a['windows']) for a in attempts]
    lines = []
    expected = []
    for features, windows in vectors:
        values = list(features)+[len(windows)]+[v for w in windows for v in w]
        lines.append(' '.join(format(float(v), '.12g') for v in values))
        expected.append(model.capture(features, windows) if len(windows) else model.predict(features))
    with tempfile.TemporaryDirectory(prefix='scratchsor-v5-') as tmp:
        executable = str(Path(tmp)/'verify.exe')
        subprocess.run([sys.executable, '-m', 'ziglang', 'c++', '-std=c++17', '-O2',
                        '-Iinclude', 'tools/check_range_v5.cpp', '-o', executable], check=True)
        result = subprocess.run([executable], input='\n'.join(lines)+'\n', text=True,
                                capture_output=True, check=True)
    native = np.array([list(map(float, line.split())) for line in result.stdout.splitlines()])
    assert len(native) == len(expected)
    assert all(p['class'] == int(n[0]) and p['accepted'] == bool(n[1])
               for p, n in zip(expected, native)), 'Class or acceptance differs'
    error = float(np.max(np.abs(np.array([p['probabilities'] for p in expected])-native[:, 2:5])))
    assert error < 1e-4, error
    import hashlib
    assert all(hashlib.sha256(Path(p['path']).read_bytes()).hexdigest() == p['sha256'] for p in inputs)
    report = {'real_feature_vectors': len(vectors)-len(attempts),
              'capture_consensus_checks': len(attempts),
              'maximum_probability_difference': error,
              'class_and_acceptance_agreement': True, 'input_files_unchanged': True}
    Path('models/v5/native_verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
