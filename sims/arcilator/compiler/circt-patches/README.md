# Arcilator bytecode output

`emit-bytecode.patch` adds the explicit `--emit-bytecode` output format to the
Arcilator driver in CIRCT `firtool-1.162.0`, commit
`fa4f662cbe710906347fac795c581a06c6b99757`. It also adds the bytecode-writer
link dependency to the upstream driver target. Existing output modes and
compiler pipelines are unchanged. The default remains LLVM text output.

Bytecode preserves MLIR operations, attributes, and locations without the text
printing/parsing boundary. In particular, bytecode keeps existing locations;
text printed without debug locations receives new file/line locations when
parsed. The patch does not add a cache or change simulation functions.

The unmodified SDK-linked driver matched the SDK's version and help output.
The patched driver's help adds only `--emit-bytecode`. Both the unmodified
driver and the bytecode conversion path were checked on complete Rocket, BOOM,
Saturn, and Shuttle models: model JSON was byte-identical, and raw LLVM matched
apart from the `ModuleID` comment. These are model-compilation checks, not a
claim of an end-to-end build speedup or exhaustive target coverage. Validation
programs are not included here.

`manifest.json` pins source and patch hashes, the SDK version, and the qualified
driver features. Apply the patch to a private checkout of that CIRCT revision,
or use the external-source build instructions in
[../arcilator-driver/README.md](../arcilator-driver/README.md). Keep the installed
SDK unchanged.

The patch modifies CIRCT code under Apache 2.0 with LLVM Exceptions.
`LICENSE.txt` is the license from the pinned CIRCT revision; its source and hash
are recorded in the manifest. The original source's copyright and SPDX notices
remain intact.
