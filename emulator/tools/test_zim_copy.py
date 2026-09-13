#!/usr/bin/env python3
"""Verify production C33 copy paths with a tiny card fixture, without archives."""
from pathlib import Path
import importlib.util
import re
import struct
import subprocess
import sys
import shlex
ROOT=Path(__file__).resolve().parents[2]
fault='--fault' in sys.argv
OUT=ROOT/('build/wr128/load-opt/copy-fault-test' if fault else 'build/wr128/load-opt/copy-test')
OUT.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('runner',ROOT/'emulator/tools/mem_dma_bench/run.py')
runner=importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
runner.STAGE=OUT
cc=ROOT/'host-tools/toolchain-c33/work/install/bin/c33-epson-elf-gcc'
flags=['-O2','-mc33pe','-mlong-calls','-std=gnu99','-fgnu89-inline','-fno-builtin']
for p in ('samo-lib/grifo/include','samo-lib/include','samo-lib/mini-libc/include','samo-lib/drivers/include','zim'):
 flags.append('-I'+str(ROOT/p))
if fault: flags.append('-DCOPY_FAULT_TEST')
objects=[]
for p in ('emulator/tools/test_zim_copy.c','zim/zim_copy.c'):
 obj=OUT/(Path(p).stem+'.o'); objects.append(obj)
 subprocess.run([str(cc),*flags,'-c',str(ROOT/p),'-o',str(obj)],check=True)
elf=OUT/'copy-test.elf'
subprocess.run([str(cc),'-nostdlib','-mc33pe',
 '-Wl,-T,'+str(ROOT/'samo-lib/grifo/lds/application.lds'),
 '-o',str(elf),*map(str,objects),str(ROOT/'samo-lib/grifo/lib/libgrifo.a'),
 str(ROOT/'samo-lib/mini-libc/lib/libc.a'),'-lgcc'],check=True)
files={n:(ROOT/'build/wr128/hsdma-tx/hsdma-boot'/n).read_bytes() for n in ('init.app','zim.ico')}
files.update({'kernel.elf':(ROOT/'samo-lib/grifo/grifo.elf').read_bytes(),'init.ini':b'zim.ico : copytest.app\n','copytest.app':elf.read_bytes()})
img=OUT/'copy-test.img'; runner.make_image(img,files)
exe=runner.build_loader()
if fault:
 source=(ROOT/'emulator/src/dma.c').read_text()
 old='struct dma *d = ctx;\n\tfor (unsigned ch = 0; ch < 4; ch++) {'
 assert source.count(old)==1
 source=source.replace(old,'struct dma *d = ctx;\n\tchannels &= ~1u; /* lost software trigger */\n\tfor (unsigned ch = 0; ch < 4; ch++) {')
 (OUT/'dma-fault.c').write_text(source)
 subprocess.run(['cc','-O2','-I'+str(ROOT/'emulator/src'),'-c',str(OUT/'dma-fault.c'),'-o',str(OUT/'dma-fault.o')],check=True)
 objects=[ROOT/'emulator/src'/f'{n}.o' for n in 'c33 mem elf uart sdcard periph lcd display touch timer wdt itc cmu port eeprom sdramc model'.split()]
 libs=shlex.split(subprocess.check_output(['pkg-config','--libs','sdl2'],text=True))
 exe=OUT/'fault-wremu'
 subprocess.run(['cc','-o',str(exe),str(OUT/'loader-main.o'),str(OUT/'dma-fault.o'),*map(str,objects),*libs],check=True)
symbols=subprocess.check_output([str(cc.parent/'c33-epson-elf-nm'),str(elf)],text=True)
def address(n): return '0x'+re.search(r'^([0-9a-f]+) [A-Za-z] '+n+'$',symbols,re.M)[1]
with (OUT/'run.txt').open('w') as out:
 subprocess.run([str(exe),'-c',str(img),'-b',address('copy_test_done'),'-D',address('copy_test_result'),'-L','16','-O',str(OUT/'result.bin'),'-n','2000000000',str(ROOT/'samo-lib/mbr/file-loader.elf')],cwd=OUT,stdout=out,stderr=subprocess.STDOUT,check=True,timeout=240)
result=struct.unpack('<4I',(OUT/'result.bin').read_bytes())
assert result[1]==0 and (result[0]==2 if fault else result[0]==708 and result[2]>0),result
if fault:
 print('PASS: lost DMA trigger times out, restores registers, copies with CPU and disables further attempts')
 sys.exit(0)
print('PASS: %d C33 copy cases, %d DMA runs / %d bytes; alignment, overlap, guards, pending-trigger/IRQ and busy-channel diagnostics, reset-flag initialization, IRQ ownership, resumption and register restoration'% (result[0],result[2],result[3]))
