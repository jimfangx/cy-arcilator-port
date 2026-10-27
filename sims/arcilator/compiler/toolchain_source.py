#!/usr/bin/env python3
"""Prepare authenticated, patched sources for a local native compiler build.

Only source bytes are downloaded. Installed SDK libraries are borrowed locally;
no compiler binaries, profiles or simulator results are transferred between hosts.
The supported SDK is the reviewed Linux x86-64 firtool-1.162.0 release (including
build-setup.sh's Z3 relocation). Unknown SDKs fail closed, even at version 24.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

import full_toolchain as ft

HERE = Path(__file__).resolve().parent
LLVM_PIN = 'a17069fea5b271c6824ea773874b007116de9644'
CIRCT_PIN = 'fa4f662cbe710906347fac795c581a06c6b99757'
LLVM_ARCHIVE_SHA256 = '78b0b128502234b2d46270da291fd4c7fbad7eea7a02a72a5a691b7ece3d82b1'
# Reviewed release fingerprints: names + bytes, independent of installation path.
# CMake exports are deliberately inventoried separately: build-setup rewrites Z3.
SDK_RELEASE_SHA256 = 'aa77705473de76b094eb06d6e7148a446bb4394e44fe7b4cffe08b25cbd4b1a6'
SDK_GENERATED_HEADERS_SHA256 = '7619b8599fa2e792ade552837144adfdae37fb23bd9e784e52d2919e1de36d68'
COMPONENTS = ft.COMPONENTS


def command(argv, log, cwd=None):
    """Use explicit argv and retain the command and its complete output."""
    argv = [str(a) for a in argv]
    with Path(log).open('a') as stream:
        stream.write(json.dumps(dict(argv=argv, cwd=str(cwd) if cwd else None)) + '\n')
        stream.flush()
        subprocess.run(argv, cwd=cwd, env=ft.clean_environment(), stdout=stream,
                       stderr=subprocess.STDOUT, check=True)


def download(url, destination, digest):
    destination = Path(destination)
    ft.require(not destination.exists(), 'Download destination must be fresh: ' + str(destination))
    temporary = destination.with_suffix(destination.suffix + '.partial')
    request = urllib.request.Request(url, headers={'User-Agent': 'Chipyard-native-toolchain-source'})
    with urllib.request.urlopen(request, timeout=120) as response, temporary.open('xb') as stream:
        shutil.copyfileobj(response, stream, 1 << 20)
    ft.checked_file(temporary, digest)
    temporary.rename(destination)
    return destination


def selected(relative):
    return len(relative.parts) == 1 or relative.parts[0] in COMPONENTS


def extract_archive(archive, destination, supplied=None):
    """Extract only source components/root files from the authenticated archive.

    An optional non-git source directory is compared against every archive entry
    and copied instead. Missing, extra, changed, escaping or object files fail.
    """
    ft.checked_file(archive, LLVM_ARCHIVE_SHA256)
    prefix = 'llvm-project-' + LLVM_PIN
    expected = set()
    destination.mkdir()
    with tarfile.open(archive, 'r:gz') as contents:
        for member in contents:
            path = Path(member.name)
            ft.require(path.parts and path.parts[0] == prefix and '..' not in path.parts,
                       'Unexpected archive path: ' + member.name)
            relative = Path(*path.parts[1:])
            if not relative.parts or not selected(relative) or member.isdir():
                continue
            ft.require(member.isfile() or member.issym(), 'Unsupported source archive entry: ' + str(relative))
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            expected.add(str(relative))
            if member.issym():
                link = member.linkname
                ft.require(not Path(link).is_absolute() and
                           (target.parent / link).resolve().is_relative_to(destination.resolve()),
                           'Source link escapes selected tree: ' + str(relative))
                if supplied:
                    original = supplied / relative
                    ft.require(original.is_symlink() and os.readlink(original) == link,
                               'Pristine source link differs: ' + str(relative))
                target.symlink_to(link)
            else:
                data = contents.extractfile(member).read()
                if supplied:
                    original = supplied / relative
                    ft.require(original.is_file() and not original.is_symlink() and original.read_bytes() == data,
                               'Pristine source differs from pinned archive: ' + str(relative))
                    shutil.copyfile(original, target)
                else:
                    target.write_bytes(data)
                target.chmod(member.mode & 0o777)
    if supplied:
        actual = {str(p.relative_to(supplied)) for p in supplied.rglob('*')
                  if (p.is_file() or p.is_symlink()) and selected(p.relative_to(supplied))
                  and '.git' not in p.relative_to(supplied).parts}
        ft.require(actual == expected, 'Pristine source has missing/extra files: ' +
                   repr(sorted(actual ^ expected)[:20]))
    return dict(kind='sha256-authenticated-git-source-archive', revision=LLVM_PIN,
                archive=ft.checked_file(archive), source_input=str(supplied) if supplied else None)


def prepare_source(work, source=None):
    work = Path(work).resolve()
    destination = work / 'source'
    ft.require(not destination.exists(), 'Prepared source must be fresh')
    source = Path(source).resolve() if source else None
    # Git commit objects authenticate all tracked bytes; do not borrow its build
    # directories or objects. A clean checkout is required instead of silently
    # ignoring caller edits. Non-git source directories use archive comparison.
    if source and source.is_dir() and (source / '.git').exists():
        query = lambda *a: subprocess.check_output(['git', '-C', str(source), *a], text=True).strip()
        ft.require(query('rev-parse', 'HEAD') == LLVM_PIN, 'Pristine checkout is not the pinned LLVM commit')
        ft.require(not query('status', '--porcelain', '--untracked-files=all'), 'Pristine checkout is dirty')
        names = query('ls-tree', '--name-only', LLVM_PIN).splitlines()
        roots = [name for name in names if name in COMPONENTS or
                 query('cat-file', '-t', LLVM_PIN + ':' + name) == 'blob']
        archive = work / 'git-source.tar'
        with archive.open('xb') as stream:
            subprocess.run(['git', '-C', str(source), 'archive', LLVM_PIN, '--', *roots],
                           stdout=stream, check=True, env=ft.clean_environment())
        destination.mkdir()
        # Git objects are trusted paths from the pinned commit; still constrain
        # extraction to the chosen source tree with Python's data filter.
        with tarfile.open(archive) as contents:
            for member in contents:
                path = Path(member.name)
                ft.require(not path.is_absolute() and '..' not in path.parts and selected(path),
                           'Unsafe git archive path')
            contents.extractall(destination, filter='data')
        provenance = dict(kind='pinned-git-commit', revision=LLVM_PIN,
                          source_input=str(source), archive=ft.checked_file(archive))
    else:
        archive = source if source and source.is_file() else work / 'llvm-project.tar.gz'
        if not archive.exists():
            download('https://codeload.github.com/llvm/llvm-project/tar.gz/' + LLVM_PIN,
                     archive, LLVM_ARCHIVE_SHA256)
        provenance = extract_archive(archive, destination, source if source and source.is_dir() else None)
    manifest_path = HERE / 'llvm-patches/manifest.json'
    manifest = ft.load(manifest_path)
    ft.require(manifest['revision'] == LLVM_PIN and manifest['circt_revision'] == CIRCT_PIN,
               'Patch manifest source pins differ')
    ft.require(len(manifest['patches']) == 13, 'Expected the complete reviewed13-patch series')
    for relative, digest in manifest['original_sources'].items():
        ft.checked_file(destination / relative, digest)
    original_headers = {str(p.relative_to(destination)): ft.sha(p)
                        for root in ('llvm/include/llvm', 'llvm/include/llvm-c')
                        for p in (destination / root).rglob('*') if p.is_file()}
    patches = []
    for entry in manifest['patches']:
        patch = HERE / 'llvm-patches' / entry['file']
        patches.append(ft.checked_file(patch, entry['sha256']))
        command(['git', 'apply', '--check', patch], work / 'patch.log', cwd=destination)
        command(['git', 'apply', patch], work / 'patch.log', cwd=destination)
    final = {}
    for key in ('patched_cpp_sources', 'patched_header_sources', 'patched_build_support_sources',
                'patched_private_header_sources'):
        for relative, digest in manifest[key].items():
            ft.checked_file(destination / relative, digest)
            final[relative] = digest
    inventory = ft.source_inventory(destination)
    provenance.update(manifest=ft.checked_file(manifest_path), patches=patches,
                      final_reviewed_sources=final, original_public_headers=original_headers,
                      final_source_tree_sha256=inventory['tree_sha256'])
    ft.dump(work / 'source-provenance.json', provenance)
    ft.dump(work / 'source-inventory.json', inventory)
    return destination, provenance, inventory


def prepare_sdk(work, sdk, source, provenance):
    """Prove the reviewed source/header ABI relation before creating a record."""
    work, sdk = Path(work), Path(sdk).resolve()
    # Shared RISCV prefixes are supported. full_toolchain deliberately rejects
    # directory symlinks, so materialize them locally if the installed layout
    # contains them. Never rewrite the installed environment.
    directory_links = [p for name in ('bin', 'include', 'lib') for p in [sdk / name, *(sdk / name).rglob('*')]
                       if p.is_symlink() and p.is_dir()]
    original_sdk = sdk
    if directory_links:
        sdk = work / 'sdk'
        sdk.mkdir()
        for name in ('bin', 'include', 'lib'):
            shutil.copytree(original_sdk / name, sdk / name, symlinks=False)
    inventory = ft.sdk_inventory(sdk)
    release = {k: v for k, v in inventory['files'].items()
               if k in ('bin/opt', 'bin/llc', 'bin/firtool', 'bin/llvm-config') or
               (k.startswith('lib/lib') and Path(k).name.startswith(('libLLVM', 'libMLIR', 'libCIRCT')))}
    ft.require(ft.fingerprint(release) == SDK_RELEASE_SHA256,
               'SDK compiler/library bytes are not the reviewed firtool-1.162.0 Linux x86-64 release')
    config = ft.sdk_configuration(sdk)
    ft.require(LLVM_PIN in (sdk / 'include/llvm/Support/VCSRevision.h').read_text(),
               'SDK LLVM source revision differs')
    ft.require(config['version'] == '24.0.0git' and config['cxx11_abi'] == 1 and
               config['assertion_mode'] == 'OFF' and not config['rtti'] and not config['exceptions'],
               'Unsupported SDK ABI configuration')
    version = subprocess.check_output([str(sdk / 'bin/firtool'), '--version'], text=True)
    ft.require('CIRCT firtool-1.162.0\n' in version + '\n', 'SDK CIRCT release differs')
    installed, generated, differences = {}, {}, {}
    originals = provenance['original_public_headers']
    allowed = ft.load(HERE / 'llvm-patches/manifest.json')['patched_header_sources']
    ft.require(set(allowed) == {'llvm/include/llvm/CodeGen/MachineRegisterInfo.h',
                               'llvm/include/llvm/CodeGen/LiveInterval.h'},
               'Unreviewed public-header patch set')
    for relative, digest in inventory['files'].items():
        if not relative.startswith(('include/llvm/', 'include/llvm-c/')):
            continue
        source_relative = 'llvm/' + relative
        if source_relative not in originals:
            generated[relative] = digest
            continue
        ft.require(digest == originals[source_relative], 'SDK public header differs from pinned source: ' + relative)
        installed[relative] = digest
        final = ft.sha(source / source_relative)
        if final != digest:
            ft.require(allowed.get(source_relative) == final, 'Unreviewed public-header mutation: ' + relative)
            differences[source_relative] = dict(sdk=digest, patched_source=final)
    # Generated entries are not waved through by a suffix heuristic. Their exact
    # name/byte set was reviewed against this SDK, including ABI configuration.
    ft.require(ft.fingerprint(generated) == SDK_GENERATED_HEADERS_SHA256,
               'SDK generated public-header set differs from the reviewed release')
    expected = {k[5:] for k in originals if k.endswith(('.h', '.def'))}
    ft.require(expected <= set(installed), 'SDK omits pinned public headers')
    ft.require(len(installed) >= 2330 and set(differences) == set(allowed), 'Incomplete public-header ABI proof')
    proof = dict(schema=1, llvm_revision=LLVM_PIN, circt_revision=CIRCT_PIN,
                 sdk=str(sdk), original_sdk=str(original_sdk), sdk_configuration=config,
                 release_fingerprint=ft.fingerprint(release), generated_headers=generated,
                 unchanged_installed_headers=installed, reviewed_additive_or_documentation_headers=differences,
                 public_header_layout_changes=False,
                 basis='Exact reviewed release compiler/library fingerprints, pinned pristine public headers, '
                       'exact generated ABI/configuration headers, and reviewed two-header patch hashes. '
                       'Generated build ABI and actual SDK-plugin loading are rechecked by full_toolchain.')
    ft.dump(work / 'sdk-source-proof.json', proof)
    return sdk, inventory, proof


def prepare_arcilator(work, sdk, jobs, cc, cxx, cmake, ninja):
    """Build the existing optional bytecode-output driver from pinned source."""
    work = Path(work)
    manifest_path = HERE / 'circt-patches/manifest.json'
    manifest = ft.load(manifest_path)
    ft.require(manifest['revision'] == CIRCT_PIN and manifest['llvm_revision'] == LLVM_PIN,
               'Arcilator source pins differ')
    checkout = work / 'circt-driver-source'
    target = checkout / manifest['source']['path']
    target.parent.mkdir(parents=True)
    download('https://raw.githubusercontent.com/llvm/circt/' + CIRCT_PIN + '/' + manifest['source']['path'],
             target, manifest['source']['original_sha256'])
    download('https://raw.githubusercontent.com/llvm/circt/' + CIRCT_PIN + '/tools/arcilator/CMakeLists.txt',
             target.parent / 'CMakeLists.txt', manifest['original_cmake_sha256'])
    for entry in manifest['patches']:
        patch = HERE / 'circt-patches' / entry['file']
        ft.checked_file(patch, entry['sha256'])
        command(['git', 'apply', '--check', patch], work / 'arcilator-build.log', cwd=checkout)
        command(['git', 'apply', patch], work / 'arcilator-build.log', cwd=checkout)
    ft.checked_file(target, manifest['source']['patched_sha256'])
    build = work / 'arcilator-driver'
    command([cmake, '-S', HERE / 'arcilator-driver', '-B', build, '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=' + str(cc),
             '-DCMAKE_CXX_COMPILER=' + str(cxx), '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
             '-DCIRCT_SDK=' + str(sdk), '-DARCILATOR_SOURCE=' + str(target),
             '-DARCILATOR_DRIVER_ENABLE_JIT=ON', '-DARCILATOR_DRIVER_ENABLE_FST=OFF',
             '-DARCILATOR_DRIVER_CXX11_ABI=1'], work / 'arcilator-build.log')
    command([cmake, '--build', build, '--parallel', str(jobs)], work / 'arcilator-build.log')
    binary = build / 'bin/arcilator'
    help_text = subprocess.check_output([str(binary), '--help'], text=True)
    ft.require('--emit-bytecode' in help_text, 'Bytecode driver capability absent')
    ft.dump(work / 'arcilator-source-record.json', dict(manifest=ft.checked_file(manifest_path),
            source=ft.checked_file(target), binary=ft.checked_file(binary),
            build_record=ft.checked_file(build / 'arcilator-driver-build.json')))
    return binary


def prepare(work, sdk, jobs, cc, cxx, cmake, ninja, source=None, *, build_arcilator=True):
    """Return local source/source_record/compatibility/arcilator and effective sdk.

    source can be a pristine pinned git checkout, a pristine extracted directory,
    or the authenticated pinned source archive. All work paths must be fresh.
    Passing build_arcilator=False retains the installed text-capable driver.
    """
    work = Path(work).resolve()
    ft.require(jobs > 0, 'jobs must be positive')
    ft.require(not work.exists() or not any(work.iterdir()), 'Source preparation work must be fresh')
    work.mkdir(parents=True, exist_ok=True)
    source_path, provenance, inventory = prepare_source(work, source)
    effective_sdk, sdk_files, proof = prepare_sdk(work, sdk, source_path, provenance)
    compatibility = work / 'compatibility.json'
    ft.dump(compatibility, dict(status='validated-pinned-sdk-source-pair', llvm_revision=LLVM_PIN,
        source_tree_sha256=inventory['tree_sha256'], public_header_layout_changes=False,
        sdk_files=sdk_files['files'], sdk_symlinks=sdk_files['symlinks'], sdk_tree_sha256=sdk_files['tree_sha256'],
        compiler_flags=[], requires_atomic_profile_updates=True,
        selected_backend_flags=['-parallel-virtreg-liveness-jobs=4'],
        evidence=[ft.checked_file(work / 'source-provenance.json'), ft.checked_file(work / 'sdk-source-proof.json')]))
    source_record = work / 'source-record.json'
    args = argparse.Namespace(source=source_path, output=source_record, revision=LLVM_PIN,
        provenance=work / 'source-provenance.json', compatibility_manifest=compatibility, sdk=effective_sdk)
    ft.record_source(args)
    arcilator = prepare_arcilator(work, effective_sdk, jobs, cc, cxx, cmake, ninja) if build_arcilator else effective_sdk / 'bin/arcilator'
    result = dict(source=source_path, source_record=source_record, compatibility=compatibility,
                  arcilator=arcilator, sdk=effective_sdk)
    ft.dump(work / 'prepared.json', {key: str(value) for key, value in result.items()})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['prepare'])
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    parser.add_argument('--cc', default='clang-18')
    parser.add_argument('--cxx', default='clang++-18')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--no-bytecode-driver', action='store_true')
    args = parser.parse_args()
    result = prepare(args.work, args.sdk, args.jobs, args.cc, args.cxx, args.cmake,
                     args.ninja, args.source, build_arcilator=not args.no_bytecode_driver)
    print(json.dumps({key: str(value) for key, value in result.items()}, indent=2))


if __name__ == '__main__':
    main()
