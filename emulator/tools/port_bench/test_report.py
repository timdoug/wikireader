"""Reject stale workloads and retain native failure evidence."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collect import collect, digest
from analyze import compare
from run import validate


def fixture():
    probes = {f'ldh_u{u}': {'shape': 'ldh', 'unroll': u, 'result': 13, 'bytes': 26 + 2 * u}
              for u in (1, 8)}
    manifest = {'version': 1, 'passes': 20000, 'repeats': 3, 'cases': 8,
                'probes': probes, 'placements': [['a0', 0], ['a0', 2]], 'data': ['a0', 'sdram']}
    lines = ['PORTQ v1 passes=20000 unroll=variable repeats=3 sdram_ctl=00001243 '
             'refresh=03ff01c0 clkcntl=0a7b050e pll=00101a95 a0=00000c00 sdram=10044630 code=00000c10']
    for name, probe in probes.items():
        ticks = (5 + probe['unroll'] * 3) * 20000
        for offset in (0, 2):
            for data in ('a0', 'sdram'):
                lines.append(f'RESULT probe={name} code=a0 offset={offset} data={data} '
                             f'bytes={probe["bytes"]} passes=20000 unroll={probe["unroll"]} '
                             f'ticks={ticks},{ticks},{ticks} result=13 ok=1')
    return manifest, '\n'.join(lines + ['END PORTQ cases=8 failures=0', ''])


class Report(unittest.TestCase):
    def test_coverage_and_native_results(self):
        manifest, text = fixture()
        self.assertEqual(len(validate(text, manifest)), 8)
        lines = text.splitlines(keepends=True)
        bad = [text.replace('unroll=1 ticks', 'unroll=2 ticks', 1),
               text.replace('bytes=28', 'bytes=30', 1),
               text.replace('result=13', 'result=5', 1),
               text.replace('ok=1', 'ok=0', 1),
               text.replace('160000,160000,160000', '0,160000,160000', 1),
               ''.join(lines[:1] + lines[2:]), ''.join(lines[:2] + lines[1:])]
        for report in bad:
            with self.subTest(report=report[:180]), self.assertRaises(ValueError):
                validate(report, manifest)

    def test_curve_keeps_raw_time_and_actual_unroll(self):
        manifest, text = fixture()
        result = compare(text, text, manifest)
        self.assertEqual(result['accuracy']['within_10_percent'], 8)
        for curve in result['curves'].values():
            self.assertEqual([row['unroll'] for row in curve], [1, 8])
            self.assertEqual(curve[0]['device_cycles_per_pass'], 8)
            self.assertEqual(curve[1]['device_cycles_per_pass'], 29)
            self.assertEqual(curve[1]['device_incremental_cycles_per_copy'], 3)
        with self.assertRaisesRegex(ValueError, 'contexts differ'):
            compare(text, text.replace('pll=00101a95', 'pll=00101a96'), manifest)

    def test_archive_failure_invalidates_latest_success(self):
        manifest, text = fixture()
        with tempfile.TemporaryDirectory() as tmp:
            boot, out = Path(tmp) / 'card', Path(tmp) / 'evidence'
            boot.mkdir()
            files = {}
            for name in ('portq.app', 'portq.ico', 'kernel.elf', 'init.app', 'init.ini'):
                (boot / name).write_text(name)
                files[name] = digest(name.encode())
            manifest = copy.deepcopy(manifest)
            manifest['app_sha256'] = files['portq.app']
            (boot / 'portq.json').write_text(json.dumps({'manifest': manifest, 'files': files}))
            (boot / 'portq.log').write_text(text)
            collect(boot, out)
            self.assertTrue(json.loads((out / 'device.json').read_text())['valid'])
            failed = text.replace('result=13', 'result=5', 1)
            (boot / 'portq.log').write_text(failed)
            with self.assertRaisesRegex(ValueError, 'Report preserved'):
                collect(boot, out)
            record = json.loads((out / 'device.json').read_text())
            self.assertFalse(record['valid'])
            self.assertEqual((out / record['report']['file']).read_text(), failed)
            (boot / 'portq.log').write_text(text)
            (boot / 'init.app').write_text('changed')
            with self.assertRaisesRegex(ValueError, 'Card input changed: init.app'):
                collect(boot, out)


if __name__ == '__main__':
    unittest.main()
