"""Check that hardware evidence cannot omit or alter a native contrast."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from collect import collect
from analyze import comparison
from report import SHAPES, expected_cases, validate


def fixture():
    addresses = dict(sdram_code=0x10042000, external=0x10800000,
                     local_code=0xe00, local_data=0xf00, measure=0xc00,
                     local_stack=0xd00, work=0xf20)
    manifest = dict(version=1, cases=600, repeats=3, passes=[1, 8, 8192],
                    offsets=list(range(0, 16, 2)), iqb=[0, 1], addresses=addresses,
                    probes={})
    for shape, (padding, kind) in SHAPES.items():
        for suffix in ('n', 'd'):
            manifest['probes'][shape + '_' + suffix] = dict(
                padding=padding, kind=kind, bytes=32 + 2 * padding, loop_offset=16)
    header = ('FETCHQ v1 repeats=3 sdram_ctl=00001243 refresh=000001c0 app=8000000b '
              'clkcntl=00000001 pll=00000001 timer0=0003 clock0=0080 timer5=0003 clock5=0080 '
              'code=10042000 data=10800000 a0=00000e00 wrapper=00000c00 stack=00000d00')
    lines = [header]
    for shape, branch, code, offset, data, iqb, passes in sorted(expected_cases()):
        padding, kind = SHAPES[shape]
        entry = (addresses['local_code'] if code == 'a0' else addresses['sdram_code'] + 256) + int(offset)
        src = addresses['external'] + 0x1000 if data == 'sdram' else addresses['local_data'] + 4
        dst = addresses['external'] + 0x21000 if data == 'sdram' else addresses['local_data'] + 8
        result = (13 if kind else 5) ^ (padding * int(passes))
        lines.append(f'RESULT probe={shape} branch={branch} code={code} offset={offset} data={data} '
                     f'iqb={iqb} passes={passes} bytes={32+2*padding} entry={entry:08x} loop={entry+16:08x} '
                     f'src={src:08x} dst={dst:08x} ticks=100,101,102 result={result} ok=1')
    lines.append('END FETCHQ cases=600 failures=0')
    return manifest, '\n'.join(lines) + '\n'


class Evidence(unittest.TestCase):
    def test_complete_native_matrix(self):
        manifest, text = fixture()
        _, rows = validate(text, manifest)
        self.assertEqual(len(rows), 600)
        self.assertEqual(sum(k[2] == 'a0' for k in rows), 24)

    def test_corrupt_or_missing_contrast_rejected(self):
        manifest, text = fixture()
        lines = text.splitlines()
        mutations = [text.replace('failures=0', 'failures=1'),
                     '\n'.join(lines[:-1]), '\n'.join(lines[:1] + lines[2:]),
                     '\n'.join(lines[:2] + [lines[1]] + lines[3:]),
                     text.replace('ok=1', 'ok=0', 1),
                     text.replace('ticks=100,101,102', 'ticks=100,0,102', 1),
                     text.replace('ticks=100,101,102', 'ticks=100,101', 1),
                     text.replace('result=69', 'result=68', 1),
                     text.replace('bytes=48', 'bytes=50', 1),
                     text.replace('loop=00000e10', 'loop=00000e12', 1),
                     text.replace('src=00000f04', 'src=00000f08', 1),
                     text.replace('clock0=0080', 'clock0=invalid', 1),
                     text.replace('iqb=0', 'iqb=0 iqb=1', 1)]
        for changed in mutations:
            with self.subTest(changed=changed[:80]):
                self.assertNotEqual(changed, text)
                with self.assertRaises(ValueError):
                    validate(changed, manifest)

    def test_manifest_and_address_identity_required(self):
        manifest, text = fixture()
        for field, value in [('cases', 599), ('repeats', 2), ('passes', [1, 8, 4096]), ('iqb', [1])]:
            changed = copy.deepcopy(manifest)
            changed[field] = value
            with self.assertRaises(ValueError):
                validate(text, changed)

    def test_comparison_keeps_short_long_and_iqb_groups_separate(self):
        manifest, text = fixture()
        result = comparison(text, text, manifest)
        self.assertEqual(len(result['cases']), 600)
        self.assertEqual(sum(g['cases'] for g in result['groups']), 600)
        self.assertTrue(all(g['cases'] == g['within10'] for g in result['groups']))
        self.assertEqual({g['passes'] for g in result['groups']}, {1, 8, 8192})
        self.assertEqual({g['iqb'] for g in result['groups']}, {0, 1})
        self.assertTrue(all(row['median_ticks'] == dict(model=101, device=101) for row in result['cases']))
        with self.assertRaisesRegex(ValueError, 'contexts differ'):
            comparison(text, text.replace('pll=00000001', 'pll=00000002'), manifest)
        with self.assertRaisesRegex(ValueError, 'contexts differ'):
            comparison(text, text.replace('refresh=000001c0', 'refresh=000001c1'), manifest)
        status = comparison(text.replace('refresh=000001c0', 'refresh=020001c0'), text, manifest)
        self.assertEqual(status['self_refresh_status'], dict(model=True, device=False))
        self.assertEqual(status['raw_context']['model']['refresh'], '020001c0')

    def test_collection_preserves_failures_and_invalidates_latest(self):
        manifest, text = fixture()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            boot, archive = root / 'card', root / 'archive'
            boot.mkdir()
            inputs = {}
            for name in ('fetchq.app', 'fetchq.ico', 'kernel.elf', 'init.app', 'init.ini'):
                data = name.encode()
                (boot / name).write_bytes(data)
                inputs[name] = hashlib.sha256(data).hexdigest()
            manifest.update(app_sha256=inputs['fetchq.app'], icon_sha256=inputs['fetchq.ico'])
            (boot / 'fetchq.json').write_text(json.dumps(dict(manifest=manifest, files=inputs)))
            (boot / 'fetchq.log').write_text(text)
            report = collect(boot, archive)
            self.assertEqual(report.read_text(), text)
            self.assertTrue(json.loads((archive / 'device.json').read_text())['valid'])
            failed = text.replace('ok=1', 'ok=0', 1)
            (boot / 'fetchq.log').write_text(failed)
            with self.assertRaises(ValueError):
                collect(boot, archive)
            self.assertFalse(json.loads((archive / 'device.json').read_text())['valid'])
            self.assertIn(failed, [p.read_text() for p in archive.glob('device-*.txt')])
            (boot / 'fetchq.log').write_text(text)
            (boot / 'fetchq.app').write_text('changed')
            with self.assertRaises(ValueError):
                collect(boot, archive)
            self.assertFalse(json.loads((archive / 'device.json').read_text())['valid'])
        for field in ('sdram_code', 'external', 'local_code', 'measure', 'local_stack'):
            changed = copy.deepcopy(manifest)
            changed['addresses'][field] += 16
            with self.assertRaises(ValueError):
                validate(text, changed)


if __name__ == '__main__':
    unittest.main()
