"""Compile and run the real Arduino-free classifier with the installed host compiler."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class FingerprintTests(unittest.TestCase):
    def test_native_classifier(self):
        self.assertTrue((ROOT / 'include/services/fingerprint.h').exists(), 'fingerprint engine missing')
        compiler = shutil.which('g++') or shutil.which('clang++')
        command = [compiler] if compiler else [sys.executable, '-m', 'ziglang', 'c++']
        unity = ROOT / '.pio/libdeps/native/Unity/src'
        with tempfile.TemporaryDirectory() as temp:
            for source in sorted((ROOT / 'test').glob('test_*/test_main.cpp')):
                with self.subTest(suite=source.parent.name):
                    exe = Path(temp) / (source.parent.name + '.exe')
                    result = subprocess.run(command + ['-x', 'c++', '-std=c++17', '-I' + str(ROOT / 'include'),
                        '-I' + str(unity), str(source), str(unity / 'unity.c'), '-o', str(exe)],
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    print(result.stdout)

if __name__ == '__main__':
    unittest.main()
