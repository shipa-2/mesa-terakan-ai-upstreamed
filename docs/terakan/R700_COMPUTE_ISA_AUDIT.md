# R700 compute and the CF index question

Audit: 2026-09-05. This is documentation evidence, not a hardware result.

## Evergreen index selection

AMD's [Evergreen ISA revision 1.1a](https://docs.amd.com/api/khub/documents/lMd~Rb1b_AOs0nYgx3YxHA/content),
printed pages 9-219 and 9-220 (PDF pages 329 and 330), specifies that SET_CF_IDX0/1 obtains
the sequencer AR value from the first active pixel and clamps it to 0..255. This does not
mean unconditional lane zero.

Those descriptions prohibit combining the operation with waterfalling, MOVA_INT or LDS,
and prohibit setting both index registers in one VLIW instruction. They do not explicitly
ban ALU_PUSH_BEFORE or execution inside a divergent region. The first prohibition does
not itself specify whether its scope is an instruction group or an entire clause; do
not turn it into a proven clause-level scheduling rule.

Consequently, the reported MOVA_INT / SET_CF_IDX / PRED_SETE_INT sequence needs its actual
VLIW grouping and active-mask transitions inspected. These pages answer the lane-selection
question but do not prove why the measured divergent case retains the preceding index.
No change to split_address_loads or the RAT scheduler follows from this audit alone.

## R700 is not an Evergreen compute register variant

AMD's [R700 ISA revision 1.0a](https://docs.amd.com/api/khub/documents/2ZrqL_eSnIV39R0_tg_OwQ/content),
sections 3.4.2 and 9.1 (printed pages 3-9 and 9-25), describes MEM_EXPORT scatter access
to a shared linear buffer, with per-thread addresses and restrictions on cross-thread
visibility. This is not an Evergreen RAT descriptor interface. Searching this document
for SET_CF_IDX produced no match; that absence alone is not proof of an opcode boundary.

The independent local encoding evidence is `src/gallium/drivers/r600/r600_isa.c`:
SET_CF_IDX0/1 have no R600/R700 encoding, MEM_RAT has none either, while MEM_EXPORT has
R700 opcode 0x3a. `r600_pipe.c` exposes compute only above R700 and initializes the
Evergreen compute atom only in the later-generation branch. Therefore classic Gallium
does not supply the requested ready-made R700 dispatch replacement.

The remaining implementation gate is a documented R700 work-launch and memory-export
path, including bounds, synchronization and readback. Reusing Evergreen DISPATCH_DIRECT,
RAT writes or LS state is not justified by successful CB/TC copies. Keep submit guarded;
do not claim a Vulkan dispatch by substituting an unrelated graphics copy test.

## Launch and DRM boundary

AMD's [Evergreen/Northern Islands acceleration guide, revision 1.0](https://docs.amd.com/api/khub/documents/ApP4PzuRytQl9QLD9lgkmA/content),
section 3.1, explicitly describes R7xx compute as a special ES shader, using ES
resources and SX_MEMORY_EXPORT_BASE, with ESGS/streamout as other output paths.
This is useful positive evidence for an ES investigation, not permission to use
the LS setup in section 3.2. Sections 9.3.12/13 label DISPATCH_DIRECT/INDIRECT
Evergreen/Cayman only. The overview's generic DISPATCH wording must not override
that generation qualification. An exact R7xx launch recipe is still missing.

The earlier claim that upstream has no SX memory-export handler was WRONG.
The web text extraction/search returned no match, but directly downloaded source
contains `case SX_MEMORY_EXPORT_BASE`, including in Linux v6.12. Absence from
that search result was not source evidence. No special remote patch follows
from the module disassembly. See the pinned-source correction below.

This inspection was of upstream source on 2026-09-05, not the exact remote
7.1.2-zen3-1-zen source: its installed build trees do not contain r600_cs.c.
No rejected dispatch or export-register write was submitted to RV710 to test
this inference. No kernel modification or validation bypass is authorized by
this note. The next implementation must resolve both launch and bounded output,
not just supply a shader with the right ISA opcode.

## Remote module correction (2026-09-06)

Do not infer that the installed remote kernel rejects SX exports from the initial
audit. Read-only inspection of its actual `radeon.ko` contradicted that audit.
The installed and loaded module report matching srcversion
`522F601355FD49E289807F7`; the copied `radeon.ko.zst` has SHA-256
`50d4e53498853a4e5331d908a876e7b382719e623c0b44575b5a972c19c0d824`.
Matching srcversion is supporting identification, not a hash of loaded memory.

