"""Exercise the six-entry card transition without accessing a real volume."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from collect import collect
from install import install, OLD_ENTRY, NEW_ENTRY
from test_report import fixture


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare(root):
    boot, stage, validation, device = [root / name for name in ('card', 'build', 'run', 'device')]
    for p in (boot, stage, validation / 'fetchq', device):
        p.mkdir(parents=True)
    menu = b''.join(f'app{i}.ico : app{i}.app option\n'.encode() for i in range(5)) + OLD_ENTRY + b'\n'
    (boot / 'init.ini').write_bytes(menu)
    for name in ('portq.app', 'portq.ico', 'portq.json', 'kernel.elf', 'init.app'):
        (boot / name).write_text(name)
    preserved = {}
    for i in range(5):
        for suffix in ('app', 'ico'):
            name = f'app{i}.{suffix}'
            (boot / name).write_text(name)
            preserved[name] = digest(name.encode())
    previous_files = {name: digest((boot / name).read_bytes()) for name in
                      ('portq.app', 'portq.ico', 'kernel.elf', 'init.app', 'init.ini')}
    previous_manifest = dict(cases=560, app_sha256=previous_files['portq.app'])
    identity = dict(volume_uuid='reader-uuid', files=previous_files,
                    manifest=previous_manifest, cleanup=dict(preserved_apps=preserved))
    old_report = b'archived, verified port report'
    (boot / 'portq.log').write_bytes(old_report)
    (device / 'port.txt').write_bytes(old_report)
    (device / 'device.json').write_text(json.dumps(dict(valid=True, inputs=previous_files,
        manifest=previous_manifest, report=dict(file='port.txt', sha256=digest(old_report)))))
    manifest, report = fixture()
    for name, key in [('fetchq.app', 'app_sha256'), ('fetchq.ico', 'icon_sha256'), ('fetch.c', 'source_sha256')]:
        (stage / name).write_text(name)
        manifest[key] = digest(name.encode())
    (stage / 'manifest.json').write_text(json.dumps(manifest))
    (stage / 'prospective.json').write_text(json.dumps(dict(complete=True, manifest=manifest, models=dict(a={}, b={}))))
    (validation / 'fetchq/fetchq.log').write_text(report)
    (validation / 'wremu').write_bytes(b'qualified emulator')
    inputs = {name: digest((boot / name).read_bytes()) for name in ('kernel.elf', 'init.app')}
    inputs.update({'bench.app': manifest['app_sha256'], 'bench.ico': manifest['icon_sha256']})
    (validation / 'results.json').write_text(json.dumps(dict(complete=True, manifest=manifest, cases=600,
        emulator_sha256=digest(b'qualified emulator'), report_sha256=digest(report.encode()), inputs=dict(fetchq=inputs))))
    disk = dict(VolumeUUID='reader-uuid', Internal=False, BusProtocol='USB', Content='DOS_FAT_32',
                WritableVolume=True, ParentWholeDisk='disk-test')
    return boot, stage, validation, identity, device, disk


class Transition(unittest.TestCase):
    def test_backup_menu_preservation_cleanup_and_collection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            args = prepare(root)
            boot, stage, validation, identity, device, disk = args
            before = {p.name: p.read_bytes() for p in boot.iterdir()}
            record = install(*args, root / 'backups')
            backup = Path(record['backup'])
            for name in ('portq.app', 'portq.ico', 'portq.json', 'portq.log', 'init.ini'):
                self.assertEqual((backup / name).read_bytes(), before[name])
            menu = (boot / 'init.ini').read_bytes().splitlines()
            self.assertEqual(menu[:5], before['init.ini'].splitlines()[:5])
            self.assertEqual(menu[5], NEW_ENTRY)
            self.assertEqual(len(menu), 6)
            for name in identity['cleanup']['preserved_apps']:
                self.assertEqual((boot / name).read_bytes(), before[name])
            self.assertFalse(list(boot.glob('portq.*')))
            (boot / 'fetchq.log').write_bytes((validation / 'fetchq/fetchq.log').read_bytes())
            collect(boot, root / 'collected')
            self.assertTrue(json.loads((root / 'collected/device.json').read_text())['valid'])

    def test_unreturned_probe_or_changed_inputs_cannot_write(self):
        for change in ('uncollected', 'report', 'uuid', 'internal', 'normal-app', 'build', 'source', 'validation', 'forecasts'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                args = prepare(root)
                boot, stage, validation, identity, device, disk = args
                if change == 'uncollected':
                    d = json.loads((device / 'device.json').read_text()); d['valid'] = False
                    (device / 'device.json').write_text(json.dumps(d))
                elif change == 'report': (boot / 'portq.log').write_text('not collected')
                elif change == 'uuid': disk['VolumeUUID'] = 'other-card'
                elif change == 'internal': disk['Internal'] = True
                elif change == 'normal-app': (boot / 'app0.app').write_text('changed')
                elif change == 'build': (stage / 'fetchq.app').write_text('changed')
                elif change == 'source': (stage / 'fetch.c').write_text('changed')
                elif change == 'validation': (validation / 'wremu').write_text('changed')
                else: (stage / 'prospective.json').write_text(json.dumps(dict(complete=False)))
                before = {p.name: p.read_bytes() for p in boot.iterdir()}
                with self.assertRaises(ValueError):
                    install(*args, root / 'backups')
                self.assertFalse((root / 'backups').exists())
                self.assertEqual({p.name: p.read_bytes() for p in boot.iterdir()}, before)


if __name__ == '__main__':
    unittest.main()
