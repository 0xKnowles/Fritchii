# Offline shader compiler

`shader-sweep` runs captured instructions through Ampere lowering, Maxwell control-flow
analysis and translation, optimization passes, and SPIR-V emission. It does not launch
the emulator or a Vulkan device. Use `spirv-val` separately to validate its output.

## Build

The normal project build exposes the `shader-sweep` target with `ENABLE_DEBUG_TOOLS=ON`.
On Linux, a separate build needs only the common and shader-recompiler libraries:

```sh
sudo apt-get install cmake ninja-build g++ libboost-all-dev nlohmann-json3-dev \
  libssl-dev liblz4-dev libzstd-dev libcpp-httplib-dev libxbyak-dev python3 spirv-tools
cmake -S tools/shader-sweep -B build/shader-sweep -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/shader-sweep -j4
ctest --test-dir build/shader-sweep --output-on-failure
```

CMake 3.24 or newer is required. The standalone configuration downloads pinned fmt,
SPIRV-Headers, sirit, and ZBIC sources. It does not require system SPIRV-Headers.
The tests include synthetic CLI cases, parser bounds checks, and three small captured
regression programs. `spirv-val` is required when `BUILD_TESTING` is enabled.

## Inputs

Compile one extracted v46, v47 or v48 serialized environment record:

```sh
shader-sweep --environment shader.env --cache-version 47 --output shader.spv
```

The record starts at its code-size field. Do not include a disk-cache header, pipeline
key, or neighboring records. Graphics records contain the program header and stage;
compute records contain the workgroup and shared-memory sizes. `--pipeline-key shader.key`
adds the captured 1056-byte graphics pipeline key. Compute keys are not accepted.

To rerun native lowering with captured specialization data:

```sh
shader-sweep capture.bin --environment shader.env --cache-version 47 \
  --pipeline-key shader.key --output shader.spv
```

Raw dumps default to instruction-only format. Use `--format native-header` only for
captures with a 128-byte native graphics header followed by 16-byte instructions.
The header supplies the exact stage. Instruction-only compilation needs `--stage`
or a captured environment. Graphics compilation also requires a header or environment;
the tool does not invent graphics input/output metadata.

The six explicit stage names are `vertex`, `tess-control`, `tess-eval`, `geometry`,
`fragment`, and `compute`. Conflicting explicit, header, and environment stages are errors.
Task, mesh, and vertex-A cache records report missing pipeline context.

For matching a raw capture against a cache, lower without choosing a graphics stage:

```sh
shader-sweep capture.bin --lower-only lowered.bin
```

This writes packed little-endian 64-bit Maxwell instructions, without a header,
scheduling words, or native address map. The reported stage is null when unknown.
Geometry-specific lowering requires `--stage geometry`. `--lower-only` does not run
control-flow translation or SPIR-V emission and is not a full compilation check.

## Offline context and profile

Missing constant-buffer values, texture descriptors, or storage-pointer classifications
are errors, not zero values. v46 records do not contain storage-pointer classifications.
Native trap guards require captured constant-buffer values and must verify before lowering.

The compiler reports assumptions in each JSON result. Without an environment, compute
uses a 1x1x1 workgroup and zero local/shared memory. Without a graphics pipeline key,
the runtime uses point input topology, isolines with equal tessellation spacing, float
interface types, and no alpha test or transform feedback. With a key, vertex input types,
fragment attachment types, topology, tessellation modes, alpha test, and depth/fragment
flags come from the capture. Transform feedback is rejected. Previous-stage varying
linkage is not reconstructed; all previous-stage varying slots are enabled.

The fixed Vulkan-style profile emits SPIR-V 1.4 with unified descriptor bindings,
one-byte minimum storage-buffer alignment, 32-lane subgroups, integer types, subgroup
votes/shuffles, geometry streams/passthrough, viewport/layer output, and eight clip
distances. Descriptor aliasing and native 8/16-bit storage access stay disabled to
exercise the compiler's portable fallbacks. Integer fragment attachments always use
typed outputs. Default emulator shader settings apply, with resolution scaling inactive.
This is an offline compiler exercise, not a driver-specific golden shader or proof of
correct in-game rendering.

## Results

Stdout contains one JSON object, except for `--help`. Stderr receives compiler warnings.
The object has `schema_version: 1`, `status`, `phase`, `stage`, `input_format`,
`source_bytes`, `spirv_words`, and `assumptions`. Lowering adds `lowered_words`.
Errors add `error.code` and `error.message`; lowering errors also include the native
instruction byte `error.offset`.

Exit status is 0 for successful emission, 1 for compilation/context/output failures,
and 2 for invalid arguments or input. Missing specialization data has phase `context`
and code `missing_context`. Compiler assertions routed through the common assertion
API become errors; a process-level crash in a dependency still needs to be detected
by the caller. Compilation failures do not overwrite the requested output.

```sh
spirv-val --target-env vulkan1.2 shader.spv
```
