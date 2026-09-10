#!/usr/bin/env python3
"""Replay an Ask question in a live WikiReader window, then leave it interactive.
Extracts a few unmodified HTML blobs from the local ZIM into a tiny test ZIM.
Uses the repository's FAT fixture builder and production file-loader/kernel/firmware.
Use --headless for an automated screenshot run without a window.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
STAGE=ROOT/'build/sparrow/emulator'


def progress(message):
    print(message,flush=True)


def archive(source):
    titles=['Fall of the Berlin Wall','George H. W. Bush','Albert Einstein','Ulm','Machu Picchu','Peru','Spanish','Aymara','Quechua','Ouagadougou','Burkina Faso']
    entries=[]; blobs=[]
    for title in titles:
        path=title.replace(' ','_')
        html=subprocess.check_output([str(ROOT/'host-tools/zim-reader/zimdump'),str(source),'blob','C',path])
        entries.append(('C',path,title,0,len(blobs)));blobs.append(html)
    entries.append(('M','Title','Title',1,len(blobs)));blobs.append(b'Sparrow local article test')
    entries.append(('X','listing/titleOrdered/v1','',2,len(blobs)));blobs.append(b'')
    entries.sort(key=lambda e:(e[0],e[1]))
    title_order=sorted((i for i,e in enumerate(entries) if e[0]=='C'),key=lambda i:entries[i][2])
    blobs[-1]=struct.pack('<'+'I'*len(title_order),*title_order)
    mime=b'text/html\0text/plain\0application/octet-stream\0\0'
    pp=80+len(mime);tp=pp+8*len(entries);cp=tp+4*len(entries);dp=cp+8
    dirs=bytearray();pointers=[]
    for ns,path,title,kind,blob in entries:
        pointers.append(dp+len(dirs))
        dirs+=struct.pack('<HBBIII',kind,0,ord(ns),0,0,blob)+path.encode()+b'\0'+title.encode()+b'\0'
    cluster_pos=dp+len(dirs)
    offsets=[4*(len(blobs)+1)]
    for blob in blobs: offsets.append(offsets[-1]+len(blob))
    cluster=b'\x01'+struct.pack('<'+'I'*len(offsets),*offsets)+b''.join(blobs)
    end=cluster_pos+len(cluster)
    header=struct.pack('<IHH16sIIQQQQIIQ',72173914,6,1,b'Sparrow-test-fixtur',len(entries),1,pp,tp,cp,80,0,0xffffffff,end)
    data=header+mime+struct.pack('<'+'Q'*len(pointers),*pointers)+struct.pack('<'+'I'*len(entries),*range(len(entries)))+struct.pack('<Q',cluster_pos)+dirs+cluster
    return data+hashlib.md5(data).digest()


def prepare(source,database,fresh=False):
    STAGE.mkdir(parents=True,exist_ok=True)
    progress('Preparing the demo card from local Wikipedia articles...')
    spec=importlib.util.spec_from_file_location('fixture',ROOT/'emulator/tools/mem_dma_bench/run.py')
    fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
    boot=ROOT/'ROOT_IMAGE'
    files={}
    for name in ['subtitle.bmf','subtlall.bmf','text.bmf','textall.bmf','texti.bmf','title.bmf','titleall.bmf']:
        files[name]=(boot/name).read_bytes()
    files.update({'kernel.elf':(ROOT/'samo-lib/grifo/grifo.elf').read_bytes(),
        'init.app':(ROOT/'samo-lib/grifo/applications/init/init.app').read_bytes(),
        'zim.app':(ROOT/'zim/zim.app').read_bytes(),'zim.ico':(ROOT/'zim/zim.ico').read_bytes(),
        'init.ini':b'zim.ico : zim.app\n','zim.ini':b'wiki_id=1\n',
        'sparrow.dat':database.read_bytes(),'wiki.nls':(ROOT/'XML-Licenses/en/wiki.nls').read_bytes(),
        'wiki.zim':archive(source),'zim.dir':bytes(512)})
    image=STAGE/'card.img'
    if image.exists() and not fresh:
        saved_history=fixture.read_file(image,'zim.hst')
        if saved_history:
            if len(saved_history)>256*264:
                raise RuntimeError('Existing demo history is oversized; use --fresh to discard it explicitly')
            files['zim.hst']=saved_history
            progress('Preserving History from the previous demo card.')
    fixture.make_image(image,files)
    # Expose wiki.zim/wiki.nls through the reader's existing 0:/zim directory.
    with image.open('r+b') as f:
        rootpos=(2048+32+2000)*512
        f.seek(rootpos);root=bytearray(f.read(4096))
        entries={}
        for i,name in enumerate(files): entries[name]=bytes(root[i*32:(i+1)*32])
        d=bytearray(512);d[:32]=entries['wiki.zim'];d[32:64]=entries['wiki.nls']
        entry=entries['zim.dir'];c=struct.unpack_from('<H',entry,20)[0]<<16|struct.unpack_from('<H',entry,26)[0]
        f.seek(rootpos+(c-2)*512);f.write(d)
        pos=list(files).index('zim.dir')*32
        root[pos:pos+11]=b'ZIM        ';root[pos+11]=0x10
        struct.pack_into('<I',root,pos+28,0)
        f.seek(rootpos);f.write(root)
    (STAGE/'source.json').write_text(json.dumps({'local_zim':str(source),'app_sha256':hashlib.sha256(files['zim.app']).hexdigest(),'database_sha256':hashlib.sha256(files['sparrow.dat']).hexdigest()},indent=2)+'\n')
    fixture.STAGE=STAGE
    progress('Building the emulator loader harness...')
    return image,fixture.build_loader(lcd_ready=True)


def replay(cmd,env,gui,limited,expected_taps):
    transcript=STAGE/'run.txt'
    progress('Opening the WikiReader window...' if gui else
             'Running without a window; a screenshot will be saved when finished.')
    progress(f'Emulator log: {transcript}')
    if gui:
        progress('The demo types automatically. Afterwards, click links, drag to scroll, '
                 'or press 1 for Search and 2 for History. Close the window or press Esc to finish.')
    started=time.monotonic();started_wall=time.time();last_report=started;typed=0;taps=0
    with transcript.open('w') as log, transcript.open() as reader:
        proc=subprocess.Popen(cmd,cwd=STAGE,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            while True:
                try:
                    proc.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
                for line in reader:
                    if '[anchor ' in line:
                        progress('Reader ready; replaying the scripted input...')
                    elif "[key '" in line:
                        typed+=1
                    elif '[tap down ' in line:
                        taps+=1
                        progress('Opening the answer...' if taps==1 else 'Opening the article link...')
                    elif '[tap up]' in line and taps==expected_taps:
                        progress('Scripted taps finished. The window is yours.' if gui else
                                 'Scripted taps finished; rendering the screenshot...')
                if proc.returncode is not None:
                    if proc.returncode:
                        raise RuntimeError(f'Emulator exited with status {proc.returncode}. See {transcript}')
                    break
                now=time.monotonic()
                if limited and now-started>240:
                    raise RuntimeError(f'Emulator timed out after 240 seconds. See {transcript}')
                if now-last_report>=15 and (not gui or not taps):
                    progress(f'Running ({now-started:.0f}s elapsed, {typed} keys replayed)...')
                    last_report=now
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill();proc.wait()
    screenshot=STAGE/'screen.pgm'
    if not screenshot.exists() or screenshot.stat().st_mtime<started_wall:
        raise RuntimeError(f'Emulator produced no new screenshot. See {transcript}')
    if shutil.which('sips'):
        png=STAGE/'screen.png'
        subprocess.run(['sips','-s','format','png',str(screenshot),'--out',str(png)],
                       check=True,stdout=subprocess.DEVNULL)
        screenshot=png
    progress(f'Screenshot saved: {screenshot}')


def main():
    global STAGE
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('database',type=Path)
    p.add_argument('--source',type=Path,default=ROOT/'wikipedia_en_all_maxi_2026-02.zim')
    p.add_argument('--query',default='ask who was us president when the berlin wall fell')
    mode=p.add_mutually_exclusive_group()
    mode.add_argument('--gui',dest='gui',action='store_true',help='show the live window (default)')
    mode.add_argument('--headless',dest='gui',action='store_false',help='save a screenshot without opening a window')
    p.set_defaults(gui=True)
    p.add_argument('--cycles',type=int,help='stop after this instruction budget (live window otherwise stays open)')
    p.add_argument('--tap-link',action='store_true')
    p.add_argument('--stage',type=Path,default=STAGE,help='directory for this demo card, log and screenshots')
    p.add_argument('--reuse-card',action='store_true',help='boot the existing demo card, preserving its history')
    p.add_argument('--fresh',action='store_true',help='discard the previous demo History when preparing the card')
    p.add_argument('--history',action='store_true',help='open the first saved History entry instead of typing a question')
    args=p.parse_args()
    STAGE=args.stage.resolve()
    if args.history and args.tap_link:p.error('--history and --tap-link cannot be combined')
    if args.fresh and args.reuse_card:p.error('--fresh and --reuse-card cannot be combined')
    if args.cycles is not None and args.cycles<=0:
        p.error('--cycles must be positive')
    STAGE.mkdir(parents=True,exist_ok=True)
    # A live session owns this disposable card until its window closes.
    lock=(STAGE/'session.lock').open('w')
    try:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:
        p.error('a Sparrow emulator session is already using the demo card; close it first')
    progress('Sparrow demo: reopen the first saved History entry' if args.history else f'Sparrow demo: {args.query}')
    if args.reuse_card:
        image,emulator=STAGE/'card.img',STAGE/'loader-wremu'
        if not image.exists() or not emulator.exists():p.error('--reuse-card requires a previously prepared demo')
        source=json.loads((STAGE/'source.json').read_text())
        if source['app_sha256']!=hashlib.sha256((ROOT/'zim/zim.app').read_bytes()).hexdigest():
            p.error('demo firmware differs from zim.app; run without --reuse-card to rebuild and preserve History')
        if source['database_sha256']!=hashlib.sha256(args.database.read_bytes()).hexdigest():
            p.error('demo data differs from the requested index; run without --reuse-card to rebuild and preserve History')
        progress('Reusing the demo card and its saved history...')
    else:
        image,emulator=prepare(args.source,args.database,args.fresh)
    app=(ROOT/'zim/zim.map').read_text()
    addr=re.search(r'^\s+0x([0-9a-f]+)\s+zim_startup_keyboard_ready\s*$',app,re.M)[1]
    # -K releases one key before the next. The final tap follows all typing.
    cmd=[str(emulator),'-c',str(image),'-Z','0x'+addr]
    if args.history:
        cmd+=['-N','2,3000000','-T','110,43,60000000']
    else:
        cmd+=['-K','3000000,'+args.query,'-T','110,43,450000000']
    if args.gui:cmd+=['-g','-N','3,1000000']
    cycles=args.cycles
    if cycles is None and not args.gui:cycles=850000000 if args.tap_link else 650000000
    if cycles is not None:cmd+=['-n',str(cycles)]
    if args.tap_link:cmd+=['-T','70,100,540000000']
    cmd.append(str(ROOT/'samo-lib/mbr/file-loader.elf'))
    env=dict(os.environ,WREMU_BOARD_REV='7')
    replay(cmd,env,args.gui,cycles is not None,2 if args.tap_link else 1)
    lock.close()

if __name__=='__main__':
    try:
        main()
    except KeyboardInterrupt:
        progress('\nStopped the Sparrow demo.')
        sys.exit(130)
    except (OSError,RuntimeError,subprocess.SubprocessError) as error:
        print(f'Sparrow demo: {error}',file=sys.stderr,flush=True)
        sys.exit(1)
