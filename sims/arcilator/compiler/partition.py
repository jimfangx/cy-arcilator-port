#!/usr/bin/env python3
"""Bound CIRCT compilation work while retaining the native model interface."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def plugin(circt_opt, cxx, cache, source=None):
    source = source or Path(__file__).with_name('Partition.cpp')
    executable = circt_opt.resolve()
    include = executable.parent.parent / 'include'
    if not (include / 'mlir/Tools/Plugins/PassPlugin.h').is_file():
        raise ValueError(
            'the matching CIRCT SDK headers are required to partition hardware'
        )
    compiler = shlex.split(cxx)
    version = subprocess.check_output([executable, '--version'], text=True)
    identity = json.dumps(dict(tool=str(executable),
                               version=version,
                               size=executable.stat().st_size,
                               modified=executable.stat().st_mtime_ns,
                               compiler=compiler),
                          sort_keys=True).encode()
    # Local helper headers are part of a plugin's identity, including nested
    # includes. Retain cache identities for plugins without local headers.
    import re
    pending = [source]
    dependencies = {}
    while pending:
        unit = pending.pop()
        for name in re.findall(rb'^#include "([^"\n]+)"', unit.read_bytes(), re.M):
            header = unit.parent / name.decode()
            if header.is_file() and header not in dependencies:
                dependencies[header] = header.read_bytes()
                pending.append(header)
    headers = b''.join(str(p.relative_to(source.parent)).encode() + b'\0' + data
                       for p, data in sorted(dependencies.items()))
    key = hashlib.sha256(source.read_bytes() + headers + identity).hexdigest()
    work = cache / key
    work.mkdir(parents=True, exist_ok=True)
    library = work / (source.stem + '.so')
    # Concurrent configuration builds share an SDK-specific plugin, never a
    # partially linked library. Each SDK/source/compiler combination has its
    # own path so another build cannot replace a library about to be loaded.
    with (work / 'build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not library.is_file():
            temporary = library.with_suffix('.so.tmp')
            command = [
                *compiler, '-O2', '-std=c++17', '-fPIC', '-shared',
                '-fno-rtti', '-fno-exceptions', '-I' + str(include),
                str(source), '-o',
                str(temporary)
            ]
            print('Building CIRCT pass ' + source.name + ': ' +
                  shlex.join(command),
                  flush=True)
            # Host symbols resolve when circt-opt loads the pass. Linking a
            # second static MLIR/CIRCT copy would create conflicting TypeIDs.
            subprocess.run(command, check=True)
            temporary.replace(library)
    return library


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--circt-opt', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--max-ops', type=int, default=4096)
    parser.add_argument('--max-ports', type=int, default=16384)
    parser.add_argument('--emit-bytecode', action='store_true')
    parser.add_argument('--native-feedback', action='store_true')
    args = parser.parse_args()
    if args.max_ops <= 0 or not 0 < args.max_ports < 32768:
        parser.error(
            'use a positive operation budget and a port budget below 32768')
    try:
        library = plugin(args.circt_opt, args.cxx, args.cache)
        extra_libraries = []
        prefix = ''
        if args.native_feedback:
            feedback = plugin(
                args.circt_opt, args.cxx, args.cache,
                Path(__file__).with_name('ProceduralFeedback.cpp'))
            extra_libraries = ['--load-pass-plugin=' + str(feedback)]
            prefix = 'chipyard-bind-procedural-feedback,'
        temporary = args.output.with_name(args.output.name + '.tmp')
        pipeline = ('builtin.module(' + prefix +
                    'chipyard-partition{max-ops=' + str(args.max_ops) +
                    ' max-ports=' + str(args.max_ports) +
                    (' native-feedback=true' if args.native_feedback else '') +
                    '})')
        subprocess.run([
            str(args.circt_opt),
            str(args.input), '--load-pass-plugin=' + str(library),
            *extra_libraries, '--pass-pipeline=' + pipeline,
            *(['--emit-bytecode'] if args.emit_bytecode else []), '-o',
            str(temporary)
        ],
                       check=True)
        temporary.replace(args.output)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'CIRCT hardware partitioning: {error}\n')


if __name__ == '__main__':
    main()
