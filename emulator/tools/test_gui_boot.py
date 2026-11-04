#!/usr/bin/env python3
"""Boot the GUI build the way the device boots: MBR, the FLASH boot loader,
grifo as kernel.elf, then init.app drawing the launcher menu.

The loader/kernel handoff and the kernel's first DMA reads run here with the
window's event polling live: SDRAM descriptor traffic can move MCLK past an
SPI deadline, and the CPU sleeps between SPI DMA bytes, neither of which may
add a GUI poll delay to every byte.  The card is generated, so the test needs
only grifo and init.app built; without them it skips."""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GRIFO = ROOT / 'samo-lib/grifo/grifo.elf'
INIT = ROOT / 'samo-lib/grifo/applications/init/init.app'
ICON = ROOT / 'zim/zim.ico'

missing = [p for p in (GRIFO, INIT, ICON) if not p.is_file()]
if missing:
    print('test-gui-boot: %s not built, skipping' % ', '.join(str(p.relative_to(ROOT)) for p in missing))
    sys.exit(0)

spec = importlib.util.spec_from_file_location('fat', ROOT / 'emulator/tools/mem_dma_bench/run.py')
fat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fat)

with tempfile.TemporaryDirectory(prefix='wr-gui-boot-') as tmp:
    tmp = Path(tmp)
    # Two entries, so init draws its menu rather than starting the one app.
    icon = ICON.read_bytes()
    fat.make_image(tmp / 'card.img', {
        'kernel.elf': GRIFO.read_bytes(), 'init.app': INIT.read_bytes(),
        'a.ico': icon, 'b.ico': icon,
        'init.ini': b'a.ico : a.app\nb.ico : b.app\n'}, 64)
    flash = fat.build_flash(tmp)
    env = dict(os.environ, SDL_VIDEODRIVER='dummy')
    run = subprocess.run([str(ROOT / 'emulator/wremu'), '-g', '-R', '-N', '3,1000000',
                          '-n', '25000000', '-e', str(flash), '-c', str(tmp / 'card.img')],
                         cwd=tmp, env=env, capture_output=True, text=True, errors='replace', timeout=120)
    log = run.stdout + run.stderr
    checks = {
        'exit status 0': run.returncode == 0,
        'Grifo starting': 'Grifo starting' in log,
        'kernel DMA reads': re.search(r'^dma: ok, [1-9]\d* blocks', log, re.M),
        'init starting': 'init starting' in log,
        'LCD programmed': re.search(r'--- lcd: [1-9]\d* register writes', log),
        'menu presented': re.search(r'--- display: \d+ update calls, [1-9]\d* presents', log),
    }
    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        sys.stdout.write(log[-4000:])
        sys.exit('FAIL: test-gui-boot: ' + ', '.join(failed))
print('PASS: GUI boot through MBR, loader, grifo (DMA reads) and the launcher menu')
