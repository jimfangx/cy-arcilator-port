#!/usr/bin/env python3
"""Compile original C/C++ drivers with the native Arcilator runtime and ABI."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import shlex
import subprocess

from filelists import source_paths


def c_preprocessor_flags(flags):
    """Keep shared -I/-D/-U options with joined or separate operands."""
    result = []
    tokens = iter(flags)
    for flag in tokens:
        if flag in ('-I', '-D', '-U'):
            try:
                operand = next(tokens)
            except StopIteration as error:
                raise ValueError(
                    f'shared C preprocessing option {flag} requires an operand'
                ) from error
            result.extend((flag, operand))
        elif flag.startswith(('-I', '-D', '-U')):
            result.append(flag)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--filelist', required=True, type=Path)
    parser.add_argument('--main', required=True, type=Path)
    parser.add_argument('--model', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxxflags', default='')
    parser.add_argument('--cflags', default='')
    parser.add_argument('--ldflags', default='')
    parser.add_argument('--jobs', type=int,
                        default=(len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity')
                                 else os.cpu_count() or 1),
                        help='parallel C/C++ compilations (default: available CPUs)')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    sim = Path(__file__).resolve().parent
    sources = [
        sim / 'native/vpi_lifecycle.cpp', args.main,
        sim / 'native/runtime.cpp', sim / 'native/exports.cpp',
        sim / 'native/strings.cpp', sim / 'native/contexts.cpp', sim / 'native/readmem.cpp',
        sim / 'native/evaluation.cpp', sim / 'native/format.cpp',
        sim / 'native/files.cpp', sim / 'native/compat.cpp', sim / 'native/open_arrays.cpp',
        sim / 'native/bindings.cpp', sim / 'native/peripherals.cpp',
        sim / 'native/cosim.cpp', sim / 'native/spike.cpp',
        sim / 'native/analog.cpp', sim / 'native/analog_nets.cpp',
        sim / 'native/additional.cpp'
    ]
    selected_sources = {path.resolve() for path in sources}
    try:
        foreign_sources = list(source_paths(args.filelist))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    for path in foreign_sources:
        if path.suffix in ('.c', '.cc', '.cpp', '.cxx'):
            if not path.is_file():
                parser.error('missing foreign driver source: ' + str(path))
            identity = path.resolve()
            if identity not in selected_sources:
                sources.append(path)
                selected_sources.add(identity)
    if (sim / 'native/wide.cpp').resolve() not in selected_sources:
        sources.append(sim / 'native/wide.cpp')
    includes = ['-I' + str(sim), '-I' + str(sim / 'native/include')]
    cxxflags = shlex.split(args.cxxflags)
    try:
        shared_c_flags = c_preprocessor_flags(cxxflags)
    except ValueError as error:
        parser.error(str(error))
    args.build.mkdir(parents=True, exist_ok=True)

    def compile_source(item):
        index, source = item
        obj = args.build / f'{index}-{source.stem}.o'
        if source.suffix == '.c':
            command = shlex.split(args.cc) + [
                '-O3', '-std=gnu11'
            ] + includes + shlex.split(args.cflags)
            command += shared_c_flags
        else:
            command = shlex.split(args.cxx) + ['-O3', '-std=c++17'
                                               ] + includes + cxxflags
        if source == sim / 'native/files.cpp':
            command.append(
                '-fno-rtti')  # Match the LLVM SDK stream base class.
        command += ['-c', str(source), '-o', str(obj)]
        print(shlex.join(command), flush=True)
        subprocess.run(command, check=True)
        return obj

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        objects = list(pool.map(compile_source, enumerate(sources)))
    temporary = args.output.with_name(args.output.name + '.tmp')
    command = shlex.split(args.cxx) + [str(p) for p in objects] + [
        str(args.model)
    ] + shlex.split(args.ldflags) + ['-o', str(temporary)]
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    temporary.replace(args.output)


if __name__ == '__main__':
    main()
