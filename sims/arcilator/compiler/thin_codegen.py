"""Compile state partitions with the matching SDK's ThinLTO summaries.

No foreign-call attributes are invented. LLVM's own summary/import pipeline
retains cross-partition information while independent prelink/backend work can
run concurrently. Unsupported inputs use the ordinary whole-module path.
"""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

from partition import plugin


_driver_options = {}


def final_optimizer_flags(driver, flags, llvm_opt):
    """Preserve optimizer/target options in the final ThinLTO worker pipeline.

    LLVM options are not generally encoded in bitcode. In particular prelink
    TTI settings alone do not configure backend TTI or our post-optimizer
    passes. Query the matching driver, reject unsupported/control options and
    fall back before partitioning rather than silently dropping a setting.
    """
    identity = (str(driver.resolve()), driver.stat().st_size,
                driver.stat().st_mtime_ns)
    options = _driver_options.get(identity)
    if options is None:
        result = subprocess.run([str(driver), 'run', '--help-hidden'],
                                text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        if result.returncode:
            return None, 'cannot inspect matching ThinLTO driver options'
        options = {}
        for line in (result.stdout + result.stderr).splitlines():
            match = re.match(r'^\s+--([A-Za-z0-9][A-Za-z0-9_-]*)(=\S+)?\s',
                             line)
            if match:
                options[match.group(1)] = bool(match.group(2))
        _driver_options[identity] = options
    reserved = {
        'o', 'output', 'output-dir', 'input', 'inputs', 'r', 'O',
        'cg-opt-level', 'relocation-model', 'filetype', 'opt-pipeline',
        'passes', 'pass-pipeline', 'thinlto-threads', 'cache-dir',
        'save-temps', 'select-save-temps', 'module-summary', 'module-hash',
        'thinlto-bc', 'emit-summary-index', 'load', 'load-pass-plugin',
        'help', 'help-hidden', 'version', 'disable-output', 'disable-verify'
    }
    forwarded = []
    index = 0
    while index < len(flags):
        argument = flags[index]
        index += 1
        if not argument.startswith('-') or argument in ('-', '--'):
            return None, 'non-option optimizer argument needs whole-module compilation'
        name, equals, value = argument.lstrip('-').partition('=')
        if name in reserved or name.startswith(('help-', 'thinlto-', 'summary-')):
            return None, 'optimizer option controls ThinLTO input/output/pipeline: ' + name
        if name == 'mtriple':
            name = 'override-triple'
        if name not in options:
            return None, 'optimizer option unavailable in ThinLTO driver: ' + name
        if not equals and options[name]:
            if index == len(flags):
                return None, 'missing optimizer option operand: ' + name
            value = flags[index]
            index += 1
            equals = '='
        if name == 'mcpu' and value == 'native':
            # opt resolves this alias itself; llvm-lto2 forwards it literally.
            # Ask the same SDK which CPU name it recognizes on this host.
            llc = llvm_opt.resolve().with_name('llc')
            version = subprocess.run([str(llc), '--version'], text=True,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
            host = re.search(r'^\s*Host CPU:\s*(\S+)', version.stdout, re.M)
            if version.returncode or not host or host[1] in ('native', 'generic'):
                return None, 'matching SDK cannot resolve the native host CPU'
            value = host[1]
        forwarded.append('-' + name + (equals + value if equals else ''))
    return forwarded, None


def children_cpu():
    usage = os.times()
    return usage.children_user + usage.children_system


def compile_partitions(source, output, seed, llvm_opt, cache, cxx, opt_level,
                       llc_level, jobs, work, extra_flags, libraries,
                       post_passes):
    lto = llvm_opt.parent / 'llvm-lto2'
    if not lto.is_file() or opt_level not in ('0', '1', '2', '3'):
        return None, 'matching ThinLTO driver or optimization level unavailable'
    final_flags, fallback = final_optimizer_flags(lto, extra_flags, llvm_opt)
    if fallback:
        return None, fallback
    # The CIRCT translator spells redundant ABI rules explicitly. LTO requires
    # its target machine's canonical spelling even when the layouts are equal.
    # The splitter checks semantic DataLayout equality before replacing it.
    probe = subprocess.run([str(llvm_opt), '-passes=verify', *extra_flags,
                            '-S', '-o', '-', os.devnull], text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    layout = re.search(r'^target datalayout = "([^"]+)"', probe.stdout, re.M)
    if probe.returncode or not layout:
        return None, 'cannot determine the matching target data layout'
    parts = work / 'thin-parts'
    parts.mkdir()
    splitter = plugin(llvm_opt, cxx, cache,
                      Path(__file__).with_name('SplitCodegen.cpp'))
    started = time.monotonic()
    subprocess.run([str(llvm_opt), '-passes=verify', *extra_flags,
                    str(source), '-o', str(seed)], check=True)
    subprocess.run([
        str(llvm_opt), '-load-pass-plugin=' + str(splitter),
        f'-passes=chipyard-split-codegen<{jobs}>', '-disable-output', str(seed)
    ], env={**os.environ, 'CHIPYARD_CODEGEN_SPLIT_DIR': str(parts),
            'CHIPYARD_THIN_TARGET_LAYOUT': layout[1]}, check=True)
    manifest = json.loads((parts / 'partitions.json').read_text())
    if manifest.get('fallback') or len(manifest['partitions']) < 2:
        return None, manifest.get('fallback', 'fewer than two independent units')
    manifest['split_seconds'] = time.monotonic() - started
    local = set(manifest['local_symbols'])
    pipeline = 'thinlto-pre-link<O' + opt_level + '>,verify'

    def prelink(part):
        start = time.monotonic()
        target = (parts / part['file']).with_suffix('.thin.bc')
        subprocess.run([str(llvm_opt), '-passes=' + pipeline,
                        *extra_flags, '-module-summary', '-module-hash',
                        '-thinlto-bc', str(parts / part['file']), '-o',
                        str(target)], check=True)
        return target, {**part, 'prelink_seconds': time.monotonic() - start}

    started, cpu = time.monotonic(), children_cpu()
    with ThreadPoolExecutor(max_workers=min(jobs, len(manifest['partitions']))) as pool:
        compiled = list(pool.map(prelink, manifest['partitions']))
    manifest['prelink_seconds'] = time.monotonic() - started
    manifest['prelink_CPU_seconds'] = children_cpu() - cpu
    manifest['partitions'] = [row for _, row in compiled]
    inputs = [target for target, _ in compiled]
    resolutions = []
    definitions = set()
    for target in inputs:
        symbols = subprocess.run([str(lto), 'dump-symtab', str(target)],
                                 text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        if symbols.returncode:
            if 'input module has no datalayout' in symbols.stderr:
                return None, 'input lacks a concrete target data layout'
            raise subprocess.CalledProcessError(
                symbols.returncode, symbols.args, symbols.stdout,
                symbols.stderr)
        table = symbols.stdout
        for line in table.splitlines():
            found = re.fullmatch(r'([A-Z-]{8}) (.+)', line)
            if not found:
                continue
            flags, name = found.groups()
            if 'W' in flags or 'C' in flags:
                return None, 'weak/common resolution requires the final native linker'
            if any(char in name for char in ',\n\r'):
                return None, 'symbol spelling unsupported by ThinLTO resolutions'
            # dump-symtab's first column is visibility, its second is U for an
            # undefined symbol. Hidden definitions start with H, not D.
            defined = flags[1] != 'U'
            if defined:
                if name in definitions:
                    return None, 'multiple prevailing definitions after splitting'
                definitions.add(name)
            # Only symbols that were private before splitting are known local.
            # Public symbols stay externally visible and potentially preemptible.
            resolution = ('pl' if name in local else 'px') if defined else ''
            resolutions.append('-r=' + target.name + ',' + name + ',' + resolution)
    arguments = ['-O' + opt_level, '-cg-opt-level=' + llc_level,
                 '-relocation-model=pic', '-thinlto-threads=' + str(jobs),
                 *final_flags, '-o', 'native.o',
                 *[p.name for p in inputs], *resolutions]
    manifest['thinlto_opt_flags'] = final_flags
    if post_passes:
        # Keep native storage/vector/ABI passes AFTER the final LLVM optimizer,
        # as in the serial pipeline. Applying them only at prelink would let
        # the backend optimizer undo them and silently lose runtime tuning.
        arguments += ['-load-pass-plugin=' + str(p) for p in libraries]
        arguments.append('-opt-pipeline=' + ','.join([
            'thinlto<O' + opt_level + '>', *post_passes, 'verify']))
        manifest['post_optimizer_passes'] = post_passes
    # A response file avoids ARG_MAX on full models. These are LLVM arguments,
    # never shell commands; GNU response quoting preserves symbol spellings.
    response = parts / 'thinlto.rsp'
    response.write_text('\n'.join(shlex.quote(x) for x in arguments) + '\n')
    started, cpu = time.monotonic(), children_cpu()
    diagnostics = parts / 'thin-backend.log'
    with diagnostics.open('w') as stream:
        result = subprocess.run([str(lto), 'run', '@' + response.name], cwd=parts,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError('ThinLTO backend failed; see ' + str(diagnostics) +
                         ': ' + diagnostics.read_text()[-6000:])
    manifest['thin_backend_seconds'] = time.monotonic() - started
    manifest['thin_backend_CPU_seconds'] = children_cpu() - cpu
    manifest['backend_diagnostics_bytes'] = diagnostics.stat().st_size
    objects = sorted(p for p in parts.glob('native.o.*')
                     if p.suffix[1:].isdigit())
    if not objects:
        raise ValueError('ThinLTO produced no native objects')
    linker = subprocess.check_output([*shlex.split(cxx), '-print-prog-name=ld'],
                                     text=True).strip()
    started = time.monotonic()
    subprocess.run([linker, '-r', '-o', str(output), *map(str, objects)], check=True)
    if local:
        names = parts / 'local-symbols.txt'
        names.write_text('\n'.join(sorted(local)) + '\n')
        objcopy = llvm_opt.parent / 'llvm-objcopy'
        if not objcopy.is_file():
            objcopy = subprocess.check_output(
                [*shlex.split(cxx), '-print-prog-name=objcopy'], text=True).strip()
        subprocess.run([str(objcopy), '--localize-symbols=' + str(names),
                        str(output)], check=True)
    manifest['link_seconds'] = time.monotonic() - started
    manifest['optimization_strategy'] = 'thinlto-summary-parallel'
    manifest['bitcode_stage'] = 'verified-pre-partition-seed'
    manifest['localized_symbols'] = len(manifest.pop('local_symbols'))
    manifest['resolution_symbols'] = len(resolutions)
    return manifest, None
