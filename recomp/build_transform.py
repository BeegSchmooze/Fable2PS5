#!/usr/bin/env python3
"""Maintainer tool: regenerate recomp/transform.py from transform_template.py, taking the long
inserted text blocks verbatim from the original development build scripts (so they stay byte
exact). Not needed to build the port.

  build_transform.py SCRIPTS_DIR   (the directory holding build_lock30.py ... build_menu113.py)
"""
import ast
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def literal(script, name):
    tree = ast.parse(Path(script).read_text())
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == name:
                    return ast.literal_eval(node.value)
    raise SystemExit(f'{name} not found in {script}')


def main():
    d = Path(sys.argv[1])
    values = {
        'LOCK30_HELPER': literal(d / 'build_lock30.py', 'helper'),
        'REF32_HELPER': literal(d / 'build_ref32.py', 'helper'),
        'TSC54': literal(d / 'build_tsc54.py', 'TSC'),
        'DEADLOCK62_OLD': literal(d / 'build_deadlock62.py', 'old'),
        'DEADLOCK62_NEW': literal(d / 'build_deadlock62.py', 'new'),
        'TIMER71': literal(d / 'build_bottleneck71.py', 'TIMER'),
        'MENU113_EXTRA': literal(d / 'build_menu113.py', 'patches')['31'][2],
    }
    text = (HERE / 'transform_template.py').read_text()
    for key, value in values.items():
        text = text.replace('@' + key + '@', repr(value))
    assert not any('@' + key + '@' in text for key in values), 'unfilled placeholder'
    (HERE / 'transform.py').write_text(text)
    print('wrote', HERE / 'transform.py')


if __name__ == '__main__':
    main()
