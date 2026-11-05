"""Exercise ordinary file installation, menu preservation and identity checks."""
import json
from pathlib import Path
import tempfile
import unittest

from collect import digest, collect
from install import install, plan, OLD_ENTRY, NEW_ENTRY
from test_report import fixture


class Installation(unittest.TestCase):
    def prepare(self, root):
        boot, stage, validation, archive = (root / name for name in ('card', 'stage', 'run', 'archive'))
        for path in (boot, stage, validation / 'portq', archive):
            path.mkdir(parents=True)
        manifest, report = fixture()
        for name in ('kernel.elf', 'init.app', 'zcal.app', 'zcal.ico', 'zimlog.on', 'zimbench.lst'):
            (boot / name).write_text(name)
        menu = b''.join(f'item{i}.ico : item{i}.app option\n'.encode() for i in range(8)) + OLD_ENTRY + b'\n'
        (boot / 'init.ini').write_bytes(menu)
        files = {p.name: digest(p.read_bytes()) for p in boot.iterdir()}
        identity = {'install_id': 'previous-install', 'files': files, 'volume_uuid': 'reader-uuid'}
        device = {**identity}
        for key, name in [('report', 'zimbench.txt'), ('startup_report', 'zimboot.log')]:
            (boot / name).write_text(name)
            (archive / name).write_text(name)
            device[key] = {'file': name, 'sha256': digest(name.encode())}
        (archive / 'device.json').write_text(json.dumps(device))
        for name in ('portq.app', 'portq.ico', 'portq.c'):
            (stage / name).write_text('new-' + name)
        manifest.update(app_sha256=digest((stage / 'portq.app').read_bytes()),
                        source_sha256=digest((stage / 'portq.c').read_bytes()))
        (stage / 'manifest.json').write_text(json.dumps(manifest))
        (validation / 'portq/portq.log').write_text(report)
        (validation / 'wremu').write_text('emulator-snapshot')
        inputs = {name.replace('portq.', 'bench.'): digest((stage / name if name.startswith('portq.') else boot / name).read_bytes())
                  for name in ('portq.app', 'portq.ico', 'kernel.elf', 'init.app')}
        result = {'manifest': manifest, 'cases': manifest['cases'], 'inputs': {'portq': inputs},
                  'report_sha256': digest(report.encode()), 'emulator_sha256': digest(b'emulator-snapshot')}
        (validation / 'results.json').write_text(json.dumps(result))
        for name in ('portq.app', 'portq.ico', 'portq.json', 'portq.log'):
            (boot / name).write_text('old-' + name)
        disk = {'VolumeUUID': 'reader-uuid', 'Internal': False, 'BusProtocol': 'USB',
                'Content': 'DOS_FAT_32', 'WritableVolume': True, 'ParentWholeDisk': 'disk-test'}
        return boot, stage, validation, identity, archive, disk

    def test_backup_preserve_menu_and_collect_new_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            args = self.prepare(root)
            boot, stage, validation, identity, archive, disk = args
            original = {p.name: p.read_bytes() for p in boot.iterdir()}
            manifest, launcher = plan(*args)
            self.assertEqual(launcher.splitlines()[:8], original['init.ini'].splitlines()[:8])
            self.assertEqual(launcher.splitlines()[8], NEW_ENTRY)
            record = install(*args, root / 'backups')
            backup = Path(record['backup'])
            for name in ('portq.app', 'portq.ico', 'portq.json', 'portq.log', 'init.ini'):
                self.assertEqual((backup / name).read_bytes(), original[name])
            for name in ('kernel.elf', 'init.app', 'zcal.app', 'zimbench.txt', 'zimboot.log'):
                self.assertEqual((boot / name).read_bytes(), original[name])
            self.assertFalse((boot / 'portq.log').exists())
            self.assertEqual((boot / 'init.ini').read_bytes(), launcher)
            # The installed identity is directly consumable by the existing
            # collector once the newly selected application writes its report.
            (boot / 'portq.log').write_bytes((validation / 'portq/portq.log').read_bytes())
            collect(boot, root / 'collected')
            self.assertTrue(json.loads((root / 'collected/device.json').read_text())['valid'])

    def test_wrong_volume_or_changed_evidence_writes_nothing(self):
        for change in ('internal', 'uuid', 'build', 'report', 'kernel', 'validation'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                args = self.prepare(root)
                boot, stage, validation, identity, archive, disk = args
                if change == 'internal': disk['Internal'] = True
                elif change == 'uuid': disk['VolumeUUID'] = 'different-reader'
                elif change == 'build': (stage / 'portq.app').write_text('changed')
                elif change == 'report': (boot / 'zimbench.txt').write_text('not-archived')
                elif change == 'kernel': (boot / 'kernel.elf').write_text('changed')
                else: (validation / 'wremu').write_text('changed')
                before = {p.name: p.read_bytes() for p in boot.iterdir()}
                with self.assertRaises(ValueError):
                    install(*args, root / 'backups')
                self.assertFalse((root / 'backups').exists())
                self.assertEqual(before, {p.name: p.read_bytes() for p in boot.iterdir()})


if __name__ == '__main__':
    unittest.main()
