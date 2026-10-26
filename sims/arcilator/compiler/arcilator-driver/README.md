# Build the optional Arcilator bytecode driver

This standalone project compiles one external Arcilator driver source file and
links it with the installed CIRCT `firtool-1.162.0` SDK libraries. It keeps the
SDK unchanged and does not rebuild its compiler passes. No upstream source or
validation programs are copied into this directory.

The project accepts only the SHA-256 hashes of the pinned original driver and
the driver modified by [../circt-patches/emit-bytecode.patch](../circt-patches/emit-bytecode.patch).
The SDK must report CIRCT `firtool-1.162.0` and LLVM `24.0.0git`. The source pin
and hashes are recorded in [../circt-patches/manifest.json](../circt-patches/manifest.json).
Source changes trigger CMake reconfiguration and another hash check.

From the Chipyard root, after sourcing `env.sh`, fetch the two pinned source
files and apply the patch in a private directory:

```sh
arc_port_root="$PWD"
arc_driver_work="$(mktemp -d)"
arc_driver_sdk="$(dirname "$(dirname "$(command -v firtool)")")"
mkdir -p "$arc_driver_work/circt/tools/arcilator"
curl --fail --location \
  https://raw.githubusercontent.com/llvm/circt/fa4f662cbe710906347fac795c581a06c6b99757/tools/arcilator/arcilator.cpp \
  --output "$arc_driver_work/circt/tools/arcilator/arcilator.cpp"
curl --fail --location \
  https://raw.githubusercontent.com/llvm/circt/fa4f662cbe710906347fac795c581a06c6b99757/tools/arcilator/CMakeLists.txt \
  --output "$arc_driver_work/circt/tools/arcilator/CMakeLists.txt"
git -C "$arc_driver_work/circt" apply --check \
  "$arc_port_root/sims/arcilator/compiler/circt-patches/emit-bytecode.patch"
git -C "$arc_driver_work/circt" apply \
  "$arc_port_root/sims/arcilator/compiler/circt-patches/emit-bytecode.patch"
cmake -S "$arc_port_root/sims/arcilator/compiler/arcilator-driver" \
  -B "$arc_driver_work/build" -G Ninja \
  -DCIRCT_SDK="$arc_driver_sdk" \
  -DARCILATOR_SOURCE="$arc_driver_work/circt/tools/arcilator/arcilator.cpp" \
  -DARCILATOR_DRIVER_ENABLE_JIT=ON \
  -DARCILATOR_DRIVER_ENABLE_FST=OFF \
  -DARCILATOR_DRIVER_CXX11_ABI=1 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$arc_driver_work/build" --parallel "$(nproc)"
"$arc_driver_work/build/bin/arcilator" --version
"$arc_driver_work/build/bin/arcilator" --help
```

Alternatively, `ARCILATOR_SOURCE` can name the unchanged pinned file for a
reference driver build. `CIRCT_SDK` defaults to the SDK containing `firtool` on
`PATH`. The default JIT-on, FST-off, C++11-ABI-1 options match the qualified Linux
SDK. Changing them changes driver capabilities or ABI and needs separate
qualification against a compatible SDK build.

The executable is `<build>/bin/arcilator`. It has the existing output modes
and, with the patched source, the explicit `--emit-bytecode` mode. Use the
driver through the port's tool selection while retaining the installed SDK
directory for its other tools and libraries. For example, explicit Make tool
selection is `CIRCT_BIN="$arc_driver_sdk/bin"`
`ARCILATOR="$arc_driver_work/build/bin/arcilator"`; output-format selection is
handled separately by the model compiler.

`arcilator-driver-config.json` records the source/SDK identities, compiler,
flags, and capabilities at configuration. After linking,
`arcilator-driver-build.json` records the executable path and hash and rechecks
the external source. These describe this separately built tool; they are not
installed SDK files.

The qualified original-source build matched the SDK's version/help output.
The patched driver's help added only `--emit-bytecode`. Complete model pilots
with both drivers preserved model JSON and raw LLVM apart from its `ModuleID`
comment for Rocket, BOOM, Saturn, and Shuttle. Those checks do not establish an
end-to-end build-time improvement or cover every possible compiler flag.
