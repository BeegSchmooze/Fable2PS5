#!/usr/bin/env python3
"""Check that every dynamic import of the linked title is provided by the payload SDK's stub
libraries or the AGC stubs (an unbacked import fails at launch on the console).

  audit_imports.py LINK_DIR   (holding work/link/llvm-pie.elf and work/link/stubs/)
"""
from pathlib import Path
import subprocess,sys
r=Path('/work/PS5_Fable2');sdk=r/'.deps/native/ps5-payload-sdk';out=Path(sys.argv[1]);nm=str(sdk/'bin/llvm-nm')
def symbols(args):
    return subprocess.check_output([nm,*args],text=True,stderr=subprocess.DEVNULL)
needed={l.split()[-1].split('@')[0] for l in symbols(['-D','--undefined-only',str(out/'work/link/llvm-pie.elf')]).splitlines() if l.strip()}
provided=set()
for p in list((sdk/'target/lib').glob('*.so'))+list((out/'work/link/stubs').glob('*.so')):
    if p.name in ('libkernel_sys.so','libScePosixForWebKit.so'):continue
    provided.update(l.split()[-1].split('@')[0] for l in symbols(['-D','--defined-only',str(p)]).splitlines() if l.strip())
missing=sorted(needed-provided);print('Unbacked dynamic imports:',missing)
(out/'import-audit.txt').write_text('Unbacked dynamic imports: '+repr(missing)+'\n')
raise SystemExit(bool(missing))

