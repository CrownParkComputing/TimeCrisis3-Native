# TC3 loadable roles and observed code coverage

The pinned **TC3LOAD is the boot executable and contains the observed main
gameplay code**. Its name must not be interpreted as proof that the actual game
is absent from that ELF. Conversely, one matching snapshot cannot establish that
all stages use only this code.

## Direct evidence

The pinned PCSX2x6 `bin/resources/GameIndex.yaml` entry `NM00012` identifies Time
Crisis 3 and selects `bootprog: TC3LOAD`. The private ELF has a file-backed main
load at `0x100000`, entry `0x100008`, and 2,594,048 file-backed bytes. Its larger
30,365,184-byte memory reservation includes zero-initialized storage; that size is
not an instruction coverage measurement. A separate scratchpad reservation also
has executable flags without file-backed bytes.

Comparison to the calibrated PCSX2 first-stage state found:

| Measurement | Result |
|---|---:|
| Recovered function-map rows | 28,078 |
| Distinct bytes covered by merged function ranges | 1,438,584 |
| Different bytes within those function ranges | **0** |
| Different bytes across the entire file-backed main load | 832 |
| First changed address in the main load | `0x2b3c80` |
| Highest recovered function range end, exclusive | `0x26de48` |

The changed segment bytes are outside the recovered function ranges. This
comparison does not by itself classify every changed byte as harmless data.
Overlapping function/entry records are merged before counting coverage.

The map's `End` column is explicitly **exclusive**. The original exporter
`ps2xRecomp/tools/ghidra/ExportPS2Functions.java:1049` calculates
`body.getMaxAddress().getOffset() + 1L` and prints `record.endExclusive` in the
CSV. The last range includes the return at `0x26de40` and its delay slot at
`0x26de44`; no final instruction bytes are omitted. `PrepareTC3.java` independently
documents this code/data cutoff. The recovered map is evidence, not a complete
instruction discovery oracle.

## TC3GAME: a conditional loader identification

The original 100,068-byte `dongle/TC3GAME` is not an ELF and contains no ELF magic.
Its first little-endian word is `0x100000`; this alone does not establish whether
that is an address, length or another field. No validated decompressor for this
payload has yet been established in this project.

The pre-existing 178,789-byte `TC3GAME.candidate` also is not an ELF. Its derivation
was not recoverable from the existing extraction tools, so it is **not approved
as a decoded executable**. Nevertheless, its internally consistent static code
strongly suggests a loader role, conditional on validating that derivation:

- Assuming base `0x100000`, code at `0x100590..0x1005a8` assembles `mc0:ACLOAD`.
- Code at `0x1005e8..0x100618` assembles either `ac0:TC3LOAD` or `mc0:TC3LOAD`,
  using the format/path strings at `0x12b940`, `0x12b948`, `0x12b958`, and
  `0x12b960`.
- It passes that constructed path to a call at `0x100634`, target `0x121f70`.
  That routine's chain ends at `0x1193a0`, a syscall-6 wrapper.
- Neither complete candidate nor complete packed payload appears verbatim in the
  supplied gameplay RAM. That observation alone does not prove they were unused
  earlier in boot or never decompressed.

Thus **TC3GAME is not evidence of a missing large gameplay executable**. The
verified ELF/RAM comparison independently establishes TC3LOAD's gameplay role;
the candidate provides a plausible earlier loader relationship. The original
packed format and exact boot loading sequence remain unvalidated.

## Remaining coverage obligations

The DVD directory contains FGH/FGI archives. Their interior resources have not
been decoded by the inventory tool. TC3LOAD contains stage-specific resource path
strings, including `cic_acgame_s1_0.bin`, `cic_acgame_s2_0.bin`, and
`cic_acgame_s3_0.bin`; names alone do not establish whether those are EE programs,
interpreted scripts or data. Observe actual load/write/execute behavior before
assigning executable coverage.

IRX modules relocate and execute on the IOP. Their discovered ELF occurrences
include container/extracted duplicates. They remain on the pinned PCSX2 IOP
implementation. VU microprograms and mutable/generated EE code also require
separate execution-aware coverage. Preserve PCSX2 fallback for every address not
independently validated for a static native replacement.

## Reproduction

`tools/inspect_loadables.py` accepts explicit private ELF, RAM/save-state,
function-map and optional payload paths. It verifies the title hash, compares
merged mapped ranges, and emits hashes/counts only under `.local`. The inspected
report is `.local/inventory/loadables.json`; no proprietary payload is committed.

```sh
python3 tools/inspect_loadables.py \
  --elf /private/TST3/TC3LOAD.elf \
  --state /private/reference-calibrated-stage1-start.p2s \
  --function-map /private/tc3load.csv \
  --payload /private/TST3/dongle/TC3GAME \
  --payload /private/TST3/TC3GAME.candidate
```
