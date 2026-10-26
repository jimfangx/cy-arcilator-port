#!/usr/bin/env python3
"""Build private, same-revision LLVM drivers with selected implementation overlays.

The installed SDK is read-only. Direct implementation objects precede its static
archives, so their original archive members are not selected. PGO instruments
the compiler implementation, never the generated simulator. See TOOLCHAIN.md.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
import urllib.request

LLVM_REVISION = "a17069fea5b271c6824ea773874b007116de9644"
CIRCT_REVISION = "fa4f662cbe710906347fac795c581a06c6b99757"
RAW_ROOT = f"https://raw.githubusercontent.com/llvm/llvm-project/{LLVM_REVISION}/"
BUILDER_FORMAT = 1


def available_cpus():
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return os.cpu_count() or 1


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capture(argv):
    return subprocess.check_output([str(x) for x in argv],
                                   text=True,
                                   env=dict(
                                       os.environ,
                                       LLVM_PROFILE_FILE=os.devnull)).strip()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def fetch(work, relative):
    """Fetch an immutable revision; record upstream bytes even for patched TUs."""
    if not relative.startswith("llvm/") or ".." in Path(relative).parts:
        raise ValueError("source paths must be llvm/ paths without '..'")
    destination = work / "source" / relative
    manifest_path = work / "source-manifest.json"
    manifest = json.loads(
        manifest_path.read_text()) if manifest_path.exists() else {
            "schema": 1,
            "llvm_revision": LLVM_REVISION,
            "circt_revision": CIRCT_REVISION,
            "files": {}
        }
    if manifest.get("llvm_revision") != LLVM_REVISION:
        raise ValueError("source manifest belongs to another LLVM revision")
    destination.parent.mkdir(parents=True, exist_ok=True)
    data = urllib.request.urlopen(RAW_ROOT + relative, timeout=120).read()
    if destination.exists() and destination.read_bytes() != data:
        raise FileExistsError("fetch would replace a modified source: " +
                              str(destination))
    destination.write_bytes(data)
    manifest["files"][relative] = {
        "path": str(destination),
        "url": RAW_ROOT + relative,
        "sha256": sha256(destination)
    }
    manifest.update(builder_format=BUILDER_FORMAT,
                    builder_sha256=sha256(Path(__file__)))
    write_json(manifest_path, manifest)
    return destination


def prepare_layout(sdk, work, tool, compilers=None):
    """Expose matching SDK dependencies without replacing compiler outputs."""
    sdk, work = sdk.resolve(), work.resolve()
    if work == sdk or sdk in work.parents:
        raise ValueError("overlay prefix must be outside the reference SDK")
    version = capture([sdk / "bin/firtool", "--version"])
    if "CIRCT firtool-1.162.0" not in version:
        raise ValueError("overlay prefix requires the pinned CIRCT SDK")
    if (work / "bin").is_symlink():
        raise ValueError("overlay bin directory must not be a symlink")
    links = [(work / name, sdk / name) for name in ("include", "lib")]
    companions = ("llvm-config", "llvm-as", "llvm-dis", "llvm-lto2",
                  "llvm-objcopy", "opt", "llc")
    compilers = compilers or {}
    for name, path in compilers.items():
        if name not in ("opt", "llc") or not path.is_file():
            raise ValueError("invalid compiler bundle input: " + str(path))
        if "LLVM version 24.0.0git" not in capture([path, "--version"]):
            raise ValueError(
                "compiler bundle input does not match SDK LLVM version")
    links += [(work / "bin" / name, compilers.get(name, sdk / "bin" / name))
              for name in companions if name != tool]
    # Validate every destination first; never replace unrelated files or links.
    for destination, target in links:
        if not target.exists():
            raise FileNotFoundError("missing reference SDK dependency: " +
                                    str(target))
        if os.path.lexists(destination):
            if not destination.is_symlink() or destination.resolve(
            ) != target.resolve():
                raise FileExistsError("unexpected overlay prefix conflict: " +
                                      str(destination))
    (work / "bin").mkdir(parents=True, exist_ok=True)
    for destination, target in links:
        if not os.path.lexists(destination):
            destination.symlink_to(target, target_is_directory=target.is_dir())
    records = [{
        "path": str(path),
        "target": str(target),
        "sha256": sha256(target) if target.is_file() else None
    } for path, target in links]
    record = {
        "schema":
        1,
        "builder_format":
        BUILDER_FORMAT,
        "builder_sha256":
        sha256(Path(__file__)),
        "sdk":
        str(sdk),
        "llvm_revision":
        LLVM_REVISION,
        "circt_revision":
        CIRCT_REVISION,
        "firtool_version":
        version,
        "sdk_firtool_sha256":
        sha256(sdk / "bin/firtool"),
        "sdk_llvm_config_sha256":
        sha256(sdk / "bin/llvm-config"),
        "sdk_llvm_config_header_sha256":
        sha256(sdk / "include/llvm/Config/llvm-config.h"),
        "compiler_output":
        str(work / "bin" / tool) if tool else None,
        "compiler_inputs": {
            name: {
                "path": str(path),
                "sha256": sha256(path)
            }
            for name, path in compilers.items()
        },
        "links":
        records
    }
    write_json(work / "layout.json", record)
    return record


def configuration(args):
    sdk = args.sdk.resolve()
    work = args.work.resolve()
    if work == sdk or sdk in work.parents:
        raise ValueError("work directory must be outside the reference SDK")
    llvm_config = sdk / "bin/llvm-config"
    version = capture([llvm_config, "--version"])
    if version != "24.0.0git":
        raise ValueError(f"expected pinned LLVM24 SDK, got {version}")
    circt_version = capture([sdk / "bin/firtool", "--version"])
    if "CIRCT firtool-1.162.0" not in circt_version:
        raise ValueError(
            f"expected pinned CIRCT firtool-1.162.0 SDK, got {circt_version}")
    sources = [Path(s).resolve() for s in args.source]
    if not sources:
        raise ValueError("at least one implementation --source is required")
    main = work / "source" / f"llvm/tools/{args.tool}/{args.tool}.cpp"
    if not main.exists():
        raise ValueError(f"fetch the pinned driver first: {main}")
    source_records = [{
        "path": str(p),
        "sha256": sha256(p)
    } for p in sources + [main]]
    # clang++ is commonly a symlink to clang; its argv[0] selects C++ linking.
    compiler = str(args.compiler.absolute())
    flags = shlex.split(capture([llvm_config, "--cxxflags"]))
    flags += ["-O3", "-DNDEBUG", "-fno-rtti", "-fno-exceptions"]
    # Patched private headers must precede matching installed SDK headers.
    flags = [f"-I{p.resolve()}" for p in args.include] + flags
    if args.native:
        flags += ["-march=native"]
    compile_only_flags = []
    if args.mode == "instrument":
        flags += [
            "-fprofile-generate"
            if args.profile_kind == "ir" else "-fprofile-instr-generate"
        ]
    elif args.mode == "pgo":
        if args.profile is None or not args.profile.is_file():
            raise ValueError(
                "pgo mode requires --profile pointing to merged profdata")
        use_flag = "-fprofile-use=" if args.profile_kind == "ir" else "-fprofile-instr-use="
        flags += [use_flag + str(args.profile.resolve())]
        if args.profile_kind == "ir":
            # LLVM normally hides COMDAT mismatches. Diagnose those too and
            # reject backend warnings, instead of silently dropping profiles.
            compile_only_flags += [
                "-Werror", "-mllvm", "-no-pgo-warn-mismatch=false", "-mllvm",
                "-no-pgo-warn-mismatch-comdat-weak=false"
            ]
        else:
            compile_only_flags += [
                "-Werror=profile-instr-out-of-date",
                "-Werror=profile-instr-unprofiled"
            ]
    if args.lto:
        flags += ["-flto=thin"]
        if args.linker is None:
            raise ValueError("--lto requires an explicit matching --linker")
    objects = [
        work / "objects" / f"{i}-{p.stem}.o"
        for i, p in enumerate(sources + [main])
    ]
    compile_commands = [[
        compiler, *flags, *compile_only_flags, "-c",
        str(p), "-o",
        str(o)
    ] for p, o in zip(sources + [main], objects)]
    for argv, obj in zip(compile_commands, objects):
        argv += ["-MMD", "-MF", str(obj.with_suffix(".d"))]
    driver = "LLVMOptDriver" if args.tool == "opt" else "LLVMLlcDriver"
    libraries = shlex.split(
        capture([llvm_config, "--link-static", "--libs", "all"]))
    system_libraries = shlex.split(
        capture([llvm_config, "--link-static", "--system-libs"]))
    output = work / "bin" / args.tool
    temporary = output.with_suffix(".new")
    link = [
        compiler, *flags, *(str(o) for o in objects), "-Wl,--export-dynamic",
        "-L" + str(sdk / "lib"), "-Wl,--start-group", "-l" + driver,
        *libraries, "-Wl,--end-group", *system_libraries, "-pthread", "-o",
        str(temporary)
    ]
    if args.linker:
        # LLD also dispatches by argv[0]; preserve the ld.lld symlink name.
        link += ["--ld-path=" + str(args.linker.absolute())]
    archive_names = [driver] + [
        lib[2:] for lib in libraries if lib.startswith("-l")
    ]
    archive_records = []
    for name in dict.fromkeys(archive_names):
        archive = sdk / "lib" / ("lib" + name + ".a")
        archive_records.append({
            "path": str(archive),
            "sha256": sha256(archive)
        })
    records = {
        "schema": 1,
        "builder_format": BUILDER_FORMAT,
        "llvm_revision": LLVM_REVISION,
        "circt_revision": CIRCT_REVISION,
        "sdk": str(sdk),
        "sdk_version": version,
        "circt_version": circt_version,
        "tool": args.tool,
        "mode": args.mode,
        "profile_kind": args.profile_kind,
        "builder_sha256": sha256(Path(__file__)),
        "compiler_version": capture([compiler, "--version"]),
        "sources": source_records,
        "output": str(output),
        "compile_commands": compile_commands,
        "link_command": link,
        "sdk_tool_sha256": sha256(sdk / "bin" / args.tool),
        "sdk_driver_archive_sha256": archive_records[0]["sha256"],
        "sdk_static_archives": archive_records,
        "profile_sha256": sha256(args.profile) if args.profile else None
    }
    return records


def build(args):
    record = configuration(args)
    work = args.work.resolve()
    write_json(work / "plan.json", record)
    if args.command == "plan":
        print(json.dumps(record, indent=2))
        return
    record["prefix_layout"] = prepare_layout(args.sdk, work, args.tool)
    for name in ("objects", "bin", "logs"):
        (work / name).mkdir(parents=True, exist_ok=True)
    started = time.monotonic()

    def compile_one(item):
        i, argv = item
        with (work / "logs" / f"compile-{i}.log").open("w") as log:
            subprocess.run(argv,
                           stdout=log,
                           stderr=subprocess.STDOUT,
                           check=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(compile_one, enumerate(record["compile_commands"])))
    dependencies = set()
    for argv in record["compile_commands"]:
        depfile = Path(argv[argv.index("-MF") + 1])
        text = depfile.read_text().replace("\\\n", "")
        dependencies.update(shlex.split(text.split(":", 1)[1]))
    record["compiler_dependencies"] = [{
        "path": str(Path(p).resolve()),
        "sha256": sha256(p)
    } for p in sorted(dependencies)]
    with (work / "logs/link.log").open("w") as log:
        subprocess.run(record["link_command"],
                       stdout=log,
                       stderr=subprocess.STDOUT,
                       check=True)
    for source in record["sources"]:
        if sha256(source["path"]) != source["sha256"]:
            raise RuntimeError("source changed during build: " +
                               source["path"])
    output = Path(record["output"])
    output_version = capture([output.with_suffix(".new"), "--version"])
    output.with_suffix(".new").replace(output)
    record.update(seconds=time.monotonic() - started,
                  output_sha256=sha256(output),
                  output_version=output_version)
    write_json(work / "build.json", record)
    print(
        json.dumps(
            {
                "output": str(output),
                "seconds": record["seconds"],
                "sha256": record["output_sha256"]
            },
            indent=2))


def train(args):
    manifest = json.loads(args.manifest.read_text())
    if not isinstance(manifest, list) or not manifest:
        raise ValueError(
            "training manifest must be a nonempty list of argv lists")
    tool = args.tool.resolve()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    # Never merge profiles from earlier training runs accidentally.
    profile_dir = Path(tempfile.mkdtemp(prefix="profiles-", dir=work))
    environment = {
        key: value
        for key, value in os.environ.items()
        if key not in ("LD_PRELOAD", "ARC_COMPILER_SAMPLE_PATH",
                       "LLVM_PROFILE_FILE")
    }
    commands = []
    for i, argv in enumerate(manifest):
        if not isinstance(argv,
                          list) or not argv or argv[0] != "{tool}" or not all(
                              isinstance(x, str) for x in argv):
            raise ValueError(
                "each training argv must begin with literal {tool}")
        argv = [
            x.replace("{tool}", str(tool)).replace("{work}", str(work))
            for x in argv
        ]
        commands.append(argv)
    if args.jobs > 1 and len(commands) > 1:
        outputs = set()
        for argv in commands:
            output = None
            for i, value in enumerate(argv):
                if value in ("-o", "--output") and i + 1 < len(argv):
                    output = argv[i + 1]
                elif value.startswith(("-o=", "--output=")):
                    output = value.split("=", 1)[1]
            if output is None:
                raise ValueError(
                    "parallel training requires an explicit -o output for each command"
                )
            if output == "-":
                continue  # Each process has its own stdout log.
            output = str(Path(output).resolve())
            if output in outputs:
                raise ValueError("parallel training outputs overlap: " +
                                 output)
            outputs.add(output)

    def run_one(item):
        i, argv = item
        run_environment = dict(environment,
                               LLVM_PROFILE_FILE=str(profile_dir /
                                                     f"{i}-%m-%p.profraw"))
        started = time.monotonic()
        with (work / f"training-{i}.log").open("w") as log:
            process = subprocess.run(argv,
                                     env=run_environment,
                                     stdout=log,
                                     stderr=subprocess.STDOUT,
                                     check=False)
        return {
            "argv": argv,
            "seconds": time.monotonic() - started,
            "returncode": process.returncode,
            "log": str(work / f"training-{i}.log")
        }

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        records = list(pool.map(run_one, enumerate(commands)))
    training = {
        "schema": 1,
        "builder_format": BUILDER_FORMAT,
        "builder_sha256": sha256(Path(__file__)),
        "tool": str(tool),
        "tool_sha256": sha256(tool),
        "manifest_sha256": sha256(args.manifest),
        "jobs": args.jobs,
        "runs": records,
        "profile_directory": str(profile_dir)
    }
    write_json(work / "training.json", training)
    if any(record["returncode"] for record in records):
        raise RuntimeError(
            "compiler training failed; see training.json and per-job logs")
    profiles = sorted(profile_dir.glob("*.profraw"))
    if not profiles:
        raise ValueError("instrumented compiler produced no profiles")
    merged = work / "compiler.profdata"
    temporary = merged.with_suffix(".new")
    subprocess.run([
        str(args.profdata.resolve()), "merge", "-o",
        str(temporary), *(str(p) for p in profiles)
    ],
                   check=True)
    temporary.replace(merged)
    training.update(profiles=[str(p) for p in profiles],
                    merged=str(merged),
                    merged_sha256=sha256(merged),
                    profdata_version=capture(
                        [args.profdata.resolve(), "--version"]))
    write_json(work / "training.json", training)
    print(merged)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser(
        "fetch", help="fetch immutable LLVM source files only")
    prepare.add_argument("--work", type=Path, required=True)
    prepare.add_argument("paths",
                         nargs="+",
                         help="revision-relative llvm/ paths")
    layout = commands.add_parser(
        "layout",
        help="expose matching SDK dependencies in an existing private prefix")
    layout.add_argument("--sdk", type=Path, required=True)
    layout.add_argument("--work", type=Path, required=True)
    layout.add_argument("--tool", choices=("opt", "llc"), required=True)
    bundle = commands.add_parser(
        "bundle",
        help="link a private prefix to explicit qualified compiler candidates")
    bundle.add_argument("--sdk", type=Path, required=True)
    bundle.add_argument("--work", type=Path, required=True)
    bundle.add_argument("--opt", type=Path, required=True)
    bundle.add_argument("--llc", type=Path, required=True)
    for command in ("plan", "build"):
        p = commands.add_parser(command)
        p.add_argument("--sdk", type=Path, required=True)
        p.add_argument("--work", type=Path, required=True)
        p.add_argument("--tool", choices=("opt", "llc"), required=True)
        p.add_argument("--compiler", type=Path, required=True)
        p.add_argument("--source", action="append", default=[])
        p.add_argument("--include", action="append", type=Path, default=[])
        p.add_argument("--mode",
                       choices=("release", "instrument", "pgo"),
                       default="release")
        p.add_argument("--profile-kind",
                       choices=("frontend", "ir"),
                       default="frontend")
        p.add_argument("--profile", type=Path)
        p.add_argument("--jobs", type=int, default=available_cpus())
        p.add_argument("--native",
                       action="store_true",
                       help="optimize the compiler executable for this host")
        p.add_argument("--lto",
                       action="store_true",
                       help="ThinLTO across overlay objects only")
        p.add_argument("--linker", type=Path)
    p = commands.add_parser("train")
    p.add_argument("--tool", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--profdata", type=Path, required=True)
    p.add_argument("--jobs", type=int, default=available_cpus())
    args = parser.parse_args()
    if args.command == "fetch":
        for path in args.paths:
            result = fetch(args.work.resolve(), path)
            print(json.dumps({"path": str(result), "sha256": sha256(result)}))
    elif args.command == "layout":
        print(
            json.dumps(prepare_layout(args.sdk, args.work, args.tool),
                       indent=2))
    elif args.command == "bundle":
        compilers = {"opt": args.opt.resolve(), "llc": args.llc.resolve()}
        print(
            json.dumps(prepare_layout(args.sdk, args.work, None, compilers),
                       indent=2))
    elif args.command == "train":
        if args.jobs < 1:
            parser.error("--jobs must be positive")
        train(args)
    else:
        if args.jobs < 1:
            parser.error("--jobs must be positive")
        build(args)


if __name__ == "__main__":
    main()
