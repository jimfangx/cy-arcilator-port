#!/usr/bin/env python3
"""Compile native LLVM models with bounded vectorization and DAG sizes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time

from partition import plugin


def available_jobs():
    return (len(os.sched_getaffinity(0))
            if hasattr(os, 'sched_getaffinity') else os.cpu_count() or 1)


def parallel_codegen(bitcode, output, llvm_opt, llc, cache, cxx, library,
                     opt_level, jobs, work):
    """Partition after optimization, restoring private bindings after linking."""
    splitter = plugin(llvm_opt, cxx, cache,
                      Path(__file__).with_name('SplitCodegen.cpp'))
    parts = work / 'parts'
    parts.mkdir()
    started = time.monotonic()
    subprocess.run([
        str(llvm_opt), '-load-pass-plugin=' + str(splitter),
        f'-passes=chipyard-split-codegen<{jobs}>', '-disable-output',
        str(bitcode)
    ],
                   check=True,
                   env={
                       **os.environ, 'CHIPYARD_CODEGEN_SPLIT_DIR': str(parts)
                   })
    manifest = json.loads((parts / 'partitions.json').read_text())
    manifest['split_seconds'] = time.monotonic() - started

    def compile_part(part):
        started = time.monotonic()
        source = parts / part['file']
        lowered = source
        obj = source.with_suffix('.o')
        # Block-address tables must be introduced after partitioning: a shared
        # table would otherwise force all its functions into the same object.
        if library is not None:
            lowered = source.with_suffix('.bounded.bc')
            subprocess.run([
                str(llvm_opt), '-load-pass-plugin=' + str(library),
                '-passes=chipyard-bound-codegen-blocks,verify',
                str(source), '-o', str(lowered)
            ], check=True)
        subprocess.run([
            str(llc), '-O' + opt_level, '-relocation-model=pic',
            '-filetype=obj',
            str(lowered), '-o',
            str(obj)
        ],
                       check=True)
        return obj, {
            **part, 'seconds': time.monotonic() - started,
            'object_bytes': obj.stat().st_size
        }

    started = time.monotonic()
    with ThreadPoolExecutor(
            max_workers=min(jobs, len(manifest['partitions']))) as pool:
        compiled = list(pool.map(compile_part, manifest['partitions']))
    manifest['codegen_seconds'] = time.monotonic() - started
    manifest['partitions'] = [entry for _, entry in compiled]
    objects = [str(obj) for obj, _ in compiled]
    started = time.monotonic()
    if len(objects) == 1 and not manifest['local_symbols']:
        os.replace(objects[0], output)
    else:
        # Ask the selected host compiler for its linker; PATH may contain a
        # different toolchain than the one used to build the native harness.
        linker = subprocess.check_output(
            [*shlex.split(cxx), '-print-prog-name=ld'], text=True).strip()
        subprocess.run([linker, '-r', '-o', str(output), *objects], check=True)
        if manifest['local_symbols']:
            names = parts / 'local-symbols.txt'
            names.write_text('\n'.join(manifest['local_symbols']) + '\n')
            objcopy = llvm_opt.parent / 'llvm-objcopy'
            if not objcopy.is_file():
                objcopy = subprocess.check_output(
                    [*shlex.split(cxx), '-print-prog-name=objcopy'],
                    text=True).strip()
            subprocess.run([
                str(objcopy), '--localize-symbols=' + str(names),
                str(output)
            ],
                           check=True)
    manifest['link_seconds'] = time.monotonic() - started
    # The names are internal implementation details, not useful in reports.
    manifest['localized_symbols'] = len(manifest.pop('local_symbols'))
    return manifest


def compile_model(source,
                  output,
                  llvm_opt,
                  llc,
                  cache,
                  cxx='c++',
                  opt_level='3',
                  llc_opt_level='3',
                  bitcode=None,
                  jobs=1,
                  report=None,
                  abi_buffers=True,
                  pipeline='bounded',
                  opt_flags='',
                  distribute_wide_stores=False,
                  vector_packing=False,
                  parallel_stock=False):
    if jobs < 1:
        raise ValueError('jobs must be positive')
    if pipeline not in ('bounded', 'stock'):
        raise ValueError('pipeline must be bounded or stock')
    extra_flags = shlex.split(opt_flags)
    bitcode = bitcode or output.with_suffix('.bc')
    paths = [path.resolve() for path in (source, output, bitcode)]
    if len(set(paths)) != 3:
        raise ValueError('input, object, and bitcode paths must be distinct')
    output.parent.mkdir(parents=True, exist_ok=True)
    bitcode.parent.mkdir(parents=True, exist_ok=True)
    requested_jobs = jobs
    if pipeline == 'stock' and not parallel_stock:
        jobs = 1
    libraries = []
    library = None
    if pipeline == 'bounded':
        library = plugin(llvm_opt, cxx, cache,
                         Path(__file__).with_name('Codegen.cpp'))
        libraries.append(library)
    # O3 argument promotion can recreate giant scalar machine arguments from
    # the private buffers introduced before optimization. Lower those ABIs
    # again before pinning instruction-selection block boundaries; blockaddress
    # uses intentionally make the later private-ABI rewrite ineligible.
    if pipeline == 'bounded':
        libraries.append(
            plugin(llvm_opt, cxx, cache,
                   Path(__file__).with_name('AggregateStores.cpp')))
    if abi_buffers:
        libraries.extend(
            plugin(llvm_opt, cxx, cache,
                   Path(__file__).with_name(name))
            for name in ('LargeReturns.cpp', 'LargeArguments.cpp'))
    if distribute_wide_stores:
        libraries.append(
            plugin(llvm_opt, cxx, cache,
                   Path(__file__).with_name('WideStores.cpp')))
    if vector_packing:
        libraries.append(
            plugin(llvm_opt, cxx, cache,
                   Path(__file__).with_name('VectorPacking.cpp')))
    # Work beside each destination so final renames do not copy large models.
    work = Path(tempfile.mkdtemp(prefix='native-codegen-', dir=output.parent))
    fd, name = tempfile.mkstemp(prefix=bitcode.name + '.',
                                suffix='.tmp',
                                dir=bitcode.parent)
    os.close(fd)
    temporary_bitcode = Path(name)
    temporary_object = work / 'model.o'
    passes = []
    if pipeline == 'bounded':
        passes += [
            'chipyard-materialize-aggregate-stores',
            'chipyard-guard-wide-vectors'
        ]
    passes.append('default<O' + opt_level + '>')
    if pipeline == 'bounded':
        passes.append('chipyard-restore-vector-flags')
    if abi_buffers:
        passes += [
            'chipyard-buffer-large-returns', 'chipyard-buffer-large-arguments'
        ]
    # Store distribution removes large PHIs after ordinary LLVM optimization.
    # Run before block boundaries or partitioning constrain CFG changes.
    if distribute_wide_stores:
        passes.append('chipyard-distribute-wide-stores')
    if vector_packing:
        passes.append('chipyard-vector-packing')
    if pipeline == 'bounded' and jobs == 1:
        passes.append('chipyard-bound-codegen-blocks')
    passes.append('verify')
    timings = {
        'jobs': jobs,
        'requested_jobs': requested_jobs,
        'pipeline': pipeline,
        'parallel_stock': parallel_stock,
        'abi_buffers': abi_buffers,
        'distribute_wide_stores': distribute_wide_stores,
        'vector_packing': vector_packing,
        'opt_flags': extra_flags
    }
    try:
        started = time.monotonic()
        subprocess.run([
            str(llvm_opt), *['-load-pass-plugin=' + str(p) for p in libraries],
            '-passes=' + ','.join(passes), *extra_flags,
            str(source), '-o',
            str(temporary_bitcode)
        ],
                       check=True)
        timings['opt_seconds'] = time.monotonic() - started
        started = time.monotonic()
        if jobs == 1:
            subprocess.run([
                str(llc), '-O' + llc_opt_level, '-relocation-model=pic',
                '-filetype=obj',
                str(temporary_bitcode), '-o',
                str(temporary_object)
            ],
                           check=True)
            timings['codegen_seconds'] = time.monotonic() - started
        else:
            timings.update(
                parallel_codegen(temporary_bitcode, temporary_object, llvm_opt,
                                 llc, cache, cxx, library, llc_opt_level, jobs,
                                 work))
    except BaseException:
        # A failed compiler must preserve the last complete model and retain
        # its failing IR/object for diagnosis; another build gets unique paths.
        print('Native LLVM compiler artifacts retained: ' +
              str(temporary_bitcode) + ' and ' + str(work),
              flush=True)
        raise
    os.replace(temporary_bitcode, bitcode)
    os.replace(temporary_object, output)
    if report is not None:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(timings, indent=2) + '\n')
    print('Native LLVM code generation: ' + json.dumps(timings), flush=True)
    shutil.rmtree(work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bitcode', type=Path)
    parser.add_argument('--llvm-opt', type=Path, required=True)
    parser.add_argument('--llc', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--no-abi-buffers',
                        dest='abi_buffers',
                        action='store_false',
                        help='retain private LLVM argument and return ABIs')
    parser.add_argument(
        '--distribute-wide-stores',
        action='store_true',
        help='distribute closed wide stores over incoming CFG edges')
    parser.add_argument(
        '--vector-packing',
        action='store_true',
        help='use target-legal vectors for pure integer packing DAGs')
    parser.add_argument('--pipeline',
                        choices=('bounded', 'stock'),
                        default='bounded',
                        help='bounded native passes or stock LLVM codegen')
    parser.add_argument(
        '--parallel-stock',
        action='store_true',
        help='compile independent optimized stock LLVM functions concurrently')
    parser.add_argument(
        '--opt-flags',
        default='',
        help='extra opt arguments, including host target/CPU controls')
    parser.add_argument(
        '--jobs',
        type=int,
        default=available_jobs(),
        help='maximum concurrent code generators (available CPUs)')
    parser.add_argument(
        '--report',
        type=Path,
        help='write optimization, partition and codegen timings')
    parser.add_argument('--opt-level',
                        choices=('0', '1', '2', '3', 's', 'z'),
                        default='3')
    parser.add_argument('--llc-opt-level',
                        choices=('0', '1', '2', '3'),
                        default='3')
    args = parser.parse_args()
    try:
        compile_model(args.input,
                      args.output,
                      args.llvm_opt,
                      args.llc,
                      args.cache,
                      args.cxx,
                      args.opt_level,
                      args.llc_opt_level,
                      args.bitcode,
                      args.jobs,
                      args.report,
                      abi_buffers=args.abi_buffers,
                      pipeline=args.pipeline,
                      opt_flags=args.opt_flags,
                      distribute_wide_stores=args.distribute_wide_stores,
                      vector_packing=args.vector_packing,
                      parallel_stock=args.parallel_stock)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Native LLVM code generation: {error}\n')


if __name__ == '__main__':
    main()
