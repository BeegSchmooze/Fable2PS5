#!/usr/bin/env python3
"""Package the linked title into dist/PPSA99124/ (the folder ShadowMountPlus mounts).

  package.py WORK_DIR DIST_DIR [--cover COVER] [--title-name NAME]

WORK_DIR holds work/link/llvm-pie.elf and work/link/stubs/ from the link step. The folder
gets the signed eboot.bin, sce_module/libc.prx, sce_sys/param.json (title "Fable 2"),
sce_sys/icon0.png (from --cover, else the repository's placeholder) and cache-seed/ (the
shipped pipeline descriptions and the hash-only shader index).
"""
import argparse
import json
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VULKAN = Path('/work/PS5_Vulkan')
SDK = Path('/work/PS5_Fable2/.deps/native/ps5-payload-sdk')
TITLE_ID = 'PPSA99124'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work', type=Path)
    ap.add_argument('dist', type=Path)
    ap.add_argument('--cover', type=Path)
    ap.add_argument('--title-name', default='Fable 2')
    args = ap.parse_args()

    app = args.dist / TITLE_ID
    for sub in ('sce_sys', 'sce_module', 'cache-seed'):
        (app / sub).mkdir(parents=True, exist_ok=True)

    param = json.loads((REPO / 'package/param.json').read_text())
    param['titleId'] = TITLE_ID
    param['localizedParameters']['en-US']['titleName'] = args.title_name
    (app / 'sce_sys/param.json').write_text(json.dumps(param, indent=2) + '\n')

    cover = args.cover if args.cover and args.cover.exists() else None
    if cover:
        subprocess.run(['python3', str(REPO / 'tools/make_icon.py'), str(cover), str(app / 'sce_sys/icon0.png')],
                       check=True)
    else:
        shutil.copy2(REPO / 'package/icon0-placeholder.png', app / 'sce_sys/icon0.png')
        print('icon0.png: placeholder (put your own cover at game/cover.png for the real art)')

    for name in ('4D5307F1.fbo.vk.xpso', '4D5307F1.fsi.vk.xpso', '4D5307F1.shader-index.bin'):
        shutil.copy2(REPO / 'cache-seed' / name, app / 'cache-seed' / name)

    elf = args.work / 'work/link/llvm-pie.elf'
    stubs = args.work / 'work/link/stubs'
    tool = VULKAN / 'build/host/ps5-native-tool'
    unsigned = args.work / 'startup.eboot.elf'
    subprocess.run([str(tool), 'link', '--in', str(elf), '--out', str(unsigned), '--stub-dir', str(SDK / 'target/lib'),
                    '--stub', str(stubs / 'libSceAgc.so'), '--stub', str(stubs / 'libSceAgcDriver.so'),
                    '--module-sdk', '0x02000009', '--companion-sdk', '0x08050001', '--file-name', 'eboot.elf'],
                   check=True)
    subprocess.run([str(tool), 'self', '--sign', '--in', str(unsigned), '--out', str(app / 'eboot.bin'),
                    '--magic', '0x1D3D154F'], check=True)
    subprocess.run(['sha256sum', '--check', '--strict', '--quiet', 'libc.prx.sha256'], cwd=VULKAN / 'runtime',
                   check=True)
    shutil.copy2(VULKAN / 'runtime/libc.prx', app / 'sce_module/libc.prx')
    print('PACKAGED', app)


if __name__ == '__main__':
    main()
