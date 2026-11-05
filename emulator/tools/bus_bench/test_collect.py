"""Ensure incomplete runs and changed card inputs never become timing evidence."""
import json
from pathlib import Path
import tempfile
import unittest

from collect import collect, digest


class Collect(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.boot = Path(self.temp.name) / 'boot'
        self.boot.mkdir()
        self.out = Path(self.temp.name) / 'evidence'
        files = {}
        for name in ('buspipe.app', 'buspipe.ico', 'kernel.elf', 'init.app', 'init.ini'):
            (self.boot / name).write_bytes(name.encode())
            files[name] = digest(name.encode())
        manifest = {'version': 1, 'probes': {'base': 5}, 'placements': [['a0', 0]],
                    'data': ['a0'], 'cases': 1, 'app_sha256': files['buspipe.app']}
        (self.boot / 'buspipe.json').write_text(json.dumps({'manifest': manifest, 'files': files}))
        self.text = ('BUSPIPE v1 passes=20000\n'
                     'RESULT probe=base code=a0 offset=0 data=a0 bytes=20 '
                     'passes=20000 ticks=260001,260002,260003 result=5 ok=1\n'
                     'END BUSPIPE cases=1 failures=0\n')
        self.log = self.boot / 'buspipe.log'
        self.log.write_text(self.text)

    def test_preserves_report_and_hashes(self):
        report = collect(self.boot, self.out)
        self.assertEqual(report.read_text(), self.text)
        record = json.loads((self.out / 'device.json').read_text())
        self.assertEqual(record['report']['sha256'], digest(self.text.encode()))
        self.assertEqual(collect(self.boot, self.out), report)

    def test_changed_binary_is_rejected_before_writing_evidence(self):
        (self.boot / 'kernel.elf').write_bytes(b'different kernel')
        with self.assertRaisesRegex(ValueError, 'Card input changed'):
            collect(self.boot, self.out)
        self.assertFalse(self.out.exists())

    def test_bad_or_partial_logs_are_rejected(self):
        for bad in (self.text.split('END')[0], self.text.replace('ok=1', 'ok=0'),
                    self.text.replace('260001', '0'),
                    self.text.replace('END BUSPIPE', self.text.splitlines()[1] + '\nEND BUSPIPE'),
                    self.text.replace('result=5', 'result=16')):
            with self.subTest(bad=bad):
                self.log.write_text(bad)
                with self.assertRaises(ValueError):
                    collect(self.boot, self.out)
                self.assertFalse(self.out.exists())


if __name__ == '__main__':
    unittest.main()
