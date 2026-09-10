#!/usr/bin/env python3
"""Download a dated compressed Wikidata JSON dump, resume, verify, never unpack.
Run --start to detach a worker. Status and logs stay beside the .part file.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


def write_status(path, data):
    tmp=path.with_suffix('.tmp')
    tmp.write_text(json.dumps(data,indent=2)+'\n')
    tmp.replace(path)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('--date',default='20260907')
    p.add_argument('--bytes',type=int,default=103048420178)
    p.add_argument('--reserve-gib',type=int,default=24)
    p.add_argument('--start',action='store_true')
    p.add_argument('--restart',action='store_true',help='restart this recorded downloader, preserving its partial file')
    args=p.parse_args()
    datetime.datetime.strptime(args.date,'%Y%m%d')
    if args.bytes <= 0 or args.reserve_gib < 1: p.error('bytes and reserve must be positive')
    args.directory.mkdir(parents=True,exist_ok=True)
    directory=args.directory.resolve()
    name=f'wikidata-{args.date}-all.json.bz2'
    target=directory/name; part=directory/(name+'.part')
    status=directory/(name+'.status.json')
    log=directory/(name+'.download.log')
    base=f'https://dumps.wikimedia.org/wikidatawiki/entities/{args.date}/'
    reserve=args.reserve_gib*1024**3
    if target.exists():
        raise SystemExit('Verified target already exists: '+str(target))
    if status.exists():
        old=json.loads(status.read_text())
        if args.restart and old.get('state') in ('downloading','verifying compressed checksum'):
            pid=old['pid']
            command=subprocess.check_output(['ps','-p',str(pid),'-o','command='],text=True).strip()
            if str(directory) not in command or not any(x in command for x in ('/wren/download.py','/sparrow/download.py')) or os.getpgid(pid)!=pid:
                raise SystemExit('Recorded process identity does not match this downloader')
            os.killpg(pid,signal.SIGTERM)
            time.sleep(1)
            old['state']='stopped for restart'
            write_status(status,old)
        if old.get('state')=='downloading':
            try:
                os.kill(old['pid'],0)
            except ProcessLookupError:
                pass
            else:
                raise SystemExit(f"Downloader already running: pid {old['pid']}")
    current=part.stat().st_size if part.exists() else 0
    if current>args.bytes or shutil.disk_usage(directory).free < args.bytes-current+reserve:
        raise SystemExit('Not enough free space for the remaining compressed download plus reserve')
    if args.start:
        with log.open('ab') as f:
            process=subprocess.Popen([sys.executable,str(Path(__file__).resolve()),str(directory),
                '--date',args.date,'--bytes',str(args.bytes),'--reserve-gib',str(args.reserve_gib)],
                stdin=subprocess.DEVNULL,stdout=f,stderr=f,start_new_session=True,close_fds=True)
        print(json.dumps({'pid':process.pid,'compressed_bytes':args.bytes,'status':str(status),'log':str(log)}))
        return
    state={'pid':os.getpid(),'state':'downloading','url':base+name,'expected_bytes':args.bytes,
           'compressed_only':True,'started':datetime.datetime.now(datetime.timezone.utc).isoformat()}
    write_status(status,state)
    def interrupted(signum, frame):
        raise SystemExit('Download interrupted; partial file retained')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    process=None
    try:
        checksums=directory/f'wikidata-{args.date}-sha1sums.txt'
        subprocess.run(['curl','-fsSL','--retry','3','--output',str(checksums),base+checksums.name],check=True)
        sums={line.split()[-1].lstrip('*'):line.split()[0] for line in checksums.read_text().splitlines() if line.strip()}
        expected=sums[name]
        state['sha1_expected']=expected
        process=subprocess.Popen(['curl','--fail','--location','--continue-at','-',
            '--retry','20','--retry-delay','30','--speed-limit','1024','--speed-time','120',
            '--output',str(part),base+name],stdin=subprocess.DEVNULL)
        while process.poll() is None:
            state.update(downloaded_bytes=part.stat().st_size if part.exists() else 0,
                         free_bytes=shutil.disk_usage(directory).free,
                         updated=datetime.datetime.now(datetime.timezone.utc).isoformat())
            write_status(status,state)
            if state['free_bytes']<reserve:
                process.terminate(); process.wait()
                raise RuntimeError('Free-space reserve reached; partial file retained for resume')
            time.sleep(10)
        if process.returncode:
            raise RuntimeError(f'curl exited {process.returncode}; partial file retained for resume')
        if part.stat().st_size!=args.bytes:
            raise RuntimeError('Unexpected compressed size')
        state['state']='verifying compressed checksum'; write_status(status,state)
        digest=hashlib.sha1()
        with part.open('rb') as f:
            for block in iter(lambda:f.read(8*1024*1024),b''):
                digest.update(block)
        if digest.hexdigest()!=expected:
            raise RuntimeError('SHA-1 mismatch; partial file retained, not published')
        part.replace(target)
        state.update(state='verified',downloaded_bytes=args.bytes,sha1=digest.hexdigest(),path=str(target))
    except BaseException as e:
        if process is not None and process.poll() is None:
            process.terminate();process.wait()
        state.update(state='stopped',error=str(e))
        raise
    finally:
        state['updated']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        write_status(status,state)

if __name__=='__main__': main()