In this ELF, `r600_cs_check_reg` starts at `.text+0x69a90`. Its safe-bitmap
lookup relocates against `.rodata+0x1cd40`. Word `0x120` is `0xffffffdf`:
register 0x9010 (bit 4) requires special handling, whereas 0x9014 (bit 5) is
accepted by the bitmap. The special branch at 0x69d15 compares against 0x9010,
calls `radeon_cs_packet_next_reloc` at 0x69d2d, and adds a relocation-derived
value to the IB register payload at 0x69d4b. It then returns success. Thus
there IS an export-base relocation path on this machine. The observed branch
does not itself couple the separately accepted size register to a BO bound;
this is not proof that no other check does so. Obtain the exact source before
using that path for shader writes.

The same module's `r600_cs_parse` packet-opcode decision tree at 0x6bf3e..0x6bf95
sends opcodes 0x15/0x16 (DISPATCH_DIRECT/INDIRECT) to -EINVAL: below 0x28 the
accepted comparisons are 0x20, 0x24 and 0x10. This is binary evidence, not an
actual rejected ioctl or an R7xx hardware launch experiment.

The AMD [R6xx/R7xx register reference](https://www.x.org/docs/AMD/old/R6xx_3D_Registers.pdf),
printed page 127, defines export base 0x9010 in 256-byte units and export
aperture 0x9014, conditional on MEM_EXPORT_PRESENT. It describes suppressed
out-of-range writes and clamped reads, but does not justify guessing the
size-unit or equality boundary. Those require an exact source/ISA cross-check
and an eventual sentinel readback. No registers were written during this audit.

## Pinned-source correction after the kernel update

The remote machine now runs 7.2.3-zen1-2-zen with matching linux-zen-headers.
Headers include Module.symvers and generated configuration but not r600_cs.c.
The [zen tag v7.2.3-zen1](https://github.com/zen-kernel/zen-kernel/tree/c046c30d42937713681e1fee2e7ad6a8bfb20ac6)
resolves to c046c30d42937713681e1fee2e7ad6a8bfb20ac6. Directly downloaded
`drivers/gpu/drm/radeon/r600_cs.c`, lines 1382-1391, consumes a relocation and
adds `gpu_offset >> 8` to SX_MEMORY_EXPORT_BASE. `reg_srcs/r600` line 443
lists SX_MEMORY_EXPORT_SIZE (0x9014). The track structure and export handler
do not track an export aperture/BO pair or validate the size against that BO.
Userspace must not treat acceptance as a bounds check. The exact size encoding
and safe launch still require implementation evidence before shader writes.

The updated module has srcversion `0AB923142CD537CD5BC2DD3`, matching sysfs,
and compressed SHA-256
`13cc44b07a098e9a9ab1c8a73354ae0d30f84543189441e84bfc136f5c22a0b4`.
Its export handler still compares 0x9010 and performs relocation at .text
0x69ef5..0x69f34. This agrees with the source path; it is not a reproducible
whole-module build comparison. No need to request an unknown local patch just
to locate the export handler. No GPU work was sent by this source/binary audit.

## Assembler encoding oracle

`terakan_shader_generation_test` now assembles an indexed four-DWORD MEM_EXPORT
through the existing bytecode builder, with the gfx level selected for RV710.
The independent literal oracle is `c382a123 9d00f000`: source R5, index R7,
DWORD base 0x123, unused ARRAY_SIZE zero, one burst, mask 0xf, barrier set,
R700 CF opcode 0x3a. It is part of the already registered CPU test in both
meson.build and bin/terakan-test, not a new unregistered executable.

Negative control: temporarily changing the R700 opcode table entry to Evergreen
0x55 produced `c382a123 aa80f000` and exit status 1; restoring 0x3a passed.
The opcode table has no retained edits. This verifies instruction packing only:
the fixture has no initialized GPRs, aperture or launch and must never be
submitted.

## SFN scalar-store path

For `ISA_CC_R700`, `RatInstr::emit_ssbo_store` now emits an indexed
`MemRingOutInstr` with `cf_mem_export`, rather than a RAT store. The class and
assembler were extended narrowly for that opcode: scalar stores use element
size zero, component mask one, and ARRAY_SIZE zero; historic ring exports
retain their four-component / `0xfff` path. Each NIR component remains a
separate one-DWORD export at `byte_offset / 4 + component`.

`terakan_sfn_lowering_test` drives that SFN instruction through the real
assembler and checks `0382a123 9d201000`: indexed scalar R5/R7, base 0x123,
component mask one and final EOP. Negative control: temporarily restoring the
ring ARRAY_SIZE `0xfff` made this exact assertion fail; the zero setting was
restored. This is an encoder/lowering-path test, not a shader execution test.

The lowering rejects a non-zero or dynamic resource selection, or an image
slot offset, on R700 rather than aliasing it to the one SX aperture. It does not configure
SX_MEMORY_EXPORT_BASE/SIZE, establish the aperture's size encoding, lower SSBO
loads/atomics/images, provide ES launch state, or make application dispatch
safe. Therefore it is intentionally not grounds for relaxing submit.
