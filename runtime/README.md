# Bounded native EE block proof

This is a compiled integration proof, not an executable native Time Crisis 3 game. It translates the seven original scalar instructions at TST3 `0x00153310..0x00153328` into C++ against the pinned PCSX2 `cpuRegisters`. It does not introduce a second register layout, scheduler, memory system, IOP, VU, GS, or SPU implementation.

The provider requires the expected ELF SHA-256, every original instruction word, the exact entry PC, ordinary EE cycle rate, an aligned return address, and execution outside a delay slot. Rejection leaves all registers and timing state untouched. The bridge additionally rejects debugger breakpoints/memory checks and the Goemon TLB hack.

The optional patch inserts the provider at the existing EE JIT compilation dispatch point. A successful block returns through PCSX2's existing event-deadline decision and dispatcher; every other block retains the original JIT path. The proof is **dormant** until an authoritative loader calls `tc3NativeBlockProofBindLoadedElf()` with the actual bytes it loaded. That function hashes the bytes itself. No filename, environment variable, or caller-provided digest activates it. Recompiler reset clears the binding. The host does not yet supply this loader integration.

## Build and checks

Run from the project root after preparing and building the pinned core with `tools/prepare_core.py` and `tools/build_core.py`. The scalar test reuses that core's compiler flags and compiles the unchanged `R5900OpcodeImpl.cpp`, retaining its original scalar opcode handlers through function-section linking.

```sh
cmake -S . -B .local/native-proof-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTC3_BUILD_HOST=OFF \
  -DTC3_BUILD_NATIVE_BLOCK_TESTS=ON
cmake --build .local/native-proof-build --target tc3-native-block-tests
ctest --test-dir .local/native-proof-build -R native-block --output-on-failure
```

To compile the dispatcher integration, apply the checked-in patch **once to a fresh prepared export**, not to the pinned source checkout:

```sh
patch --batch -p1 -d .local/pcsx2-source \
  -i "$PWD/runtime/pcsx2-native-block-proof.patch"
python3 - <<'PY'
import hashlib, json
from pathlib import Path
patch = Path('runtime/pcsx2-native-block-proof.patch')
provenance = Path('.local/pcsx2-source/tc3-source-provenance.json')
data = json.loads(provenance.read_text())
patches = [p for p in data.get('local_patches', []) if p['path'] != str(patch)]
patches.append({'path': str(patch), 'sha256': hashlib.sha256(patch.read_bytes()).hexdigest()})
data['local_patches'] = patches
provenance.write_text(json.dumps(data, indent=2) + '\n')
Path('.local/core-build/tc3-build-report.json').unlink(missing_ok=True)
PY
python3 tools/build_core.py --native-block-proof --jobs 4
```

The build command regenerates the archive report with patch provenance and provider/bridge hashes. Pass `--prefix` if the core dependencies require a local prefix. Omitting `--native-block-proof` disables the optional integration.

## Evidence and remaining work

The scalar test passes 10,000 canonical-state comparisons against the original upstream SLL, ADDU, LUI, and ADDIU handlers, including randomized register contents and sign/overflow boundaries. It also checks corrupted instruction words and rejected execution conditions. The patched `iR5900.cpp`, provider, and bridge compile into the real PCSX2 archive.

The test supplies JR/delay-slot sequencing and cycle accounting explicitly; those portions are not an independent scheduler oracle. Actual JIT-versus-native dispatch execution, event-boundary equivalence, loader binding, exceptions, debugger transitions, invalidation, and savestate behavior still require integration tests before activation. Current-byte checks on each attempted execution are useful guards, not a claim of complete invalidation or savestate support. No gameplay speed or 60 FPS result follows from this proof.
