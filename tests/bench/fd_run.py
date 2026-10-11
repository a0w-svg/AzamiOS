#!/usr/bin/env python3
"""Run fixed-count pinned eventfd workers using an immutable kernel image."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
REPO=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(REPO/'tests/semantics'))
from namespaces import digest, execute, save, source_state


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel',type=Path,default=REPO/'build/kernel.elf')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--kernel-source-manifest',type=Path)
    args=parser.parse_args();work=args.output.resolve();kernel=args.kernel.resolve()
    if not work.is_relative_to((REPO/'build').resolve()):parser.error('output must be new under build/')
    work.mkdir(parents=True,exist_ok=False)
    payload=REPO/'build/bench/bin/bench_fd'
    manifest={'started_utc':datetime.now(timezone.utc).isoformat(),'status':'running',
              'kernel_sha256':digest(kernel),'payload_sha256':digest(payload),
              'worktree_at_launch':source_state(),'source_sha256':digest(REPO/'tests/bench/bench_fd.c'),
              'flags':'-static -std=c11 -O2 -g -Wall -Wextra -Werror -pthread',
              'config':{'smp':4,'ram':'1536M','machine':'q35','iterations_per_worker':20000,
                        'warmups':1,'repetitions':10},
              'accelerator':'kvm' if os.access('/dev/kvm',os.R_OK|os.W_OK) else 'tcg'}
    if args.kernel_source_manifest:
        origin=json.loads(args.kernel_source_manifest.read_text())
        if origin['kernel_sha256']!=manifest['kernel_sha256']:
            parser.error('source manifest does not match selected kernel')
        manifest['kernel_source']=origin['source']
        manifest['kernel_source_manifest_sha256']=digest(args.kernel_source_manifest)
    save(work/'manifest.json',manifest)
    try:
        cmd=[str(REPO/'scripts/linux-test.sh'),str(payload)]
        env=dict(os.environ,LINUX_TEST_WORK=str(work/'vm'),LINUX_TEST_TIMEOUT='150',
                 LINUX_TEST_KERNEL=str(kernel),LINUX_TEST_SMP='4')
        execute(cmd,work/'guest.txt',180,env)
        text=(work/'guest.txt').read_text(errors='replace')
        rows=[json.loads(s[8:]) for s in text.splitlines() if s.startswith('FDBENCH ')]
        if len(rows)!=99 or '== probe complete: 99 passed, 0 failed ==' not in text:
            raise ValueError('missing complete FD benchmark')
        summary={}
        for mode,n in [('one-thread',1),('four-threads',4),('four-processes',4)]:
            samples=[];rounds=[]
            for rep in range(-1,10):
                group=[r for r in rows if r['mode']==mode and r['repetition']==rep]
                if (len(group)!=n or {r['worker'] for r in group}!=set(range(n)) or
                    len({r['cpu'] for r in group})!=n or
                    any(r['passed'] is not True or type(r['elapsed_ns']) is not int or r['elapsed_ns']<=0 or
                        r['iterations']!=20000 or r['checksum']!=20000 for r in group)):
                    raise ValueError('corrupt worker/count/CPU evidence')
                if rep>=0:
                    # Independently timed workers: this is slowest-worker latency,
                    # not group makespan or measured aggregate throughput.
                    elapsed=max(r['elapsed_ns'] for r in group)
                    samples.append(elapsed/20000)
                    rounds.append({'elapsed_ns':elapsed,'ns_per_worker_pair':elapsed/20000})
            median=statistics.median(samples)
            summary[mode]={'median_ns_per_worker_pair':median,'min':min(samples),'max':max(samples),
                           'mad':statistics.median(abs(x-median) for x in samples),'rounds':rounds}
        save(work/'observations.json',{'rows':rows,'summary':summary})
        manifest.update(status='passed',iso_sha256=digest(work/'vm/test.iso'))
        print(json.dumps(summary,indent=2));print('PASS:',work)
    except (OSError,ValueError,KeyError,subprocess.SubprocessError) as error:
        manifest.update(status='failed',error=str(error));print('FAIL:',error,file=sys.stderr);return 1
    finally:
        manifest['finished_utc']=datetime.now(timezone.utc).isoformat();save(work/'manifest.json',manifest)
    return 0

if __name__=='__main__':sys.exit(main())
