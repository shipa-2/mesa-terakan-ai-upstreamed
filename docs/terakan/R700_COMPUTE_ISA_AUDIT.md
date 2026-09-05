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

The inspected upstream Linux [r600_cs.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/radeon/r600_cs.c)
has no DISPATCH handler and rejects unhandled packet-3 opcodes. Its register
checker accepts the safe bitmap or a handled special register; the
[safe-register input](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/radeon/reg_srcs/r600)
does not list SX_MEMORY_EXPORT_BASE (0x9010), and the parser has no named SX
memory-export handler. A MEM_EXPORT opcode alone therefore does not establish
a usable, BO-bounded userspace export path. Audit a pinned kernel version and
the numeric register cases before proposing a kernel change.

This inspection was of upstream source on 2026-09-05, not the exact remote
7.1.2-zen3-1-zen source: its installed build trees do not contain r600_cs.c.
No rejected dispatch or export-register write was submitted to RV710 to test
this inference. No kernel modification or validation bypass is authorized by
this note. The next implementation must resolve both launch and bounded output,
not just supply a shader with the right ISA opcode.

## Remote module correction (2026-09-06)

Do not infer that the installed remote kernel rejects SX exports from the upstream
audit above. Read-only inspection of its actual `radeon.ko` found different code.
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
