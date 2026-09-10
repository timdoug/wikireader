#!/usr/bin/env python3
"""Queue a frozen current-engine evaluation after an existing full build.

The source job and its results remain untouched. --start detaches a worker;
job.json and run.log in the new directory record its progress. No network.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def status(directory, **fields):
    path = directory/'job.json'
    record = json.loads(path.read_text()) if path.exists() else {}
    record.update(fields,updated=time.time())
    tmp = directory/'job.tmp'; tmp.write_text(json.dumps(record,indent=2)+'\n'); tmp.replace(path)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source_job',type=Path); p.add_argument('directory',type=Path)
    p.add_argument('--start',action='store_true')
    p.add_argument('--worker',action='store_true',help=argparse.SUPPRESS)
    args = p.parse_args(); directory = args.directory.resolve(); source = args.source_job.resolve()
    if not args.worker:
        if directory.exists(): p.error('use a new output directory')
        if json.loads((source/'job.json').read_text())['state'] == 'failed': p.error('source job failed')
        directory.mkdir(parents=True); bundle = directory/'frozen'; bundle.mkdir()
        for name in ('benchmark.py','title-index.py','evaluate-build.py','sparrow.c','sparrow.h','cli.c','html.c','html.h','crc32.h'):
            shutil.copy2(ROOT/name,bundle/name)
        shutil.copy2(ROOT/'build/sparrow',bundle/'sparrow')
        # The ongoing job's current binary is this revision's baseline.
        shutil.copy2(source/'frozen/sparrow',bundle/'before-cli')
        for split in ('train','test'): shutil.copy2(source/f'frozen/{split}.json',bundle/f'{split}.json')
        status(directory,state='waiting',source_job=str(source),
               frozen_sha256={f.name:sha(f) for f in bundle.iterdir()},
               policy='Untouched stored benchmark; compare engines on original index, then apply title precedence only')
        if args.start:
            with (directory/'run.log').open('ab') as log:
                child = subprocess.Popen([sys.executable,str(bundle/'evaluate-build.py'),str(source),str(directory),'--worker'],
                    stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True,close_fds=True)
            print(json.dumps({'pid':child.pid,'status':str(directory/'job.json'),'log':str(directory/'run.log')}))
            return
    bundle = directory/'frozen'
    config = json.loads((directory/'job.json').read_text())
    status(directory,pid=os.getpid())
    try:
        while True:
            state = json.loads((source/'job.json').read_text())['state']
            if state == 'complete': break
            if state == 'failed': raise RuntimeError('source build failed; inspect '+str(source/'job.json'))
            time.sleep(30)
        for name,digest in config['frozen_sha256'].items():
            if sha(bundle/name) != digest: raise RuntimeError('frozen input changed: '+name)
        status(directory,state='upgrading titles')
        subprocess.run([sys.executable,str(bundle/'title-index.py'),str(source/'sparrow.dat'),str(directory/'sparrow.dat')],check=True)
        status(directory,state='evaluating')
        for split in ('train','test'):
            for name,index,binary in [('before',source/'sparrow.dat','before-cli'),
                                      ('operators',source/'sparrow.dat','sparrow'),
                                      ('titles',directory/'sparrow.dat','sparrow')]:
                subprocess.run([sys.executable,str(bundle/'benchmark.py'),str(bundle/f'{split}.json'),str(index),
                    '--cli',str(bundle/binary),'--output',str(directory/f'{split}-{name}.json')],check=True)
        status(directory,state='complete',results={f'{s}-{v}':json.loads((directory/f'{s}-{v}.json').read_text())['counts']
               for s in ('train','test') for v in ('before','operators','titles')})
    except BaseException as error:
        status(directory,state='failed',error=str(error)); raise


if __name__ == '__main__': main()
