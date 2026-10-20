#!/usr/bin/env python3
"""Compile original C/C++ drivers with the native Arcilator runtime and ABI."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shlex
import subprocess


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
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    sim = Path(__file__).resolve().parent
    sources = [
        args.main, sim / 'native/runtime.cpp', sim / 'native/exports.cpp',
        sim / 'native/strings.cpp', sim / 'native/contexts.cpp', sim / 'native/readmem.cpp',
        sim / 'native/evaluation.cpp', sim / 'native/format.cpp',
        sim / 'native/files.cpp', sim / 'native/compat.cpp', sim / 'native/open_arrays.cpp',
        sim / 'native/bindings.cpp', sim / 'native/peripherals.cpp',
        sim / 'native/cosim.cpp', sim / 'native/spike.cpp',
        sim / 'native/analog.cpp', sim / 'native/additional.cpp'
    ]
    for token in shlex.split(args.filelist.read_text(), comments=True):
        path = Path(token)
        if path.suffix in ('.c', '.cc', '.cpp', '.cxx'):
            if not path.is_file():
                parser.error('missing foreign driver source: ' + str(path))
            if path not in sources:
                sources.append(path)
    if sim / 'native/wide.cpp' not in sources:
        sources.append(sim / 'native/wide.cpp')
    args.build.mkdir(parents=True, exist_ok=True)
    includes = ['-I' + str(sim), '-I' + str(sim / 'native/include')]
    cxxflags = shlex.split(args.cxxflags)

    def compile_source(item):
        index, source = item
        obj = args.build / f'{index}-{source.stem}.o'
        if source.suffix == '.c':
            command = shlex.split(args.cc) + [
                '-O3', '-std=gnu11'
            ] + includes + shlex.split(args.cflags)
            command += [
                flag for flag in cxxflags
                if flag.startswith(('-I', '-D', '-U'))
            ]
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
