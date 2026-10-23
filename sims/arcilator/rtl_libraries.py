#!/usr/bin/env python3
"""Resolve original libraries omitted by selected Chipyard blackbox packages."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

from filelists import source_paths


ROOT = Path(__file__).resolve().parents[2]


def discover(filelist, root=ROOT):
    """Keep library models lazy: selected RTL already defines some of them.

    NVDLA's preprocessor includes RAM wrappers but omits their implementation
    and some cell models. Use the matching original release's search paths;
    concatenating these files would duplicate modules already in the wrapper.
    """
    selected = {}
    for path in source_paths(filelist):
        match = re.fullmatch(r'nvdla_(large|small)\.preprocessed\.v', path.name)
        if match:
            selected.setdefault(match[1], []).append(path)
    flags, files = [], {}
    packages = []
    for variant, sources in sorted(selected.items()):
        directory = root / 'generators/nvdla/src/main/resources/vsrc' / variant / 'vmod'
        libraries = [directory / part for part in ('rams/synth', 'rams/model', 'vlibs')]
        include = directory / 'include'
        for path in [*libraries, include]:
            if not path.is_dir():
                raise ValueError(f'selected NVDLA {variant} requires original library directory: {path}')
            for entry in sorted(path.rglob('*')):
                if entry.is_file():
                    files[str(entry)] = hashlib.sha256(entry.read_bytes()).hexdigest()
        for source in sources:
            files[str(source)] = hashlib.sha256(source.read_bytes()).hexdigest()
        for path in libraries:
            flags += ['-y', str(path)]
        flags += ['-I', str(include)]
        packages.append(dict(package='nvdla', variant=variant,
                             selected_sources=[str(p) for p in sources]))
    if packages:
        # Original libraries share the defines established by the selected
        # preprocessed design, including its original SYNTHESIS setting.
        flags += ['-Y', '.v', '-Y', '.vlib', '--libraries-inherit-macros']
    return dict(packages=packages, flags=flags, source_sha256=files)


def update(path, manifest):
    content = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    if path.is_file() and path.read_bytes() == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--filelist', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        changed = update(args.output, discover(args.filelist, args.root))
    except (OSError, ValueError) as error:
        parser.exit(1, f'native RTL libraries: {error}\n')
    if changed:
        print('Updated native RTL libraries: ' + str(args.output))


if __name__ == '__main__':
    main()
