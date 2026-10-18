#!/usr/bin/env python3
"""Build Fable 2 for PS5 from your own default.xex (runs inside the Docker image).

  build_port.py [--game /game] [--out /out] [--jobs N] [--skip-xex-check]

1. codegen   rexglue recompiles /game/default.xex into C++ (Fable2Recomp/generated/default)
2. transform this port's changes to the recompiled code (recomp/transform.py)
3. compile   every object with its recorded command (recipe/compile.json), in parallel
4. link      PS5_Vulkan's link recipe with RADV, in the recorded order (recipe/link-inputs.txt)
5. package   out/PPSA99124: eboot.bin, sce_sys (title "Fable 2", icon), sce_module, cache-seed
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
W = Path('/work')
RECOMP = W / 'Fable2Recomp'
GENERATED = RECOMP / 'generated/default'
SDK = W / 'PS5_Fable2/.deps/native/ps5-payload-sdk'
LINK_WORK = W / 'PS5_Fable2/build/ps5/fable2-ps5-link'
SWEEP = W / 'PS5_Fable2/build/ps5/fable2-link-sweep'


def step(title):
    print(f'\n==> {title}', flush=True)


def xex_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def pinned(name):
    for line in (REPO / 'docker/pins.sh').read_text().splitlines():
        if line.startswith(name + '='):
            return line.split('=', 1)[1].split()[0]
    raise SystemExit(f'{name} missing from docker/pins.sh')


def compile_one(entry):
    out = Path(entry['output'])
    out.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PS5_PAYLOAD_SDK=str(SDK))
    if entry.get('llvm_config'):
        env['LLVM_CONFIG'] = entry['llvm_config']
    else:
        env.pop('LLVM_CONFIG', None)
    r = subprocess.run(entry['argv'], cwd=entry['cwd'], env=env, capture_output=True, text=True)
    return entry, r.returncode, r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', type=Path, default=Path('/game'))
    ap.add_argument('--out', type=Path, default=Path('/out'))
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--skip-xex-check', action='store_true')
    args = ap.parse_args()
    started = time.time()

    xex = args.game / 'default.xex'
    if not xex.is_file():
        raise SystemExit(f'{xex} not found: put your own default.xex in the game/ folder (README.md, "Your game files").')
    step('checking default.xex')
    digest = xex_sha256(xex)
    if digest != pinned('XEX_SHA256'):
        msg = (f'default.xex SHA-256 is {digest}\nexpected                {pinned("XEX_SHA256")}\n'
               'This port was made from one specific build of the game (title 4D5307F1, media 716F0A0D, '
               'v0.0.0.26); the recompiled code and the patches only fit that build.')
        if not args.skip_xex_check:
            raise SystemExit(msg + '\n(--skip-xex-check builds anyway; expect the transform to stop.)')
        print('WARNING:', msg)
    print('default.xex OK')

    step('codegen (rexglue, about a minute)')
    shutil.copy2(xex, RECOMP / 'default.xex')
    subprocess.run([str(W / 'ReXGlueSDK/out/linux-amd64/rexglue'), '-f', 'codegen', 'fable_2_manifest.toml'],
                   cwd=RECOMP, check=True)

    step('transform')
    transformed = W / 'PS5_Fable2/build/ps5/fable2-ps5-recomp'
    shutil.rmtree(transformed, ignore_errors=True)
    subprocess.run([sys.executable, str(REPO / 'recomp/transform.py'), str(GENERATED), str(transformed)], check=True)
    units = json.loads((REPO / 'recipe/recomp-units.json').read_text())
    for name, path in units.items():
        path = Path(path)
        source = transformed / name if (transformed / name).exists() else GENERATED / name
        if path != source:
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, path)

    entries = json.loads((REPO / 'recipe/compile.json').read_text())
    step(f'compile ({len(entries)} objects, {args.jobs} jobs; the recompiled game code takes most of the time)')
    failed = []
    done = 0
    with ThreadPoolExecutor(args.jobs) as pool:
        # Biggest first: the recompiled units dominate.
        order = sorted(entries, key=lambda e: e['kind'] != 'recomp')
        for entry, code, err in pool.map(compile_one, order):
            done += 1
            if code:
                failed.append((entry['output'], err))
                print(f'FAILED {entry["output"]}\n{err[-2000:]}', flush=True)
            if done % 50 == 0 or done == len(entries):
                print(f'  {done}/{len(entries)} ({time.time() - started:.0f} s)', flush=True)
    if failed:
        raise SystemExit(f'{len(failed)} objects failed to compile')

    step('link')
    inputs = (REPO / 'recipe/link-inputs.txt').read_text().split()
    (LINK_WORK / 'work/link').mkdir(parents=True, exist_ok=True)
    shutil.copy2(SWEEP / 'full-game-link/samples.txt', LINK_WORK / 'work/samples.txt')
    shutil.copy2(SWEEP / 'full-game-link/link/ps5-pie-weak.ld', LINK_WORK / 'work/link/ps5-pie-weak.ld')
    env = dict(os.environ, LLVM_CONFIG='/usr/bin/llvm-config-18', PS5_PAYLOAD_SDK=str(SDK))
    with open(LINK_WORK / 'link.log', 'w') as log:
        r = subprocess.run(['bash', str(SWEEP / 'provider-batch11/link-stage0.sh'), str(LINK_WORK / 'work'),
                            str(W / 'PS5_Vulkan'), str(SDK), *inputs], env=env, stdout=log, stderr=subprocess.STDOUT)
    if r.returncode:
        print((LINK_WORK / 'link.log').read_text()[-4000:])
        raise SystemExit('link failed')

    step('package')
    cover = args.game / 'cover.png'
    cmd = [sys.executable, str(REPO / 'tools/package.py'), str(LINK_WORK), str(args.out)]
    if cover.is_file():
        cmd += ['--cover', str(cover)]
    subprocess.run(cmd, check=True, env=env)
    subprocess.run([sys.executable, str(REPO / 'tools/audit_imports.py'), str(LINK_WORK)], check=True)
    eboot = args.out / 'PPSA99124/eboot.bin'
    print(f'\nDone in {(time.time() - started) / 60:.0f} min: {args.out / "PPSA99124"} '
          f'(eboot.bin sha256 {xex_sha256(eboot)[:16]}...)')


if __name__ == '__main__':
    main()
