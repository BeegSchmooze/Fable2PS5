#!/usr/bin/env python3
"""Maintainer tool: export the build recipe, the overlay and the ReXGlue patch from a development
workspace (/work as used to develop the port). Not needed to build the port.

  export_repo.py OUT_DIR [LINK_BATCH=170]

Inputs (made while verifying the recipe):
  /tmp/recipe_got.json  recorded compile command for every object of build 169
  /tmp/deps169.json     every file those compiles read (clang -M)
Writes into OUT_DIR:
  recipe/compile.json        one entry per object: output, argv, cwd, LLVM_CONFIG, kind
  recipe/link-inputs.txt     the link order of build LINK_BATCH
  recipe/recomp-units.json   recompiled unit -> the source path the recipe compiles it from
  overlay/...                the port's own source files at their /work-relative paths
  patches/rexglue-sdk.patch  ReXGlueSDK changes on top of the pinned commit (new files included)
Game-derived files (the generated and transformed recompiled code) are never exported.
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

OUT = Path(sys.argv[1])
LINK_BATCH = int(sys.argv[2]) if len(sys.argv) > 2 else 170
SWEEP = Path('/work/PS5_Fable2/build/ps5/fable2-link-sweep')
PORT_OBJECTS = {
    str(SWEEP / f'provider-batch{LINK_BATCH}/objects/game_boot.cpp.o'): '/work/fable2-ps5/port/game_boot.cpp',
    str(SWEEP / f'provider-batch{LINK_BATCH}/objects/cache_seed.cpp.o'): '/work/fable2-ps5/port/cache_seed.cpp',
}
BOOT_TEMPLATE = '/work/PS5_Fable2/build/ps5/rex-batch157-boot-objects/game_boot.cpp.o'


def game_derived(path):
    name = Path(path).name
    return '/Fable2Recomp/generated/' in path or name.startswith('fable_2_recomp.')


def main():
    got = json.load(open('/tmp/recipe_got.json'))
    deps = json.load(open('/tmp/deps169.json'))
    links = (SWEEP / f'provider-batch{LINK_BATCH}/link-inputs.txt').read_text().split()

    entries, units = [], {}
    for obj in links:
        if not obj.endswith('.o'):
            continue
        if obj in PORT_OBJECTS:
            base = got[BOOT_TEMPLATE]
            argv = list(base['argv'])
            argv[argv.index('-c') + 1] = PORT_OBJECTS[obj]
            argv[argv.index('-o') + 1] = obj
            if obj.endswith('cache_seed.cpp.o'):
                argv[argv.index('-O0')] = '-O2'
            entries.append({'output': obj, 'argv': argv, 'cwd': base['cwd'], 'kind': 'port'})
            continue
        r = got[obj]
        source = r['argv'][r['argv'].index('-c') + 1]
        kind = 'recomp' if game_derived(source) else 'source'
        if kind == 'recomp':
            units[Path(source).name] = source
        e = {'output': obj, 'argv': r['argv'], 'cwd': r['cwd'], 'kind': kind}
        if r.get('llvm_config'):
            e['llvm_config'] = r['llvm_config']
        entries.append(e)
    (OUT / 'recipe').mkdir(parents=True, exist_ok=True)
    (OUT / 'recipe/compile.json').write_text(json.dumps(entries, indent=1) + '\n')
    (OUT / 'recipe/link-inputs.txt').write_text('\n'.join(links) + '\n')
    (OUT / 'recipe/recomp-units.json').write_text(json.dumps(dict(sorted(units.items())), indent=1) + '\n')
    print('compile entries', len(entries), 'recomp units', len(units))

    # Overlay: the port's own files (PS5_Fable2 tree) that the compiles and the link read.
    extra = [
        SWEEP / 'full-game-link/samples.txt',
        SWEEP / 'full-game-link/link/ps5-pie-weak.ld',
        SWEEP / 'provider-batch11/link-stage0.sh',
        Path('/work/PS5_Fable2/ps5_fiber_test/sce_sys/param.json'),
        Path('/work/PS5_Fable2/ps5/tools/setup-sdk.sh'),
        Path('/work/ReXGlueSDK/build-host-codegen19/include/rex/version.h'),
    ]
    files = [Path(f) for f in deps if f.startswith('/work/PS5_Fable2/') and '/.deps/' not in f
             and Path(f).is_file() and not game_derived(f)] + extra
    overlay = OUT / 'overlay'
    if overlay.exists():
        shutil.rmtree(overlay)
    for f in files:
        rel = f.relative_to('/work') if str(f).startswith('/work/') else Path('tools-dev') / f.name
        dst = overlay / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, dst)
    print('overlay files', len(files))

    # ReXGlueSDK: tracked changes plus new files, skipping backups and build trees.
    sdk = Path('/work/ReXGlueSDK')
    tracked = subprocess.run(['git', '-C', str(sdk), 'diff', 'HEAD', '--binary'], capture_output=True, text=True, check=True).stdout
    new = subprocess.run(['git', '-C', str(sdk), 'ls-files', '--others', '--exclude-standard'], capture_output=True,
                         text=True, check=True).stdout.split()
    new = [n for n in new if '.before-' not in n and not n.startswith(('build-', 'out/')) and not n.endswith('.toml.before-codegen')]
    patch = tracked
    for n in sorted(new):
        p = subprocess.run(['git', '-C', str(sdk), 'diff', '--no-index', '--binary', '/dev/null', n], capture_output=True, text=True)
        patch += p.stdout
    (OUT / 'patches').mkdir(parents=True, exist_ok=True)
    (OUT / 'patches/rexglue-sdk.patch').write_text(patch)
    print('rexglue patch: tracked changes +', len(new), 'new files:', ', '.join(new))


if __name__ == '__main__':
    main()
