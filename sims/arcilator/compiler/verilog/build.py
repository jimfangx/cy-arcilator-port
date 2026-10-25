#!/usr/bin/env python3
"""Build the interface-array importer against the matching installed CIRCT SDK."""
import argparse
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

CIRCT_REVISION = 'fa4f662cbe710906347fac795c581a06c6b99757'
SLANG_REVISION = '44dc55f99b9c64971893013e7931e643fbedcf23'
HERE = Path(__file__).resolve().parent
PATCHES = ('interface-arrays.patch', 'timing.patch',
           'hierarchical-queries.patch', 'port-initializers.patch',
           'port-connections.patch', 'dpi-abi.patch',
           'canonicalization-order.patch', 'unary-increment.patch',
           'unpacked-reshape.patch', 'packed-case.patch',
           'packed-ref-slices.patch', 'static-storage.patch',
           'global-identity.patch', 'dpi-exports.patch',
           'task-export-abi.patch', 'unit-static.patch', 'nonoverlap.patch',
           'unknown-comparisons.patch', 'dpi-context-scopes.patch',
           'dpi-source-hierarchy.patch', 'dpi-open-arrays.patch',
           'static-formals.patch', 'hierarchy-formats.patch', 'scope-lookups.patch',
           'realtime-f64.patch', 'hierarchical-call-captures.patch', 'inertial-delays.patch', 'parenthesized-delays.patch', 'delay-precision.patch', 'rounded-real-casts.patch', 'vpi-source-metadata.patch', 'declaration-initializers.patch')


def run(*command, **kwargs):
    print('Frontend build: ' + ' '.join(map(str, command)),
          file=sys.stderr,
          flush=True)
    subprocess.run([str(arg) for arg in command],
                   check=True,
                   stdout=sys.stderr,
                   **kwargs)


def download(url):
    print('Downloading ' + url, file=sys.stderr, flush=True)
    with urllib.request.urlopen(url, timeout=120) as stream:
        return stream.read()


def slang_source(work):
    destination = work / 'slang-source'
    if (destination / 'CMakeLists.txt').is_file():
        return destination
    archive = download(
        'https://codeload.github.com/MikePopoloski/slang/tar.gz/' +
        SLANG_REVISION)
    temporary = work / 'slang-source.tmp'
    if temporary.exists():
        shutil.rmtree(temporary)
    temporary.mkdir()
    # Extract only regular files, beneath the archive's single root directory.
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            relative = Path(*Path(member.name).parts[1:])
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('unsafe source archive path: ' + member.name)
            output = temporary / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(tar.extractfile(member).read())
    temporary.replace(destination)
    return destination


def build(args):
    executable = args.circt_verilog.resolve(strict=True)
    sdk = executable.parent.parent
    version = subprocess.check_output([executable, '--version'], text=True)
    if '\nCIRCT firtool-1.162.0\n' not in version:
        raise ValueError(
            'this source patch requires the firtool-1.162.0 CIRCT SDK')
    for relative in ('lib/libsvlang.a',
                     'include/circt/Conversion/ImportVerilog.h'):
        if not (sdk / relative).is_file():
            raise ValueError('the full matching CIRCT SDK is required: ' +
                             relative)
    identity = json.dumps(dict(tool=str(executable),
                               version=version,
                               size=executable.stat().st_size,
                               modified=executable.stat().st_mtime_ns,
                               compiler=os.environ.get('CXX',
                                                       shutil.which('c++')),
                               circt=CIRCT_REVISION,
                               slang=SLANG_REVISION),
                          sort_keys=True).encode()
    for name in ('build.py', 'CMakeLists.txt', 'sources.json', *PATCHES):
        identity += (HERE / name).read_bytes()
    work = args.cache.resolve() / hashlib.sha256(identity).hexdigest()
    work.mkdir(parents=True, exist_ok=True)
    binary = work / 'bin/arcilator-verilog'
    with (work / 'build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if binary.is_file():
            return binary
        source = work / 'source'
        source.mkdir(exist_ok=True)
        manifest = json.loads((HERE / 'sources.json').read_text())
        for relative, expected in manifest.items():
            data = ((args.circt_source /
                     relative).read_bytes() if args.circt_source else
                    download('https://raw.githubusercontent.com/llvm/circt/' +
                             CIRCT_REVISION + '/' + relative))
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(
                    'CIRCT source does not match the pinned SDK: ' + relative)
            (source / Path(relative).name).write_bytes(data)
        for name in PATCHES:
            run('patch', '--batch', '-p1', '-i', HERE / name, cwd=source)
        shutil.copyfile(HERE / 'CMakeLists.txt', source / 'CMakeLists.txt')
        slang = args.slang_source.resolve(
        ) if args.slang_source else slang_source(work)
        slang_build = args.slang_build.resolve(
        ) if args.slang_build else work / 'slang-build'
        if not args.slang_build:
            run('cmake', '-S', slang, '-B', slang_build, '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE=Release', '-DSLANG_INCLUDE_TESTS=OFF',
                '-DSLANG_INCLUDE_TOOLS=OFF', '-DSLANG_USE_THREADS=OFF',
                '-DSLANG_USE_MIMALLOC=OFF', '-DSLANG_INCLUDE_INSTALL=OFF')
            # Generate Slang headers without recompiling the SDK's Slang library.
            targets = subprocess.check_output(
                ['ninja', '-C',
                 str(slang_build), '-t', 'targets', 'all'],
                text=True)
            headers = [
                line.split(': ', 1)[0] for line in targets.splitlines()
                if line.endswith('.h: CUSTOM_COMMAND')
            ]
            if not headers:
                raise ValueError(
                    'Slang configuration did not expose generated headers')
            run('ninja', '-C', slang_build, *headers)
        run('cmake', '-S', source, '-B', work / 'build', '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DCIRCT_DIR=' + str(sdk / 'lib/cmake/circt'),
            '-DCIRCT_INCLUDE_DIR=' + str(sdk / 'include'),
            '-DSLANG_SOURCE=' + str(slang),
            '-DSLANG_BUILD=' + str(slang_build))
        run('cmake', '--build', work / 'build', '--target',
            'arcilator-verilog', '-j', args.jobs)
        binary.parent.mkdir(exist_ok=True)
        temporary = binary.with_suffix('.tmp')
        shutil.copyfile(work / 'build/arcilator-verilog', temporary)
        temporary.chmod(0o755)
        temporary.replace(binary)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-verilog', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--circt-source',
                        type=Path,
                        help='use an existing checkout at the pinned revision')
    parser.add_argument(
        '--slang-source',
        type=Path,
        help='use an existing Slang checkout at the pinned revision')
    parser.add_argument('--slang-build',
                        type=Path,
                        help='reuse matching generated Slang headers')
    parser.add_argument('--jobs', type=int,
                        default=(len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity')
                                 else os.cpu_count() or 1),
                        help='frontend compilation jobs (default: available CPUs)')
    args = parser.parse_args()
    if args.jobs < 1 or (args.slang_build and not args.slang_source):
        parser.error(
            'use positive --jobs; --slang-build also requires --slang-source')
    try:
        print(build(args))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'CIRCT frontend build: {error}\n')


if __name__ == '__main__':
    main()
