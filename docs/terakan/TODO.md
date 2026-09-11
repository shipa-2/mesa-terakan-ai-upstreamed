# Terakan development TODO

Last updated: 2026-08-23. Primary target: stable game rendering on AMD
CAICOS with DXVK-Sarek.

Importance measures the expected effect on real games. Complexity includes
implementation, hardware research, and the CAICOS regression test needed to
accept the work. Both use a 1–5 scale.

## P0 — game-rendering blockers

| Work item | Importance | Complexity | Feasibility | Acceptance criteria |
|---|---:|---:|---|---|
| ~~Implement and validate FMASK/CMASK allocation, identity initialization and sampled MSAA addressing~~ **FIXED** | 5/5 | 5/5 | **Acceptance criteria met.** Per-sample reads pass at 2x/4x/8x (`terakan_color_msaa_fetch_2x`/`_4x`/`_8x`, each sample given its own distinct colour so a fetch landing on the wrong plane is caught), resolved reads pass at 2x/4x/8x against a genuinely multi-valued surface whose mean differs from every individual sample (`terakan_color_resolve_multivalued_2x`/`_4x`/`_8x`, which also keeps CB compression and fast clear enabled and so covers the compressed CB write and CB_RESOLVE path), and the rest of the suite stays green, which is the "without corrupting ordinary color targets" half. Three real bugs were found and fixed -- see the note below | Per-sample reads and resolved reads pass for 2x/4x/8x images without corrupting ordinary color targets |
| ~~Fix stencil-only render targets on combined depth/stencil images~~ **FIXED** | 4/5 | 3/5 | **Root-caused and fixed.** `terakan_hw_config_draw_set_db_depth_stencil_buffer()` stored the two aspects' base addresses *swapped* whenever depth was not bound, so the stencil-only emit path wrote the depth plane's address into `DB_STENCIL_READ_BASE`/`DB_STENCIL_WRITE_BASE` and every stencil write landed in the depth plane. Verified directly with register-level instrumentation: the incoming descriptor had `z_base=0x0`/`stencil_base=0x4` and the emit chose `0x0`. Fixed by storing each base as itself; the emit path never writes the unbound aspect's base registers at all, so the swap accomplished nothing, and the setter's own dedup comparison (which compares each stored base against the incoming descriptor's same-named field) was silently broken by it too. `terakan_stencil_only_render` regression-covers both the previously broken shape and the depth-bound workaround shape, and is a verified negative control: against the unfixed driver it fails 15/16 stencil-only iterations while the depth-bound pass still passes | A stencil-only `vkCmdBeginRendering` (`pDepthAttachment == NULL`, `pStencilAttachment` naming a combined-format image) writes and reads back correctly without a depth attachment bound alongside, at any sample count, repeatably |
| Complete cache and barrier coherency | 5/5 | 4/5 | Implementable. Composition coverage now exists (`terakan_frame_chain` with the render-pass producer, the compute producer, `--compute-ssbo` for a storage-buffer producer/consumer alongside the storage-image one, `--multi-size` for a second, independently sized 4x4 chain recorded within the same per-frame block as the main 16x16 one, and `--compute-multi-pipeline` for six distinct `VkPipeline` objects bound round-robin across frames instead of rebinding one) and all pass, closing every concrete gap the original coverage note named. Whatever remains is narrower than this composition shape, not a specific named scenario | Focused attachment, texture, storage, transfer, graphics/compute and query producer-consumer chains pass without application-specific waits |
| ~~Cover remaining copy, blit and resolve format/subresource combinations~~ **FIXED** | 5/5 | 4/5 | **Acceptance criteria met.** The group was run with the CTS binary that turned out to be available after all, and closed in three steps. `resolve_image` started at 15 passing and 87 failing; two self-inflicted causes went first (an unmeasured requirement that source and destination surface dimensions match, and cross-layer resolve, now done by shifting the destination base), then multisample `vkCmdCopyImage` stopped being a silent no-op for the whole-surface case through CP DMA, then the two remaining shapes were implemented: a multisample colour copy a sample at a time for regions the byte copy cannot express, and a shader resolve for regions whose source and destination offsets differ, with NIR shaders averaging two, four or eight samples. `resolve_image` is now **102 passing and none failing**. Around it: a 7600-case sample of `blit_image` passes 1117 with none failing, a 5906-case sample of `image_to_image` and `depth_stencil_msaa_copy` passes 2823 with none failing, and an 8536-case stride sample of the whole of `api.copy_and_blit` passes 1701 with none failing. Regression-covered by `terakan_copy_image_subresource`, `terakan_color_resolve_subresource`, `terakan_copy_image_multisample` and `terakan_blit_format_matrix` | Boundary tests cover non-zero offsets, partial extents, mip levels, array/3D layers and every advertised compatible format class A 2778-case copy batch (`image_to_image` simple/dimensions/array/cube/3d, and all of `buffer_to_image`, `image_to_buffer`, `buffer_to_buffer`, `buffer_to_depthstencil`) passes **1146 with zero failures**, including 672 `dimensions` cases of varying extents and offsets -- which is the boundary coverage the acceptance criteria name. A sampled `image_to_image.all_formats` run adds 1439 passing and zero failing, so image-to-image copying is clean across the format matrix; blitting is not, see the row above |
| ~~Correct meta blit format coverage~~ | 5/5 | 3/5 | **Acceptance criteria met.** `blit_image.simple_tests` passes 114 with none failing, and it is exactly what the criteria name: mirrored X, Y and XY, mirrored subregions, and the 3D variant of each. `terakan_blit_format_matrix` covers RGBA to BGRA straight and mirrored, and identity R32. The sampled per-format matrix went from 204 failures to 18 through four fixes and three measured withdrawals; what remains of it is 6 3D linear blits, whose cause is identified -- a 3D source is sampled as a 2D array, which has no depth filter -- and which now needs a shader rather than more bytecode | All basic CTS blits pass for RGBA, BGRA, R32, reversed source/destination axes and 3D slices |
| ~~Correct descriptor binding across arrays, slices and update timing~~ **mostly closed** | 5/5 | 3/5 | `dEQP-VK.binding_model` was run for the first time and four separate defects came out of it. Arrays of uniform texel buffers all read element zero, because `nir_chase_binding` collects array indices only for images and samplers and a separate texture is `GLSL_TYPE_TEXTURE`; a dynamically indexed one failed separately, because `TexInstr::emit_buf_txf` read the resource offset from `sampler_offset` and a sampled buffer has no sampler. Non-array storage image views ignored `baseArrayLayer`, because the view type asked for `TEXTURE2D` and the hardware has no slice to select under a non-array resource type; cube storage images lost their face for the same reason. Update after bind was advertised and cannot work, since `terakan_CmdBindDescriptorSets` snapshots the descriptors into the command stream as register state, and has been withdrawn. `shader_access` is now clean at 1470 of 1470 supported storage image cases and 108 of 108 texel buffer array cases, and `api.buffer_view` is at 411 passing and 0 failing. `descriptorset_random` came down from 1752 failing to 649, and two SFN defects reduced out of it took that to 211 and then 52 of 2752. Both were about the index register a resource id is taken from: the pass that inserts the load reused one established inside a conditional block for consumers after it, and the scheduler did not count that register among a `MEM_RAT`'s operands, so a RAT read could be issued ahead of its own `SET_CF_IDX`. **What remains is 52**, all fragment and all `dynindexed` (30) or `runtimesize` (22); `noarray`, `constant`, `unifindexed` and every compute case are clean | Descriptor arrays, single-slice and cube views, and the advertised update timings all behave as specified |

## P1 — broad DXVK and D3D11 compatibility

| Work item | Importance | Complexity | Feasibility | Acceptance criteria |
|---|---:|---:|---|---|
| Enable geometry shaders and complete vertex-pipeline stage plumbing | 4/5 | 5/5 | Hardware-supported; see the vertex pipeline stage survey below | Focused GS tests pass and `geometryShader` is exposed only afterwards |
| Enable tessellation control/evaluation shaders | 4/5 | 5/5 | Hardware-supported; see the vertex pipeline stage survey below | Tessellation limits are reported from tested hardware behavior and representative pipelines pass |
| Complete stream output / transform feedback | 4/5 | 4/5 | Hardware-supported | SFN receives NIR stream-output metadata and D3D11 stream-output workloads pass readback tests |
| ~~Complete storage-image/UAV format and atomic coverage~~ **load/store + atomics covered** | 4/5 | 4/5 | `shaderStorageImageExtendedFormats` is exposed, and both halves of the acceptance criteria now have real coverage. `terakan_storage_format_matrix` walks 28 formats spanning every class Terakan advertises -- UNORM, SNORM, packed 10/11-bit, 16- and 32-bit float, and UINT/SINT at 8/16/32 bits, one to four channels -- storing a known value through a formatless storage image and loading the same texel back in one dispatch. All 28 are advertised as storage images and all 28 round-trip correctly on real CAICOS hardware, with none skipped. `terakan_storage_image_atomic` covers the atomic half (atomicAdd/Min/Max/Exchange across 32768 invocations, exact). What is left is not format breadth but `shaderStorageImageMultisample`, which is its own P1 row below | Every advertised storage-image format passes load, store and applicable integer-atomic tests |
| ~~Fix image clearing, measured under CTS~~ **done** | 4/5 | 4/5 | The whole of `dEQP-VK.api.image_clearing` now passes: **0 failures of 45636**, 29340 passed and 16296 unsupported. The three causes the original sample of 3042 found are all closed. The 117 three-component-format cases were `vkCmdClearColorImage` returning without doing anything; the 35 multisample integer ones were never clear failures at all but the integer resolve returning early, which instrumenting that return confirmed; and the last group, depth/stencil clears coming back as zero on small images, turned out not to be in the clear either -- `vkCmdCopyBufferToImage` recorded its flush in the color field while the barrier after it named a depth image, so the tail of the fill landed on top of the clear. See the section below. | Clears produce the requested value for every advertised format, sample count and subresource range |
| Implement multisample storage images | 4/5 | 4/5 | Implementable with format-aware lowering, FMASK work and RAT validation; single-sample formatless reads/writes now work | Multisample UAV loads/stores pass for every exposed format before the remaining feature bit is enabled |
| ~~Implement extended image gather~~ | 5/5 | 1/5 | Done, and it turned out to be already implemented: the TODO's premise that `LowerTexToBackend::lower_tg4` drops the offset source was wrong -- `finalize()` removes only the coordinate, LOD, bias, comparator and sample index sources, so `nir_tex_src_offset` reaches `TexInstr::Inputs`, where `GATHER4_O` selection for non-constant offsets and constant-offset folding into the TEX instruction fields both already existed. Only the feature bit, the gather offset limits and coverage were missing. `terakan_image_gather` covers all four components, constant offsets at both extremes, a non-constant offset, `textureGatherOffsets` and ordinary offset sampling, checking the gather order and not just the footprint; all fourteen cases passed unchanged, with two negative controls confirming the coverage bites. `maxTexelOffset` was also corrected from 8 to 7: the five-bit `SQ_TEX_WORD2` offset fields carry the value shifted left by one, so +8 was being applied as -8 | Component selection and constant/dynamic offset gather tests pass for all advertised sampled formats |
| Implement vertex-pipeline stores and atomics | 4/5 | 4/5 | Hardware-supported with stage-specific RAT synchronization work | VS/GS/TES storage writes and applicable atomics pass readback and cross-stage visibility tests before exposure |
| Enforce robust buffer and image bounds everywhere | 5/5 | 4/5 | Descriptor bounds are done and regression-covered on all three paths: the SIZE reclamping of the `resource[1]` read path (`terakan_dynamic_offset_bounds`), the UAV/colour path, where `BASE`/`VIEW`/`DIM` are now rebuilt together for the dynamic offset instead of `BASE` being moved on its own (`terakan_dynamic_uav_bounds`), and storage images, where the hardware's own `CB_COLOR*_DIM` and `CB_COLOR*_VIEW` were confirmed to hold stores inside the bound view across mip levels and layer subranges (`terakan_image_bounds`). Vertex fetch now folds the actual binding VA into the pre-R9xx truncation calculation, and the CPU oracle covers both an aligned base and a two-byte binding offset (the latter requires three dwords of truncation); the nineteen `terakan_vertex_fetch_bounds_probe` cases still accept at exactly the size the element needs. Hardware execution of a generic application draw with an unaligned RV710 binding offset remains unverified because the submit guard and the existing draw-path failure are still present | Guard regions remain intact for misaligned, dynamic and end-of-range accesses |
| ~~Integrate query reset/copy/end synchronization with the common barrier machinery~~ | 5/5 | 3/5 | Done, and it turned up more than synchronization: queries had no coverage at all, and `vkCmdCopyQueryPoolResults` had never worked on DRM Radeon (its destination UAV described a colour surface with a pitch of 8, which the kernel rejects, losing the device), while the pipeline-statistics destination offsets were built in `VkQueryPipelineStatisticFlags` bit order and read in hardware counter order. Both fixed. The copy now raises a pending VS partial flush that `vkCmdResetQueryPool`, `vkCmdBeginQuery`, `vkCmdEndQuery` and `vkCmdWriteTimestamp` drain before writing. Note the ordering half is unproven: `terakan_query_sync` still passes with that wait removed, at eight generations of a 64-query pool, so it is kept on the strength of the requirement rather than of the test | Occlusion, timestamp and pipeline-statistics queries pass reuse and cross-stage ordering tests |

### Meta shaders through NIR

Every meta shader in this driver is hand-assembled Evergreen bytecode, per
generation. Four measured gaps -- 3x-expanded clearing and copying, integer
colour resolve, and multisample sub-region copying -- each need a shader that
does not exist, so each has been costed as "write more bytecode by hand" and
deferred on that basis.

`meta/terakan_meta_nir.c` removes that cost. `terakan_shader_impl_compile` turned
out to depend on no Vulkan pipeline object, so a meta shader can be built with
`nir_builder`, run through the driver's own post-link lowering and compiled by
the same backend as application shaders. Two places in the lowering assumed a
pipeline layout and now treat NULL as meaning a meta shader, which changes
nothing for application shaders.

Proven with `TERAKAN_DEBUG_META_NIR_SELFTEST`, which compiles the opaque pixel
shader as NIR at device creation: 2 dwords, NUM_GPRS 1, CB_SHADER_MASK 0xF, one
export -- the same size and shape as the hand-written one, both encoding the
constant into the export's swizzle selects.

The placement work is done too. Device initialization compiles the NIR meta
shaders first, sizes the buffer from the lengths that come back, and copies from
either source in the same loop; `terakan_meta_nir_builders` decides which shader
is which. The opaque pixel shader is the first real entry, and its bytecode was
compared against the hand-written one it replaces: identical export instruction
and identical swizzle selects, differing only in `ELEM_SIZE` and `BARRIER`, which
`sfn_assembler` sets on every export and CF instruction respectively.

The first of the four shaders is written and the gap it closes is closed; see
below. Two things had to be learned for meta NIR doing it, and both are handled
for whatever comes next: the backend wants the fragment position as an input
variable at `VARYING_SLOT_POS` rather than as `load_frag_coord`, and the binding
pass that meta NIR skips is also what records which hardware resource slots a
shader touches, so `terakan_meta_nir_compile` now walks the NIR and records them
itself -- without that every fetch returns zero.

| Gap | Shader needed | Worth |
|---|---|---|
| 3x-expanded clearing | write three components per texel at `x % 3` | 117 CTS cases |
| ~~Integer colour resolve~~ | **Done** -- the first shader written this way | 35 CTS cases, all now passing |
| Multisample sub-region copy | per-sample copy with FMASK | 24 CTS cases |
| 3D blit depth filtering | sample the source as 3D rather than a 2D array | 6 CTS cases |

## P2 — optional Vulkan functionality

These are useful, but Vulkan 1.1 permits the corresponding feature bits to be
`VK_FALSE`. They do not block a legal Vulkan 1.1 capability report.

| Work item | Importance | Complexity | Feasibility | Notes |
|---|---:|---:|---|---|
| Lower 64-bit buffer accesses and finish 64-bit shader coverage | 2/5 | 4/5 | Partly implementable through 32-bit lowering | Native performance will remain limited |
| Add 16-bit storage support | 2/5 | 4/5 | Implementable through packing/lowering where necessary | Rare in the current DXVK-Sarek target set |
| Add multiview | 1/5 | 3/5 | Implementable | Primarily useful for VR and Vulkan-native applications |
| Add sampler YCbCr conversion | 1/5 | 3/5 | Implementable, likely shader-assisted | Primarily video-oriented |
| Add variable pointers | 1/5 | 4/5 | Potentially implementable through lowering | Low game priority |
| Complete shader float-control rounding modes | 1/5 | 3/5 | Partly hardware-limited | Expose only modes verified on R8xx |
| Complete device-group compute system values | 1/5 | 3/5 | Implementable with explicit driver constants | `dispatch_base`, `dispatch_base_maintenance5` when exposed, and `device_index` pass |

## Not planned for CAICOS

| Item | Importance | Reason |
|---|---:|---|
| Protected memory | 0/5 | The required protected execution and allocation model is unavailable with this hardware and the `radeon` kernel interface |
| Native high-performance FP64 | 1/5 | CAICOS lacks the hardware needed for a useful implementation; software lowering may be used only where practical |

## TeraScale 1 (R600/R700) port

A separate, secondary target from the CAICOS-focused P0-P2 lists above: real
R700 (RV710) hardware is now available for testing alongside CAICOS. TeraScale
1 needs a genuinely separate code path throughout, not just different register
values plugged into the R8xx/R9xx ones -- it has no tessellator, a fixed
64-lane wavefront, and a differently-shaped `SQ_THREAD_RESOURCE_MGMT`/
`SQ_GPR_RESOURCE_MGMT` register set. See the `terascale_1` member of
`terakan_physical_device_chip_info` for what is already ported (physical
device enumeration and property reporting) and its reference source
(`r600_init_atom_start_cs()` in `src/gallium/drivers/r600/r600_state.c`, the
classic Gallium R600 driver that has supported this hardware for years).

| Work item | Importance | Complexity | Feasibility | Acceptance criteria |
|---|---:|---:|---|---|
| ~~Determine real per-family render backend counts~~ | 5/5 | 2/5 | Done: `max_render_backends_log2` is now populated from `RADEON_INFO_NUM_BACKENDS`, queried by the drm_radeon winsys for TeraScale 1 devices and required to succeed, matching the classic R600 driver's own unconditional query-and-fail-if-missing behavior. Confirmed on real RV710 hardware (ioctl reports 1 backend). R8xx/R9xx keep their existing static table, untouched | A real value backs `max_render_backends_log2` for every recognized TeraScale 1 chip family, sourced from kernel query or documented per-family reference, not guessed |
| Wire the TeraScale 1 register-emission helpers into command buffer recording, keep surveying and porting per-draw CB/DB state, and build command stream submission | 5/5 | 5/5 | Minimal logical-device creation is enabled on hardware-validated R600 and R700: physical-device ISA selection, conservative maximum BO alignment and internal allocations are derived from the actual chip family and DRM-reported tiling/backend topology. Real RV610 `1002:94c1` passes create/destroy, basic linear/tiled image create/layout/allocate/bind, real 1x and 4x application VS/FS pipeline compilation and the guarded-submit negative check in ten consecutive runs. This does **not** validate command packets or rendered output on R600. The begin-command-buffer atom is wired to the separate TeraScale 1 SQ/default-register writers; the R600 gfx-level path now also places the required `PKT3_START_3D_CMDBUF` before `CONTEXT_CONTROL`, while R700 emits neither dword because the opcode was removed there. R600 rasterizer discard is translated from the common software `PA_CL_CLIP_CNTL.DX_RASTERIZATION_KILL` intent to `SX_MISC.MULTIPASS`, following `r600_create_rs_state()`; R700 retains its own clip-control bit. Both are exact packet-oracle results only, not submitted on RV610. R700 `CB_COLOR_CONTROL` now repacks the proven `DISABLE`/`NORMAL`/`RESOLVE_BOX`, ROP3, degamma and per-target blend-enable fields rather than passing Evergreen `MODE` through the same address; its `CB_BLENDn_CONTROL` packets strip Evergreen-only enable bit 30 and move those enables into `TARGET_BLEND_ENABLE`, following `r600_create_blend_state_mode()`. R700 `DB_ALPHA_TO_MASK` is emitted at its real `0x028D44` offset with the classic driver's regular `2,2,2,2` sample offsets, not Evergreen's potentially dithered value at `0x028B70`. The no-query, no-HTILE `DB_RENDER_CONTROL/DB_RENDER_OVERRIDE` baseline now also matches `r600_emit_db_misc_state()` (`ZPASS_INCREMENT_DISABLE`, HiZ and both HiS directions forced off); nonzero Evergreen-shaped values are rejected pending individual query/HTILE/copy ports. `DB_SHADER_CONTROL` now repacks only the six fields whose bit positions match in both headers, accepts `DB_SOURCE_FORMAT` solely as validated Evergreen software metadata, and rejects rather than emits Evergreen-only execution/alpha/conservative-Z state. Conservative-Z still needs its classic-R700 placement in `DB_RENDER_CONTROL`; early-fragment-tests and shader memory side effects remain unsupported. R700 pixel-shader SPI state now follows `r600_update_ps_state()`: interpolation location/mode is packed into each `SPI_PS_INPUT_CNTL_n`, `SPI_PS_IN_CONTROL_0/1` and `SPI_INPUT_Z` use the classic fields, and the emitter does not write Evergreen `SPI_BARYC_CNTL` to R700's conflicting `SPI_FOG_FUNC_SCALE` address. Its packet layout has an exact CPU oracle, and real RV710 still compiles the application VS/FS pipeline, but no SPI packet has been submitted or read back. `MULTIWRITE_ENABLE` is deliberately still clear pending the shader-export/MRT survey, and Evergreen eliminate/decompress modes are safely disabled because no exact classic mapping has been established. The first RV710 meta-draw diagnostic found and fixes the parser-required placement of DRM relocation NOPs directly after each R700 `CB_COLOR{BASE,FRAG,TILE}` address; its CPU oracle fails when the first NOP is moved, but the corrected stream is still awaiting a hardware submission result. Queue submission still returns `VK_ERROR_DEVICE_LOST` before the winsys because the remaining per-draw packets and meta shaders are not hardware-validated. Real RV710 `1002:954f` passes create/destroy and the guarded-submit negative check; the new CB/DB/SPI control work is packet-tested only, not submitted. | A trivial graphics submission completes on R700 with register and memory readback checks |
| TeraScale 1 tiling/surface addressing (bank/pipe swizzle, macro-tile layout) | 5/5 | 5/5 | Layout is wired into image creation: linear/1D/2D alignment, mip offsets, 2D-to-1D degradation, array/3D sizing, block-compressed/subsampled texel-to-block conversion and linear 3x-expanded formats follow classic `radeon_surface.c` plus AddrLib's documented 3x pitch ordering. Aspect bases are independently aligned, preventing depth/stencil overlap. Real RV710 create/layout/allocate/bind checks pass for linear RGBA8/R32G32B32 and tiled RGBA8/BC1 mip chains. R600/R700 MSAA color images now allocate the classic FMASK/CMASK pair: FMASK follows `r600_texture_get_fmask_info()`'s R600-R700 doubled-BPE workaround, CMASK follows `r600_texture_get_cmask_info()`, and the TeraScale 1 CB encoder writes `FRAG_ENABLE`, `FRAG`, `TILE` and `MASK` as an inseparable pair. R600/R700 depth-only DB descriptors now accept 2x/4x/8x too: `r600_init_depth_surface()` has no DB sample-count bit, so the sample count is kept solely in `PA_SC_AA_CONFIG` and the larger tiled pitch/slice allocation feeds the ordinary DB registers. Real RV610 creation/allocation/binding of a 1024² RGBA8 2x target reports 10,493,952 bytes, strictly beyond its 8,388,608-byte main surface; disabling the allocator restores exactly 8,388,608 and makes the hardware smoke test fail. Its 1024² D32 4x target also creates, allocates and binds (16,777,216 bytes, 8 KiB alignment). These prove allocation and descriptor packet construction only. FMASK tile swizzle, CB/DB rendering/resolve, per-sample fetch, metadata initialization and GPU copy/readback remain unverified because TeraScale 1 submit is deliberately blocked. UAVs, packed depth/stencil and HTILE remain open too. | A buffer/image round trip through the tiled surface layout matches, the same class of check `terakan_image.c` already does for R8xx/R9xx |
| Replace the hand-written Evergreen meta shader bytecode (blit/resolve/clear/copy/query, all of `src/amd/terascale/vulkan/meta/`) with generation-neutral NIR | 4/5 | 5/5 | `terakan_meta_nir_compile()` now lets meta operations use `nir_builder` and compile through SFN with the runtime chip generation, including `ISA_CC_R700`; it also performs the input-variable and resource-binding setup required by this backend. The first NIR meta shader is in use, but the remaining hand-assembled Evergreen shaders still need conversion and R700 readback validation | Each meta operation this driver depends on (at minimum blit and clear) passes its existing readback test on TeraScale 1 hardware |
| Recognize R600/R700 PCI IDs and correctly plumb `gfx_level` through the NIR shader path for real application shaders | 4/5 | 2/5 | Production PCI lookup and `terakan_shader_generation_test` now consume the same complete `r600_pci_ids.h` table. The actual runtime family selects `R600`/`ISA_CC_R600`, `R700`/`ISA_CC_R700`, Evergreen or Cayman for NIR optimization, SFN translation, bytecode assembly and ISA initialization. NIR compiler options now follow classic `r600_screen_create()` too: pre-Evergreen lowers bit count/reverse, unrolls indirect sampler indexing, does not advertise BFE/BFM/BFI, and keeps non-zero-based vertex IDs; Evergreen behavior is unchanged. The existing R700 enumeration test additionally creates a real application VS/FS graphics pipeline without submitting it. Pipeline compilation is hardware-tested separately on RV710; rendering remains blocked by the deliberate submit guard | A real application vertex/fragment shader renders correctly on TeraScale 1 after command submission is safely enabled |

Clarification for the long per-draw summary above: its earlier sentence saying that
conservative-Z still needs a classic-R700 placement is superseded. Valid
`ANY`/`LESS`/`GREATER` modes now go through paired R700
`DB_RENDER_CONTROL`/`DB_RENDER_OVERRIDE` state; R600 admits only `ANY`.
This is packet-oracle coverage only, not a submitted rendering result.

TeraScale 1 submission has a deliberately narrow diagnostic opt-in: only
`TERAKAN_DEBUG_TERASCALE_1_SUBMIT=1` reaches the winsys. With the variable absent, or with any
other value including `0`, `terakan_queue_submit` still returns `VK_ERROR_DEVICE_LOST`. This is
only B1 of the first-execution plan: it is not a claim that any command buffer is safe to submit.
On RV710, the first empty-buffer experiment exposed two separate facts: the kernel first rejected
Evergreen-only `PKT3_PFP_SYNC_ME` (`0x42`), so TeraScale 1 now omits it; after that correction the
kernel accepted the IB but ring 0 locked up and its fence timed out. The offline audit also found
`EVENT_TYPE_CS_PARTIAL_FLUSH` (`0x07`) marked `eg+` in `r600d.h`; unlike the former packet, it
passed validation and therefore could plausibly have caused the stall. TeraScale 1 now emits only
the `PS_PARTIAL_FLUSH` used by `r600_init_atom_start_cs()`. The same audit removed the
Evergreen-only `CB8...CB11_DEST_BASE_ENA` bits from the end-of-recording `SURFACE_SYNC`, matching
the CB0...CB7 subset in `r600_flush_emit()`. A repeatable dry-run
`TERAKAN_DEBUG_DRY_RUN_SUBMIT=1` paired with `TERAKAN_DUMP_CS=1` records the exact IB without
issuing `DRM_RADEON_CS`; on RV710 it proved the 216-dword post-change stream contains no `0x07`
event and carries `CP_COHER_CNTL = 0x9E807FC0` rather than the old `0x9E87FFC0`.

Fence completion has a second, aligned IB generated in `terakan_queue.c`, not the recorded
command-buffer IB. A fence gives it `ALL_COMMANDS` even for an empty submit, which had made it
reintroduce the same `eg+` CS flush and CB8...CB11 bits. The existing RV710 probe accepts the
additional exact opt-in `TERAKAN_DEBUG_TERASCALE_1_SIGNAL_ONLY=1`; with `DRY_RUN` it produced a
16-dword signal IB containing `PS_PARTIAL_FLUSH` (`0x16`) and `CP_COHER_CNTL = 0x86007FC0`, with
no CS flush or CB8...CB11 bits. This validates only the chosen packet fields, not a completed
fence or cache coherency. One real signal-only attempt on RV710 then returned from `vkQueueSubmit`
and its five-second fence wait successfully, but the kernel journal in the same wall-clock window
also contained a ring reset and lockup report whose reported stall duration predated the attempt.
That is not a clean survival result and must not be generalized or repeated: B2 remains open until
the recorded-command-buffer path and a data readback are independently clean.

`TERAKAN_DEBUG_TERASCALE_1_PREAMBLE_ONLY=1` is the next recorded-IB differential: it creates the
IB with one NOP, causing the transcribed context defaults to be emitted, but excludes the unrelated
host-to-device cache tail. RV710 dry-run produced 208 dwords (the final two are alignment NOPs),
and no `PKT3_SURFACE_SYNC` tail. The real RV710 fence then completed once, followed by 5/5 further
clean repetitions with no kernel-journal entries after the series. This establishes the
start-atom/prelude portion of B2; it is deliberately test-only and not a cache-coherency mode.
It does not validate the omitted normal cache tail, any draw/copy/readback, or rendering.

The normal 216-dword empty command-buffer path, including that cache tail, subsequently completed
once and then 5/5 further times on RV710, again with no kernel-journal entries after either run.
This closes **B2 only**: a normal empty recorded IB plus its fence completion is now validated on
hardware. It does not lift the default `VK_ERROR_DEVICE_LOST` guard or validate any GPU work;
CP DMA/readback, descriptors and every draw/dispatch packet remain separate gates before general
submission can be enabled.

The first B3 CP-DMA packet constraint is now transcribed from classic
`r600_cp_dma_copy_buffer()`: R600/R700 caps `BYTE_COUNT` at `2^21 - 8`, rather than the nominal
all-ones 21-bit value retained by the Evergreen path. This matters for four-byte fills: the exact
R700 maximum is `0x001ffff8`, while the old generic cap would emit `0x001ffffc`; 32-byte-aligned
copies round both to `0x001fffe0`. `terakan_cp_dma_terascale_1_test` checks all three values and
has the old fill value as a negative control. It proves only packet-size selection on the CPU;
the hardware evidence is split below by operation, and does not turn the unsupported fill path
into a supported one. B3 and the submit guard therefore remain open for the unvalidated classes.

That first B3 transfer is now hardware-validated on RV710: the opt-in probe copied 64 bytes from
one host-visible buffer to another, waited for the submission fence, invalidated the destination,
and compared every dword. The destination was initialized to the inverse pattern, so a skipped
copy is a negative control rather than a possible false pass. One initial run and five consecutive
repetitions all matched, with no kernel-journal entries after the series. This establishes only an
aligned buffer-to-buffer CP-DMA copy with host readback. Unaligned copies, fills, images, cache
transitions under GPU work, descriptors and draws remain unvalidated; the default submit guard
therefore remains in place.

The unaligned branch of B3 is separately checked on RV710: source offset 4 and 60 copied bytes
force a 28-byte source head, one 32-byte bulk packet and the four-byte discard tail that restores
the internal CP-DMA alignment counter. Every copied dword matched and the untouched final
destination dword retained its inverse-pattern sentinel in five consecutive runs; the kernel
journal stayed clean. This remains a boundary check, not a substitute for image copy validation.

The new `TERAKAN_DEBUG_TERASCALE_1_CP_DMA_LARGE_COPY=1` probe crosses the R600/R700 packet limit:
it copies 2 MiB, eight bytes beyond the `2^21 - 8` maximum, so the command writer must emit a
legal full-size packet followed by the remainder. Five consecutive RV710 runs matched all 524288
dwords; the inverse destination pattern provided the skipped-transfer negative control and the
kernel journal was empty for the series. This is a 2 MiB linear buffer split/readback on RV710,
not evidence for image CP-DMA, fill packets, cache transitions, or general submission safety. The
default submit guard remains.

The opt-in CP-DMA fill probe (`TERAKAN_DEBUG_TERASCALE_1_CP_DMA_FILL=1`) is deliberately not
counted as working on RV710: three consecutive attempts returned `VK_ERROR_UNKNOWN` (`-13`) from
the transfer submission path, with no kernel journal entry. The existing copy/readback probes are
still valid, but fill remains unsupported. The classic source confirms why: `r600_clear_buffer()`
uses `evergreen_cp_dma_clear_buffer()` only for `gfx_level >= EVERGREEN`; R600/R700 take the
streamout/blitter fallback, so there is no classic R700 CP-DMA fill packet to transcribe. The
submit guard is unchanged.

The first graphics-state submission has now passed the RV710 kernel parser and fence through the
complete draw-state and SQK emission when the draw packet itself is replaced by TYPE2 padding.
The earlier apparent failure at `CB_COLORn_INFO` was a localization error: the passing prefix
already contained the same INFO write, while the next entry appended Evergreen-only
`EG_PKT3_INDEX_BUFFER_SIZE` (opcode `0x13`) for an unbound index buffer. Classic
`r600_draw_vbo()` places a direct indexed address in `PKT3_DRAW_INDEX` and emits no unbind packet;
omitting opcode `0x13` made both state stages complete on RV710. This does not validate bound or
indexed application draws.

A real three-index meta draw is accepted by the parser but locks ring 0. A zero-count
`PKT3_DRAW_INDEX_AUTO` isolator was added to distinguish draw/state validation from shader-wave
execution. Its first apparent success overlapped the delayed reset from the preceding lockup, and
subsequent attempts timed out while the adapter repeatedly failed its post-reset IB self-test.
Two clean-boot attempts have now bounded the zero-count behavior more precisely. With shared-state
entry 0 only (the pipeline-stat event), the submission and fence complete. Adding entry 1 makes the
zero-count draw lock ring 0; however, the same `VGT_PRIMITIVE_TYPE = RECTLIST` register packet with
the draw replaced by TYPE2 padding completes cleanly, so the register packet by itself is not the
cause. On another clean boot, skipping entry 1 but adding the remaining shared entries also locks
the zero-count draw. Therefore a zero-count `DRAW_INDEX_AUTO` on RV710 is not a valid safety oracle
once normal draw state has been configured. These are single clean-boot observations per variant,
not repetition-qualified rendering results. Continue localization with TYPE2 for state-only checks
and audit the real nonzero draw path against Gallium; do not infer shader-wave behavior from the
zero-count packet.

The first two shaders needed by that clear no longer reuse their hand-written Evergreen bytecode
on TeraScale 1. The packed-index position VS (including the layered variant) and constant-colour
PS are now built as NIR and compiled by SFN with the runtime family. RV710 device creation proves
that all three compile with `ISA_CC_R700`; a kernel-facing dry-run also shows generated R700 state
replacing the old table (`SQ_PGM_RESOURCES_VS` changed from `0x00200001` to `0x00200102`, and
`SPI_PS_IN_CONTROL_0` from `0x20000001` to `0x10000000`). This is compilation and packet
characterization only. The GPU was still in a repeated post-lockup reset/self-test loop, so no
execution or readback claim is made.

After a clean reboot, the NIR-generated three-index clear completed its fence six times without a
kernel error, but initially left all four distinct host-written RGBA8 texels unchanged. A dedicated
vertex-bytecode diagnostic confirms that the R700 meta VS reads the immediate index from `R0.x`,
unpacks its two 16-bit coordinates and exports `POS0 = xy01`; therefore source NIR compilation and
the position export are not the explanation for that silent no-op. Removing the unused single-slice
layer export also changed nothing, and adding the required transfer-write-to-host-read barrier did
emit the cache flush and `SURFACE_SYNC` tail but still changed no texel.

Two omissions relative to the classic framebuffer path were then corrected. R600/R700 now writes
`CB_SHADER_CONTROL`, enabling all RT slots through the highest bound color target as
`r600_emit_framebuffer_state()` does; this was required state, but on its own still produced four
unchanged texels. More importantly, the TeraScale 1 context baseline had never initialized
`PA_SC_WINDOW_SCISSOR_TL/BR`: the classic driver writes it for every framebuffer, while Terakan's
actual render-area clipping uses `VPORT_SCISSOR`. Initializing the outer window scissor to the same
8192-by-8192 limit as the classic start atom's screen/generic scissors made the 2-by-2 linear RGBA8
clear and host readback pass on real RV710. It passed once immediately after the remote source build
and then 10/10 repeated runs, with no new radeon kernel-journal error. The four input texels are all
different and none equals the requested `0xff00ff00`, so a skipped draw cannot pass this check.
This proves one single-sample linear clear through CB and its host-visible cache tail. It does not
yet prove texture sampling, image-to-buffer meta copying, tiled CB addressing, layers, MSAA,
application rendering or general queue safety; the default TeraScale 1 submit guard remains.
The separate `TERAKAN_DEBUG_TERASCALE_1_META_STATE_ONLY=1` variant replaces the meta draw packet
with TYPE2 padding and has its own inverse-sentinel oracle: the 2x2 source remains unchanged while
the fence completes. Five consecutive RV710 runs passed with no kernel journal entries. This
validates acceptance of the emitted R700 meta state/preamble only, not shader execution or
render-target writes, and does not alter the normal clear expectation.
The opt-in `TERAKAN_DEBUG_TERASCALE_1_TILED_IMAGE_CLEAR=1` variant now exercises the same clear
shader against an optimal (microtiled 2x2) color image, then copies that image into the linear
host-visible target for readback. Five consecutive RV710 runs produced the requested green word at
every texel, while the inverse linear-image sentinel detects a skipped clear or copy; the kernel
journal stayed empty. This is the first tiled CB clear/readback result, still limited to one
single-sample RGBA8 level and not evidence for macrotiles, layers, MSAA or general submission.
The same probe now has `macro` and `edge` modes for 128x128 and 129x65 optimal images. Each mode
passed five consecutive RV710 runs, including the padded edge extent, with no kernel-journal
entries; every texel matched the clear value and the inverse sentinel remained an effective
skipped-operation control. This closes only the level-zero single-sample RGBA8 macro CB readback
shape, not array-layer bank rotation, MSAA metadata or arbitrary formats.
The `layer` and `layer-negative` modes extend this to a two-layer 129x65 optimal image: layer 0 is
cleared magenta, layer 1 green, and the selected layer is copied to the linear target. Five runs
of each mode passed on RV710 with no kernel-journal entries; the negative mode observed all 8385
expected mismatches when it deliberately selected layer 0. The first attempt exposed a test-only
omission of `levelCount=1` in the explicit clear ranges and failed by reading the magenta sentinel;
that was corrected before the reported series. This is evidence for one array-layer/bank-rotation
boundary, not combined mip/layer addressing, MSAA or general submission.

The `mip-layer` and `mip-layer-negative` clear modes then selected level 1/layer 1 versus level
0/layer 0 in the same 129x65 two-level, two-layer image. Five runs of each mode passed and the
negative mode observed all 8385 expected mismatches every time, with no kernel-journal entries; mismatch logging is
suppressed for that intentional negative control. This is a combined selector result, not a claim
that every mip/layer layout or format is correct.

The opt-in `TERAKAN_DEBUG_TERASCALE_1_MSAA_IMAGE_CLEAR=2` probe did not establish an R700
resolve path. On RV710 it reached FMASK/CMASK initialization and the kernel rejected the
Evergreen-shaped CP-DMA fill with `CP DMA src buffer too small (67380228 20480)`, returning
`VK_ERROR_DEVICE_LOST`. This was a safety failure, not a readback result, and the packet was not
repeated. `terakan_barrier_initialize_color_metadata()` now records a command-buffer error for
TeraScale 1 before emitting that fill; MSAA color metadata initialization, clear and resolve remain
unsupported until a R700-specific mechanism is implemented and read back. The ordinary submit
guard is unchanged. After rebuilding this guard on RV710, the same opt-in probe stopped with
`VK_ERROR_UNKNOWN` during command recording and produced no new `CP DMA`, `Invalid command
stream`, ring or GPU-reset journal entry; this verifies the safety boundary, not the operation.

The next image-to-buffer attempt established a separate safety boundary. Its generation-neutral
NIR shader still writes the destination with `MEM_RAT`, but the R600/R700 CB converter deliberately
rejects Evergreen-shaped buffer UAV descriptors. Letting the draw continue with that UAV unbound
locked RV710 ring 0 and timed out with `VK_ERROR_DEVICE_LOST`; therefore this is not a usable route
to R700 readback. Linear levels now take a row-wise raw CP-DMA path instead, using the already
validated surface pitch/slice layout and Vulkan's buffer row/slice pitches. Unsupported tiled or
otherwise invalid TeraScale 1 regions fail command-buffer recording rather than entering RAT.
The 2-by-2 RGBA8 image-to-buffer probe passed once after rebuilding this diff from source on RV710
and then 10/10 repeated runs, with four distinct image words replacing four different inverse
buffer sentinels and no new kernel error. This proves the linear level-zero, single-layer case only;
mip offsets, layers, compressed formats and tiled detiling still need focused readbacks.

The inverse 2-by-2 linear buffer-to-image copy is now also hardware-validated on RV710. The first
attempt exposed two descriptor-port boundaries: the shared bookkeeping emitted Evergreen's fetch
shader resource base 992 where R600/R700 uses 320, which the kernel rejected as `bad SET_RESOURCE`;
after translating the PS/VS/GS/FS resource ranges, the parser accepted the IB, but executing the
still-handwritten Evergreen buffer-to-image meta shader locked ring 0. R600/R700 buffer resource
word 3 is now kept zero as classic `texture_buffer_sampler_view()` requires rather than inheriting
Evergreen destination-selection bits, but a shader fetch through that descriptor has not yet
completed and therefore is not claimed working. Linear TeraScale 1 uploads instead use a preflighted
row-wise CP-DMA path and reject tiled/invalid regions without falling through to the unsafe shader.
Four distinct buffer words replaced four inverse image sentinels once and in 10/10 repetitions,
with no new RV710 kernel error. This proves only level-zero, single-layer linear RGBA8 upload; mip
offsets, layers, compressed/3x formats, tiled addressing and the eventual NIR replacement for the
generic meta shader remain unverified. The default submit guard remains.

The buffer-to-image meta shader now also has a TeraScale 1 NIR implementation compiled by the
runtime-selected R700 SFN/assembler path instead of reusing either handwritten Evergreen program.
Its disassembly is a 28-dword PS containing one `VFETCH RID:0` with `USE_CONST_FIELDS` and one
colour export. With a diagnostic switch bypassing the linear CP-DMA shortcut, the same four-word
RV710 upload/readback passed once and then 10/10 repetitions with a clean kernel journal. Keeping
the old handwritten program is the hardware negative control: with the same now-parser-valid
resource packet it locked ring 0. This validates a single linear RGBA8 buffer fetch, address
calculation and CB export; it does not validate other buffer formats, swizzles, offsets, layers or
tiled destinations, so linear uploads continue to use the already validated CP-DMA path and the
NIR route is not yet selected for general TeraScale 1 copies.

The single-sample copy-image PS now has a T1-only NIR builder with explicit integer coordinates
and LOD zero. The opt-in enumeration probe `TERAKAN_DEBUG_TERASCALE_1_TILED_IMAGE_ROUNDTRIP=1`
(also requiring `TERAKAN_DEBUG_TERASCALE_1_SUBMIT=1` and
`TERAKAN_DEBUG_TERASCALE_1_BUFFER_UPLOAD_NIR=1`) uploads four distinct RGBA8 words into a 2x2
optimal image, copies it into a linear image and checks all four words against inverse sentinels.
This small optimal image is **1D microtiled**, not a macrotiling test. On remote RV710 1002:954f,
restored source passed 10/10; a shader mutation fetching (0,0) for every pixel failed 3/3 with
exactly three mismatches per run. The kernel journal stayed clean. The existing sampler CPU test
also checks the exact three-dword meta sampler payload; changing TYPE to zero failed that oracle.
Both tests are already registered in Meson and bin/terakan-test.

An important limitation of the investigation: the initial texture reads returned zero, whereas
writing sampler 0 explicitly produced correct results. After that, omitting the write passed
10/10 too. Therefore a particular sampler field is NOT established as the root cause; inherited
GPU state still needs a cold-start investigation. The copy now explicitly sets/restores a
point/repeat, LOD-zero sampler with TYPE=1 as in r600_create_sampler_state(), and T1 meta LD usage
tracks its sampler. The CPU TYPE mutation proves encoding only, not an LD requirement for TYPE.
An earlier successful linear-to-linear comparison used CP DMA and did not validate texture
fetching; the successful microtiled-to-linear path cannot take that shortcut. These results do
not validate macrotiles, bank rotation, nonzero mip/layer/region offsets, other formats, filtered
sampling, MSAA or RV610. The default submit guard and general tiled buffer-upload guard remain.

The same opt-in probe now accepts `TERAKAN_DEBUG_TERASCALE_1_MACROTILED_ROUNDTRIP=1` (128x128)
or `edge` (129x65), with a distinct 32-bit pattern per pixel. RV710 source builds passed each
size once and then 10/10 after the negative control. Reducing the copy width by one, while
still checking the entire destination, failed 3/3 per size with exactly 128 and 65 inverse
sentinels remaining. IB dumps confirm TILE_MODE=4 (2D_TILED_THIN1), rather than assuming that
an optimal VkImage necessarily stays macrotiled. This establishes level-zero RGBA8 CB-to-TC
roundtrips across macrotiles, including an unaligned extent and padded pitch. It still does
not establish the CPU address formula independently: matching CB and TC layout interpretations
could hide a shared mistake. Bank rotation between layers, nonzero mips/offsets, other formats
and cold-start sampler state remain unverified. No additional driver path or submit guard was
enabled by this test extension; the existing enumeration test is already in both test lists.

`TERAKAN_DEBUG_TERASCALE_1_MACROTILED_ROUNDTRIP=offset` additionally copies a 125x62 rectangle
from (1,2) to (3,1) in 129x65 images: the NIR constants include a negative X delta (-2) and
positive Y delta (+1). The CPU checks all 7750 copied pixels and all 635 inverse sentinels
outside the rectangle. RV710 passed once plus 10/10 restored runs, with no kernel messages.
Changing source X from 1 to 2 failed 3/3 with 7750 mismatches; widening the destination by one
column failed 3/3 with exactly 62 sentinel mismatches. Neither mutation changes the oracle.
This covers nonzero XY copy offsets and preservation outside that rectangle, not mip/layer
addressing, filtered sampling, CPU swizzle equations or cold-start initialization.

The `mip` variant uses mip 1 (129x65) of a 258x130 two-level optimal source, while the linear
destination stays at mip 0. Mip 0 of the source is first cleared magenta; mip 1 receives the
per-pixel pattern. The captured T# has BASE_LEVEL=LAST_LEVEL=1, TILE_MODE=4 and mip address
0x360 (0x36000 bytes before relocation), consistent with the CPU surface layout. On RV710 the
restored probe passed 10/10. Reading source mip 0 instead, without changing the oracle, failed
3/3 with all 8385 values magenta. This distinguishes the two levels rather than relying only
on a same-level upload/readback that could alias both to level 0. Kernel journal stayed clean.
This covers one nonzero mip and one RGBA8 extent, not the full mip chain, small-mip degradation,
other formats, array layers or CPU per-pixel tiled address equations.

The `layer` variant similarly uses a two-layer 129x65 optimal source. It clears layer 0 magenta,
uploads the distinct per-pixel pattern only to layer 1, and copies layer 1 to a linear destination.
The current RV710 source build passed 3/3 with a clean kernel journal. Its `layer-negative`
hardware control changes only the image-copy source layer to 0 while retaining the layer-1 oracle:
it observed exactly 8385 mismatches (all pixels) and then reports PASS. This makes layer selection
observable and covers one array-layer/bank-rotation boundary; it does not establish combined mip
and layer addressing, other formats, 3D slices, CPU tiled-address equations, or general submission.
The combined `mip-layer` variant selects level 1/layer 1 of a two-level, two-layer source and
passed 3/3 on RV710 with no journal messages. Its `mip-layer-negative` control instead copies
level 0/layer 0 while retaining the level-1/layer-1 oracle; it observed all 8385 expected
mismatches and PASS. This is a combined selector check only, not full mip-chain or array support.
The TeraScale 1 linear buffer-image path also operates on compressed blocks: BC1 regions are
translated to 4x4 block coordinates, use `bytes_per_block == 8`, and CP-DMA row/slice pitches are
checked in blocks before emission. The opt-in `TERAKAN_DEBUG_TERASCALE_1_BC1_ROUNDTRIP=1` oracle
now passes on RV710: an 8x8 linear BC1 image (four 8-byte blocks) is initialized with inverse
sentinels, uploaded through the TeraScale 1 CP-DMA path, and all 32 bytes read back correctly.
The `negative` mode uses a 40-byte source buffer with `bufferOffset = 8`; it also passes by
observing the shifted 32-byte image, so ignoring the offset or skipping the transfer is detected.
Both modes pass in the final paired remote run with no new kernel journal entries. This proves
only linear BC1 block addressing/offset handling on RV710; tiled BC1, mip/layer BC1, and format
filtering on other R700 chips remain unverified and tiled BC1 remains explicitly rejected.
The inverse direction is now covered too: `TERAKAN_DEBUG_TERASCALE_1_BC1_ROUNDTRIP=readback`
copies the four blocks from a linear image into a 40-byte inverse buffer, and
`readback-negative` repeats it at `bufferOffset = 8`. Both modes passed on RV710 in the final
paired run with no kernel messages. This adds image-to-buffer evidence only for linear level zero,
single layer; it does not validate tiled, mip/layer BC1 or other R700 devices.
The `readback-mip-layer` variant then uses a 16x16 two-level, two-layer linear image and reads
level 1/layer 1; its negative control reads level 0/layer 0 and observed the expected 32-byte
mismatch. Both passed on RV710 with no kernel messages, demonstrating the linear BC1 mip offset
and layer slice address calculation. Tiled BC1 and other R700 families remain unverified.
The existing CPU tiling test also fixes this exact fixture: 384x144 base, 256x128 padded mip,
0x36000 mip offset and 0x56000 total bytes. Adding 256 bytes to the production mip-offset output
failed its new assertion; restoring the calculation passed. The mutation was CPU-only and was
never submitted to hardware. Both test executables already participate in the normal test lists.

Post-reboot check on 2026-09-05: the user restarted the remote machine (boot ID
31ee52a4-a784-4762-a541-f33929776637). At source commit 3e2aa864832, rebuilt remotely before
any GPU probe from this session, the first 2x2 roundtrip passed. Each of the small, 128x128,
129x65, offset and mip variants then passed 10/10; the kernel journal gained no messages.
This is ONE post-reboot first-run observation followed by 50 warm repetitions, not 51 cold
starts. It shows the explicit-sampler implementation does not require our earlier successful
submissions on this boot. It does not establish the offending old sampler field, rule out
other boot-time GPU users, or reproduce the old no-sampler implementation after reboot.
The original missing-state root cause therefore remains open, while this implementation has
now passed a first-run-after-reboot check on RV710. RV610 was still deliberately skipped.

Direct indexed application draws now follow `r600_draw_vbo()` too: R600/R700 binding emits no
Evergreen `INDEX_BASE`/`INDEX_BUFFER_SIZE`, and `vkCmdDrawIndexed` carries the adjusted absolute
40-bit address in `PKT3_DRAW_INDEX` with an immediate DRM relocation. The exact packet has a CPU
oracle; indirect indexed draws and all indexed hardware execution remain unverified.

The existing `terakan_vertex_fetch_bounds_probe` was attempted on RV710 with
`TERAKAN_TEST_DEVICE="TeraScale 1"` and the submit diagnostic enabled. It selected
`AMD TeraScale 1 RV710 (Terakan)`, but `vkEndCommandBuffer` returned `VK_ERROR_DEVICE_LOST`
before a queue submission, because the probe requires the still-unvalidated application draw
path. The kernel journal remained empty. This is not a vertex-fetch threshold measurement and
does not justify carrying Caicos thresholds to R700; a dedicated probe must first use a validated
TeraScale 1 draw/readback path.

The dynamic binding-offset hole is now handled in the descriptor path: the fetch resource keeps
the static truncation selected by the shader and raises it when the actual 64-bit binding VA makes
the first naturally aligned chunk smaller. `terakan_vertex_input_test` has a positive aligned-base
case and a negative-controlled two-byte-base case (the test fails if the helper is forced to return
zero). This proves only the CPU calculation and descriptor plumbing; no RV710 draw readback with
an unaligned `VkBuffer` offset has been obtained yet.

## Completed and regression-covered

- Multisample correctness, closed as a group. Seven defects, each with a probe or
  a CTS delta and a negative control; the measurements are in
  `docs/terakan/FUNCTIONAL_COVERAGE.md`.

  - **Per-sample fragment invocation.** `PS_ITER_SAMPLES` was derived from
    `sampleShadingEnable` alone, so a shader reading `SampleId` -- which the
    specification says always runs per sample -- ran once per fragment and every
    sample got sample zero's value. That is what the depth and stencil min/max
    resolve failures were, and it also overturned two earlier conclusions about
    the resolve shaders and the hardware, both corrected in the coverage document.
  - **`gl_FragCoord` under sample shading**, which must be the sample's position.
    `SPI_PS_IN_CONTROL_0.POSITION_SAMPLE` is now set both from the shader (when it
    reads `SampleId`) and from the pipeline (when `minSampleShading` is what makes
    it per-sample). At two samples the hardware does not honour it; four and eight
    do.
  - **RTV metadata coherency for the shader resolve**, which samples the source as
    a texture and bypasses CB, so `FLUSH_INV_CB_RTV_META` has to run first.
  - **Compression on multisample images that are read as textures.** An integer
    colour attachment is sampled by the shader resolve, and an input attachment is
    sampled by `nir_lower_input_attachments`, whether or not the application asked
    for `VK_IMAGE_USAGE_SAMPLED_BIT`; both now disable compression the way a
    sampled image already did.
  - **The multisample depth/stencil clear**, which rasterized single-sample and so
    wrote one quarter of a 4x surface. Covered by
    `terakan_depth_stencil_clear_multisample`, which reports 3072 of 4096 samples
    unwritten against the unfixed driver.
  - **Custom sample locations at one sample**, withdrawn from
    `sampleLocationSampleCounts`: the registers are right and the sample does not
    move, measured against four placements in x and in y.
  - **A GPU hang sampling `r32g32b32_sfloat`**, which aborted whole CTS runs.
    `32_32_32` was the only unpacked three-channel format left advertising a
    texture fetch; it is a vertex fetch format, and the surface's row is three
    times as wide in surfels as the descriptor's texel pitch claims.

  A 2641-case multisample and renderpass sample went from 119 failures to 0, and
  an 8630-case stride sample of the whole suite now runs to the end instead of
  aborting.

- Integer sampler border colours: `VK_BORDER_COLOR_INT_OPAQUE_BLACK` and `_WHITE`
  now use the per-sampler border colour registers with the integer values, which
  every 8-bit and 16-bit integer format needed. Reaching that path for the first
  time lost the device -- it emitted `PKT3_SET_CTL_CONST` for what are
  configuration registers, underflowing the offset -- so the packet is corrected
  too. `pipeline.monolithic.sampler` went from 406 failures to 243 over a
  30340-case sample, and every one of the 243 left is an unnormalized-coordinate
  case: `S_03C000_FORCE_UNNORMALIZED` is Cayman-only and nothing normalizes the
  coordinates in the shader, which is the open half.

- TeraScale 1 (R600/R700) physical device enumeration and property reporting:
  chip family recognition now covers `CHIP_R600`..`CHIP_RV740`, not just
  `CHIP_CEDAR`..`CHIP_ARUBA`. `terakan_physical_device_chip_info` gained a
  `terascale_1` member with the real per-family GPR/thread/stack-entry counts
  (transcribed from `r600_init_atom_start_cs()` in
  `src/gallium/drivers/r600/r600_state.c`, not guessed), since TeraScale 1 has
  no tessellator, a fixed 64-lane wavefront, and a flat, single-variant
  register set where R8xx/R9xx has a tessellation-stage-indexed one --
  populating that struct needed a genuinely separate code path through
  `terakan_physical_device_chip_info_init`, not new switch cases plugged into
  the existing one. Minimal `terakan_CreateDevice` bring-up is enabled on
  hardware-validated R600 and R700, while queue submission on both generations
  is stopped before the winsys until each command stream is verified. Verified on real RV610
  (R600) and RV710 (R700) hardware; the R600 check covers only device creation,
  basic image layout/allocation/binding and application shader compilation, not execution;
  `terakan_terascale_1_enumeration` covers it and passes trivially (nothing to
  check) on machines with no TeraScale 1 hardware installed.

  Getting there exposed a device-selection bug in three other tests
  (`terakan_compute_loop`, `terakan_dynamic_offset_bounds`,
  `terakan_formatless_image_store`) and in `bin/terakan-test`'s own trailing
  `vulkaninfo` sanity check: all of them assumed exactly one Terakan-capable
  device would ever be enumerated, which a machine with both a working card
  and a TeraScale 1 one now violates. The three tests now loop through
  enumerated devices and pick one whose name does not start with "TeraScale
  1" rather than assuming there is only one candidate or that the first
  match is usable; `vulkaninfo`'s own device-selection flags (`--json=N`)
  turned out not to help, since it creates a device for every physical
  device it finds regardless of which one is asked to be reported on, so
  `bin/terakan-test` instead falls back to the GPU test suite's own log
  when `vulkaninfo`'s summary aborts on the unusable device before printing
  anything useful.

- TeraScale 1 (R600/R700) "begin command buffer" register atom: the whole of
  `r600_init_atom_start_cs()` (`src/gallium/drivers/r600/r600_state.c`). R600
  gfx-level command buffers now start with `PKT3_START_3D_CMDBUF, 0` before
  `CONTEXT_CONTROL`; R700 skips it because the opcode was removed there. The
  remaining initialization is split across two functions.
  `terakan_hw_config_shared_terascale_1_write_sq_config()`
  writes `SQ_CONFIG`/`SQ_GPR_RESOURCE_MGMT_1`/`SQ_GPR_RESOURCE_MGMT_2`/`SQ_THREAD_RESOURCE_MGMT`/
  `SQ_STACK_RESOURCE_MGMT_1`/`SQ_STACK_RESOURCE_MGMT_2`, the block
  `chip_info->terascale_1`'s fields feed directly.
  `terakan_hw_config_shared_terascale_1_write_context_defaults()` writes
  everything else the reference function does: `VC_ENHANCE`, the R700-vs-R600
  branch (`VGT_ENHANCE`/`SQ_DYN_GPR_CNTL_PS_FLUSH_REQ`/`DB_DEBUG`/
  `DB_WATERMARKS`/`SPI_THREAD_GROUPING`), the `SQ_*_RING_ITEMSIZE`,
  `ALU_CONST_BUFFER_SIZE_*` and `VGT_OUTPUT_PATH_CNTL` zero-initialization
  blocks, a long run of individual VGT/PA/SPI/DB/CB/SQ context and ctl_const
  registers, and the three initial `SQ_LOOP_CONST` values -- R700 writes 3
  baseline stores (`VGT_ENHANCE`, `PA_SC_EDGERULE`, `SX_MISC`) that the
  reference begin atom writes only for R700, which the `is_r700` parameter
  selects. R600 does use `SX_MISC` later for rasterizer discard, but not as a
  begin-atom constant. Both helpers are kept in
  the same translation unit, including only `r600d.h`/`r600d_common.h` and
  never `evergreend.h`. The two headers give the *same offset* a different
  meaning on this generation (`0x8C0C` is `SQ_THREAD_RESOURCE_MGMT` here,
  `SQ_GPR_RESOURCE_MGMT_3` -- an entirely different register -- on
  Evergreen-and-later), and while the handful of field macros this file
  actually uses happen to be bit-identical between the two headers where
  their names coincide, mixing both in one translation unit is not something
  to bet real hardware state on; the classic Gallium R600 driver keeps the
same split between `r600_state.c` and `evergreen_state.c`.
  `terakan_hw_config_shared_terascale_1_test` checks the exact packet bytes
  against RV710's real reference values (`r600_state.c`) for the R700 path,
  dword for dword, plus the R600 branch's distinct values and total length,
  rather than checking the driver's own logic reproduced back at itself,
  including the family baseline PS/VS/clause-temporary GPR partition in
  `SQ_GPR_RESOURCE_MGMT_1`. There is intentionally no
  `SQ_GPR_RESOURCE_MGMT_3`: address `0x8C0C` is thread management on this
  generation. Draw recording now checks the GPR count of every bound hardware
  stage against this exact per-family partition before emitting any draw packet.
  A shader exceeding its stage's baseline makes command-buffer recording fail,
  preventing the lockup `r600_adjust_gprs()` documents when
  `SQ_PGM_RESOURCES_*.NUM_GPRS` exceeds `SQ_GPR_RESOURCE_MGMT*.NUM_*_GPRS`.
  Live redistribution remains unported: unlike the classic driver, Terakan does
  not yet emit the replacement allocation with the required `WAIT_3D_IDLE`, so
  it conservatively rejects some shader combinations the hardware could run.
  The CPU oracle checks equality at the RV710 limit and all four first-invalid
  values; changing the PS comparison from `<=` to `<` makes the equality case
  abort. No draw was submitted to RV710, so the baseline packet plus admission
  check still does not prove shader execution. Streaming
  output is out of scope (see TODO.md's existing R8xx/R9xx item for it), so
  the reference function's streamout-conditional stores are not written.
  Both functions are wired through
  `terakan_hw_config_shared_indirect_buffer_begun()` for TeraScale 1; the
  queue-submit guard remains because this state has not executed on RV710.

- TeraScale 1 sampler descriptors now use a generation-specific S# encoder in
  `terakan_sampler_terascale_1.c`, transcribed from `r600_create_sampler_state()`
  and the field layout in `r600d.h`. In particular, R600/R700 store 6-bit
  fractional min/max LOD and LOD bias in sampler word 1, while Evergreen uses
  8-bit fractional min/max LOD in word 1 and puts the primary LOD bias in word
  2. Evergreen-only `TRUNCATE_COORD`, `DISABLE_CUBE_WRAP`, `PERF_MIP` and
  `ANISO_BIAS` writes are not copied to R700 fields with unrelated meanings.
  R700 `Z_FILTER` is independent from `XY_MAG_FILTER` and `XY_MIN_FILTER`.
  `r600_create_sampler_state()` leaves it at `NONE`, so Terakan does too rather
  than guessing how one Z field maps to Vulkan's distinct minification and
  magnification filters. The exact three literal words, including deliberately
  different XY minification and magnification filters, are covered by
  `terakan_sampler_terascale_1_test`. The test proves encoding only. It does
  not prove 3D filtering behavior, the choice of the single Z filter when
  Vulkan minification and magnification filters differ, border colors, or
  anisotropic filtering on RV710 because queue submission remains blocked.

- TeraScale 1 sampled-resource and buffer T# packet encoding now translates
  Terakan's Evergreen-shaped software descriptors at emission time, only in
  the `is_terascale_1` branch. R600/R700 `PKT3_SET_RESOURCE` uses seven dwords
  per slot rather than Evergreen's eight, so both the packet count and the
  resource-slot offset use a stride of 7. Image fields are rearranged according
  to `r600_create_sampler_view_custom()` and `r600d.h`: `TILE_MODE` and
  `TILE_TYPE` move to word 0, `DATA_FORMAT` to word 1, and validity plus maximum
  anisotropy to word 6. Array activation is expressed by the R700 `DIM` value
  (`*_ARRAY`), with `BASE_ARRAY` and `LAST_ARRAY` retained in word 5; there is
  no Evergreen word-7 resource layout to reuse. For multisampled resources the
  R700 mip word points at the base surface, matching direct pre-Evergreen sample
  fetch rather than treating Evergreen FMASK as a mip address. Buffer words
  0-3 retain the common SQ vertex-constant layout, words 4-5 are zero as in
  `texture_buffer_sampler_view()`, and validity moves to word 6. The exact
  seven descriptor dwords and complete nine-dword packet are checked by
  `terakan_resource_descriptor_terascale_1_test` for a tiled 2D array image,
  with a separate buffer case. The test proves construction, not texture
  sampling or array/MSAA addressing on RV710. R700's equivalent of Evergreen's
  buffer `UNCACHED` bit is not named in `r600d.h`, so the existing runtime
  uncached override is deliberately not applied on TeraScale 1 pending cache
  coherency research. The first-draw and per-slot unbind paths also no longer
  emit Evergreen `SQ_TEX_RESOURCE_CLEAR` at control-constant address `0x03FF04`:
  no such constant exists in `r600d.h`, and the classic R600/R700 state path
  never writes it. The exact TeraScale 1 clear packet is therefore zero dwords.
  This is sufficient for valid draws, which cannot access an unbound descriptor;
  null-descriptor support would need a real invalid R700 T# descriptor rather
  than reviving the Evergreen clear packet. No resource fetch has executed on
  RV710 while queue submission remains guarded.

- TeraScale 1 (R600/R700) `DB_DEPTH_CONTROL`/`CB_TARGET_MASK`: confirmed to
  need no separate emission path at all, not just no separate
  value-computation logic as first thought. `DB_DEPTH_CONTROL`'s fields
  (`STENCIL_ENABLE`, `Z_ENABLE`, `Z_WRITE_ENABLE`, `ZFUNC`,
  `BACKFACE_ENABLE`, the `STENCILFUNC`/`FAIL`/`ZPASS`/`ZFAIL` group and their
  `_BF` back-face counterparts) have identical bit positions and widths in
  both `r600d.h` and `evergreend.h`; `CB_TARGET_MASK` has no named fields in
  either header at all, just a plain 4-bits-per-render-target write mask. On
  top of that, `R_028800_DB_DEPTH_CONTROL` and `R_028238_CB_TARGET_MASK` are
  the same numeric register offset in both headers, and
  `R600_CONTEXT_REG_OFFSET`/`R600_CONFIG_REG_OFFSET` (`r600d_common.h`) are
  numerically identical to the `EVERGREEN_CONTEXT_REG_OFFSET`/
  `EVERGREEN_CONFIG_REG_OFFSET` constants Terakan's `TERAKAN_CONTEXT_REG_OFFSET`/
  `TERAKAN_CONFIG_REG_OFFSET` macros are built from. That means the existing
  R8xx/R9xx `terakan_hw_config_draw_emit_db_depth_control()`/
  `_emit_cb_target_mask()` in `terakan_hw_config_draw.c` -- unmodified --
  already emit byte-identical, correct packets for TeraScale 1. The
  dedicated `terakan_hw_config_draw_terascale_1_write_db_depth_control()`/
  `_write_cb_target_mask()` functions from the previous pass were retired as
  redundant once this was confirmed, along with their test and `meson.build`
  entries -- keeping a parallel implementation that duplicates code the
  driver already runs would be exactly the kind of unnecessary abstraction
  this project's conventions ask not to add. Not every neighboring register
  is like this: see the note on `CB_COLOR_CONTROL` in the P0-equivalent item
  above, checked and found to diverge in the course of finding these two, so
  this offset/base-constant equality is a fact to check per-register, not
  something to assume applies more broadly without checking.

- TeraScale 1 (R700) `CB_COLOR_CONTROL` and `CB_BLENDn_CONTROL` packet
  construction: the shared `0x028808` address is no longer treated as shared
  semantics. The runtime R700 emitter translates Evergreen's tracked
  `CB_DISABLE`, `CB_NORMAL` and `CB_RESOLVE` intent to R700
  `SPECIAL_DISABLE`, `SPECIAL_NORMAL` and `SPECIAL_RESOLVE_BOX`, preserving
  only the independently confirmed ROP3 and degamma fields. Per-target blend
  enables are extracted from Terakan's Evergreen-shaped software state and
  moved from its bit 30 into R700 `TARGET_BLEND_ENABLE`; bit 30 is stripped
  from each otherwise field-compatible R700 `CB_BLENDn_CONTROL` payload, and
  `PER_MRT_BLEND` is enabled as classic r600 does for every family newer than
  original R600. The exact packet bytes and all three operation encodings are
  CPU-tested, including a negative control that fails when `NORMAL` is
  deliberately encoded as `DISABLE`. This does not prove GPU rendering:
  `MULTIWRITE_ENABLE` remains disabled pending the MRT/shader-export survey,
  eliminate/decompress modes remain unsupported rather than guessed, and the
  queue-submit guard remains in place.

- TeraScale 1 (R700) `DB_ALPHA_TO_MASK`: the existing Evergreen emitter no
  longer writes `R_028B70` on R700. The generation-specific writer targets
  `R_028D44`, and its field-based encoder takes only alpha-to-coverage enable
  from the shared software state while programming offsets `2,2,2,2` exactly
  as `r600_create_blend_state_mode()` does. Although the enable and offset
  bit positions happen to match `evergreend.h`, the offset and source value
  are intentionally not reused across generations. Exact CPU packet tests
  cover both enable values and fail when one classic offset is deliberately
  changed. This is packet construction only: alpha-to-coverage rendering has
  not run on RV710 while queue submission remains blocked.

- TeraScale 1 (R700) polygon offset/depth bias: the two independently tracked
  software entries now target R700's actual `PA_SU_POLY_OFFSET` block at
  0x028DF8-0x028E0C rather than Evergreen's unrelated 0x028B78-0x028B8C
  range. `DB_FMT_CNTL` is passed only after validating the two field ranges
  that are bit-identical in `r600d.h` and `evergreend.h`; clamp, front/back
  scale and offset preserve their raw IEEE-754 dwords and the ordering used by
  `r600_emit_polygon_offset()`. The exact CPU packet is covered, but depth-bias
  rasterization has not been submitted or read back on RV710.

- TeraScale 1 (R600/R700) application VS/PS and fetch-shader binding now uses the
  non-contiguous R700 `SQ_PGM_START`/`SQ_PGM_RESOURCES`/`SQ_PGM_EXPORTS`
  addresses from `r600_update_vs_state()`, `r600_update_ps_state()` and
  `r600_emit_vertex_fetch_shader()`. The first resource word is accepted only
  for the field subset proven compatible in both headers; Evergreen
  `RESOURCES_2` must remain zero because R700 has no corresponding per-stage
  float-control word. R700 `SPI_VS_OUT_ID_0` also starts at 0x028614 rather
  than Evergreen's 0x02861C. Exact CPU packets cover VS, PS, FS and VS output
  IDs. The original `CHIP_R600` pixel-shader path additionally forces
  `SQ_PGM_RESOURCES_PS.UNCACHED_FIRST_INST`, matching the hardware workaround
  in `r600_update_ps_state()`; RV610 and all later chips leave it unchanged.
  The forced and unforced resource words have an exact negative-controlled CPU
  oracle, but no original R600 card was available and shader execution is not
  proved until queue submit and readback are enabled. ES/GS binding now
  likewise uses R700's non-consecutive
  `SQ_PGM_START_ES/GS` and `SQ_PGM_RESOURCES_ES/GS` pairs transcribed from
  `r600_update_es_state()`/`r600_update_gs_state()`, rather than Evergreen's
  three-register blocks at colliding addresses. Exact CPU packets cover both
  stages. LS/HS remain rejected because TeraScale 1 has no tessellator; the
  geometry ring setup and VGT GS mode are still unported, so this does not yet
  establish a working geometry-shader pipeline.

- TeraScale 1 no longer replays the Evergreen-only draw-constant array after
  the dedicated per-indirect-buffer begin atom. That atom already transcribes
  the complete applicable `r600_init_atom_start_cs()` baseline and has an exact
  packet oracle. Replaying the later array was actively unsafe: its
  `SQ_LDS_ALLOC`, `SQ_PGM_RESOURCES_FS`, `DB_SRESULTS_COMPARE_STATE` and
  `SQ_DYN_GPR_RESOURCE_LIMIT` offsets name unrelated registers on R600/R700.
  The per-draw constant packet is therefore exactly zero dwords on TeraScale 1;
  this has not been submitted to RV710 while the queue guard remains active.

- TeraScale 1 suppresses the three Evergreen-only tessellation-stage controls
  `VGT_SHADER_STAGES_EN`, `VGT_LS_HS_CONFIG` and `VGT_TF_PARAM`. They are
  absent from `r600d.h` and from the classic R600/R700 state path, so the only
  supported tracked value is disabled and the exact hardware packet is zero
  dwords; nonzero state is rejected rather than mapped to an unrelated R700
  address. This does not add tessellation support or prove GPU execution.

- TeraScale 1 no longer replays the six Evergreen `SQ_*TMP_RING_ITEMSIZE`
  entries on the first draw. Their addresses and stage set differ from the
  R600/R700 ring block; the dedicated begin atom already clears the real R700
  `SQ_ESGS_RING_ITEMSIZE` through `SQ_PSTMP_RING_ITEMSIZE` registers. The
  currently supported all-zero state therefore produces exactly zero extra
  dwords. Nonzero ring sizes remain rejected until the corresponding R700
  GS/ES or compute ring users and base/size registers are ported.

- TeraScale 1 suppresses `SET_BOOL_CONST` for Evergreen's LS stage index 4.
  Classic R600 exposes only PS/VS/GS/ES indices 0 through 3; the existing
  VSES index 1 therefore remains valid for ordinary vertex shaders, while LS
  has no R700 destination. Its supported zero state emits exactly zero dwords,
  and nonzero LS boolean constants remain rejected with tessellation itself.

- TeraScale 1 suppresses the Evergreen `CB_IMMED0_BASE` through
  `CB_IMMED11_BASE` packet that `set_all_modified()` otherwise emits even for
  the first ordinary graphics draw. No such register block exists in
  `r600d.h`; it belongs to the still-unported UAV/compute path. The R700 packet
  is therefore exactly zero dwords and its dirty mask is consumed without
  touching hardware. Storage buffer/image execution remains unsupported.

- TeraScale 1 command recording suppresses the whole Evergreen compute-state
  emitter. Its `SQ_DYN_GPR_RESOURCE_LIMIT_1`, compute-flavoured `VGT_GS_MODE`,
  `VGT_SHADER_STAGES_EN`, `SPI_COMPUTE_*`, `SQ_PGM_*_LS`, `SQ_LDS_ALLOC` and
  `VGT_COMPUTE_*` packets are not a register-compatible R700 compute path.
  The generation-specific state packet is exactly zero dwords and the dirty bits
  are consumed. This suppresses that emitter only: direct and indirect dispatch
  recording still contain Evergreen launch packets. It does not make an opt-in
  application dispatch safe and deliberately does not provide R700 compute support.
  The [compute/ISA audit](R700_COMPUTE_ISA_AUDIT.md) now records why waiting for a classic
  R700 compute atom is insufficient: Gallium exposes compute only above R700. It also answers
  the plan's Evergreen SET_CF_IDX lane-selection question from AMD's manual, without claiming
  a proven divergent-clause fix or touching the shared SFN files under parallel investigation.
  AMD documents R7xx compute as an ES variant, but qualifies DISPATCH_DIRECT/INDIRECT
  as Evergreen/Cayman. Upstream DRM's R600 parser also lacks dispatch handling;
  the exact remote kernel source and a bounded MEM_EXPORT path still need auditing.
  Read-only inspection of the installed remote module subsequently found an export-base
  relocation handler at 0x9010 and a bitmap-accepted size register at 0x9014. The claim
  that upstream lacked this handler was wrong: direct source download finds it too;
  the web search had missed it. Its opcode tree still rejects DISPATCH_DIRECT/INDIRECT.
  See the audit's module identity and offsets; no shader export was executed and no
  relationship between the aperture and BO bounds has yet been established. Pinned
  zen v7.2.3-zen1 source relocates the base but does not validate the separately
  accepted aperture against that BO; kernel acceptance is not a bounds oracle.
  SFN now encodes a scalar `store_ssbo` as R700 `MEM_EXPORT` only for the one
  static resource that could own that aperture, with a CPU oracle for the exact
  final CF pair and a wrong-ARRAY_SIZE negative control. This is not submission
  support: base/size emission, safe BO bounds, ES launch, loads, atomics and
  dynamic bindings remain deliberately unavailable.
  A second CPU oracle now enters the same lowering from vertex NIR with
  `key.vs.as_es`, the stage the R6xx/R7xx guide says is the pre-GS
  memory-output stage: static `store_ssbo` becomes `MEM_EXPORT` on RV710 and
  remains `MEM_RAT` on Evergreen. Its forced-false R700 predicate negative
  control fails. This does not enable the still-disabled geometry feature or
  establish a runnable ES/GS draw, aperture bounds, or submission support.

- TeraScale 1 (R700) `DB_SHADER_CONTROL`: the runtime emitter no longer sends
  the full Evergreen payload merely because both generations place the
  register at `0x02880C`. Direct comparison of `r600d.h` and `evergreend.h`
  confirms identical positions for Z export, stencil-reference export,
  `Z_ORDER`, kill, mask export and dual export, and the field-based R700
  encoder emits exactly those six. `DB_SOURCE_FORMAT` values 0-2 are accepted
  as Evergreen-side shader-export metadata but omitted from the R700 payload,
  matching `r600_update_db_shader_control()`, which controls the corresponding
  R700 choice solely with `DUAL_EXPORT_ENABLE`. Reserved source format 3,
  unknown bits, `EXEC_ON_HIER_FAIL`, `EXEC_ON_NOOP`,
  `ALPHA_TO_MASK_DISABLE` and `DEPTH_BEFORE_SHADER` are rejected rather than
  reinterpreted. Conservative-Z is the deliberate exception: the valid
  `ANY`/`LESS`/`GREATER` modes are removed from the R700 `DB_SHADER_CONTROL`
  payload and emitted in the paired `DB_RENDER_CONTROL` write at `0x028D0C`,
  exactly as `r600_emit_db_misc_state()` does; the transition is admitted only
  for R700, because that reference gates it on `gfx_level >= R700`. Every
  fragment-shader change repeats the baseline control/override pair before
  the shader-control write, so a previous pipeline's depth layout cannot
  persist. The CPU oracle checks the exact R700 values and rejects both an
  R600 `LESS` mode and the reserved fourth encoding; in the external negative
  control, inverting the R700 admission predicate made the `LESS` assertion
  fail. Early-fragment-tests, queries, HTILE, copy state and fragment shader
  memory side effects remain unsupported. No R700 command stream was
  submitted and no RV710 rendering is proved by this packet test.

- TeraScale 1 (R600/R700) `RADEON_INFO_TILING_CONFIG` decode:
  `terakan_physical_device_decode_tiling_config()`
  (`terakan_physical_device_tiling_config.c`) replaces the inline struct
  literal that used to sit in `terakan_physical_device_drm_radeon.c` and was
  only ever written for R8xx/R9xx. Checked directly against libdrm's
  `radeon/radeon_surface.c` (`r6_init_hw_info()` vs `eg_init_hw_info()`, fetched
  via `curl` since `gitlab.freedesktop.org`'s raw file is behind anti-bot
  protection that blocks `WebFetch`), because the ioctl's bit layout is
  generation-specific, not just its interpretation: on TeraScale 1 the pipe
  count field starts one bit later (bits [1:3] instead of [0:3]) and the pipe
  interleave ("group bytes") field lives at bits [6:7] instead of [8:11] -- a
  different position, not a narrower field at the same one. There is also no
  row size / `TILE_SPLIT` concept on this hardware at all (zero `TILE_SPLIT`
  occurrences in `r600d.h`, and `r6_surface_best()` in the reference is a
  literal no-op for r6xx/r7xx), so `row_bytes_log2` is set to 0 for TeraScale 1
  rather than a guessed nonzero value, so a caller that starts treating it as
  real before rendering is implemented fails loudly instead of silently using
  a fabricated tile split. `bank_interleave_log2` stays 0 for both
  generations, confirmed correct rather than assumed, since libdrm's
  `struct radeon_hw_info` has no such field for either. The existing
  R8xx/R9xx decode was cross-checked against the same reference in the same
  pass and confirmed already correct, so only its formulas moved, unchanged,
  into this new isolated function. Covered by
  `terakan_physical_device_tiling_config_test`, which checks every switch case
  (including the reference's undefined-input fallbacks) for both generations.
  This is a prerequisite for tiled surface addressing, not the tiling work
  itself -- the actual pitch/height alignment, macro-tile-aspect selection,
  and bank-swizzle math for TeraScale 1 in `terakan_image.c` has not been
  started; see the tiling/surface addressing row in the P0-equivalent table
  above.

- TeraScale 1 (R600/R700) render backend count:
  `max_render_backends_log2` is now populated for TeraScale 1 instead of
  staying at its deliberate zero placeholder. Unlike R8xx/R9xx, which use a
  per-family static table in `terakan_physical_device_chip_info_init`, there
  is no such table for TeraScale 1 in this driver -- and the classic Gallium
  R600 driver does not use one either: `radeon_drm_winsys.c` queries
  `RADEON_INFO_NUM_BACKENDS` from the kernel unconditionally for every R600+
  chip and treats a failed query as fatal. Terakan does the same, but only
  for TeraScale 1: the drm_radeon winsys
  (`terakan_physical_device_drm_radeon.c`) queries it only when the chip is
  TeraScale 1, and refuses the physical device
  (`VK_ERROR_INCOMPATIBLE_DRIVER`, the same error already used for the
  tiling config and GEM info queries) if the query fails or returns 0.
  R8xx/R9xx are entirely unaffected -- the ioctl is not issued for them, so
  they gain no new kernel-version dependency.
  `terakan_physical_device_chip_info_init` gained a `terascale_1_num_backends`
  parameter carrying this value through to `max_render_backends_log2`;
  passing 0 (the WDDM winsys does this today, since TeraScale 1 is not
  brought up there yet) leaves the old placeholder behavior unchanged. The
  raw count-to-log2 conversion is `terakan_physical_device_backend_count_to_log2()`
  (`terakan_physical_device_backend_count.c`), pulled into its own file and
  unit-tested the same way the tiling config decode was, since
  `chip_info_init` itself cannot be linked into a lightweight standalone test
  binary (it pulls in the same driver-wide Vulkan/NIR machinery as the rest
  of `terakan_physical_device.c` -- confirmed by trying that first and
  hitting undefined references at link time before extracting this
  function). It rounds up rather than down so a real backend count is never
  underrepresented, though every publicly documented R600/R700 backend count
  is a power of two anyway (1, 2, or 4), confirmed on real hardware: the RV710
  in the dual-GPU test machine reports exactly 1 backend via a standalone
  ioctl probe, matching its known single-render-backend spec.

- TeraScale 1 (R600/R700) begin-command-buffer atom wired in, and two latent
  bugs found and fixed while doing it:
  `terakan_hw_config_shared_indirect_buffer_begun()` now branches on
  `chip_info->is_terascale_1` and calls the already-tested
  `terakan_hw_config_shared_terascale_1_write_sq_config()`/
  `_write_context_defaults()` in place of the R8xx/R9xx SQ_CONFIG block, so
  the atom written and tested in an earlier pass is now actually reachable
  from command buffer recording. Minimal R700 device creation now reaches this
  setup, though queue submission is still guarded. Auditing the surrounding code
  for other places gated on `is_r9xx` alone -- since `is_terascale_1` chips
  have `is_r9xx == false` too, the same as R8xx -- turned up two functions
  that would have miscompiled TeraScale 1 command buffers had they run
  before this: `terakan_hw_config_shared_draw_emit_sq_thread_stack_resource_mgmt()`
  and `_compute_emit_sq_thread_stack_resource_mgmt()` both wrote
  `R_008C18_SQ_THREAD_RESOURCE_MGMT_1` per draw/dispatch, and the LDS setup
  blocks in `terakan_hw_config_shared_draw_emit_modified()` and
  `_compute_emit_modified()` wrote `R_008E2C_SQ_LDS_RESOURCE_MGMT` on the
  compute-to-draw and graphics-to-compute transitions respectively -- neither
  register is defined at all in `r600d.h`, so on real TeraScale 1 hardware
  these would have written to whatever unrelated register (if any) actually
  lives at those offsets, not a differently-laid-out version of the same
  one. All four now also check `is_terascale_1` and skip. The latter transition
  was found later because its condition checked only `!is_r9xx`; its exact
  TeraScale 1 packet is now covered as zero dwords. This is a real,
  currently-undocumented gap, not a no-op: TeraScale 1 has no tessellator
  and doesn't re-switch thread/stack allocation per draw the way R8xx does,
  so the draw-time skip is permanently correct, but TeraScale 1's
  compute/LDS thread and stack configuration has not been researched at all
  (`chip_info->terascale_1` has no LS-stage field the way it has
  `num_ps_gprs`/`num_vs_gprs`/etc. for the graphics stages), so a TeraScale 1
  compute dispatch would currently run with no compute thread/stack
  configuration whatsoever -- tracked here rather than guessed at.

- TeraScale 1 (R600/R700) CB/DB/PA/SPI/SQ register compatibility audit:
  every `R_XXXXXX_*` register `terakan_hw_config_draw.c` and
  `terakan_hw_config_shared.c` reference was checked mechanically against
  both `r600d.h` and `evergreend.h` -- offset presence, field-macro name
  sets, and field-macro bit-shift/mask formulas (comment and whitespace
  differences normalized out, so this isn't textual diffing) -- extending
  the DB_DEPTH_CONTROL/CB_TARGET_MASK/tiling-config-offset-base findings
  from earlier passes into a full survey rather than a couple of one-off
  checks. Findings, most important first:

  - **Same offset, entirely different register on R600/R700 -- never reuse
    R8xx/R9xx emission code for these without a real TeraScale 1 port**:
    most dangerously, the entire DB_* block Terakan currently emits at
    0x028000-0x02805C (`DB_RENDER_CONTROL`, `DB_COUNT_CONTROL`,
    `DB_RENDER_OVERRIDE`, `DB_RENDER_OVERRIDE2`, `DB_Z_INFO`,
    `DB_Z_READ_BASE`, `DB_STENCIL_READ_BASE`, `DB_DEPTH_SIZE`,
    `DB_DEPTH_SLICE`) sits on top of `CB_COLOR0_BASE`/`_2_BASE`/`_3_BASE`/
    `_6_BASE`/`_7_BASE` on R600/R700 -- render target base *addresses*, not
    depth/stencil control bits, so blindly reusing this code for TeraScale 1
    would write GPU addresses into whatever these DB fields decode to, or
    vice versa. Also colliding: the `SQ_PGM_START`/`SQ_PGM_RESOURCES(_2)`
    range for GS/ES/FS/HS/LS (0x028874-0x0288D8) is fully remapped --
    Evergreen's HS/LS tessellation-stage shader pointers occupy addresses
    that hold R600/R700's `SQ_ESGS_RING_ITEMSIZE`/`SQ_VSTMP_RING_ITEMSIZE`/
    `SQ_PSTMP_RING_ITEMSIZE`/`SQ_FBUF_RING_ITEMSIZE`/`SQ_PGM_CF_OFFSET_*`
    instead, and even the ES/FS shader-start pointers are shifted by one
    register slot relative to each other between the two generations.
    `CB_COLOR8_BASE`/`_9_BASE` (0x028E40/0x028E5C) collide with
    `PA_CL_UCP2_X`/`_UCP3_W` (user clip plane coefficients!) on R600/R700.
    Evergreen `PA_SC_AA_MASK` (0x028C3C) collides with `CB_CLRCMP_MSK`; the
    TeraScale 1 path now emits the classic driver's repeated low-byte mask at
    the real R600/R700 `PA_SC_AA_MASK` address 0x028C48, with an exact packet
    oracle. This proves the CPU-side register address and payload construction,
    but not multisample rendering on RV710 while queue submission remains
    blocked. RV610 and every later TeraScale 1 family use exactly the two shared
    `PA_SC_AA_SAMPLE_LOCS_MCTX` words at 0x028C1C/20, while original
    `CHIP_R600` selects the separate 2x/4x/8x configuration-register forms at
    0x008B40..0x008B4C, following the family branch in `r600_emit_msaa_state()`.
    Exact CPU packet oracles cover all four CHIP_R600 sample-count forms and
    the later context-register form, rather than reusing the four/eight
    pixel-specific R8xx words (whose later addresses collide with
    `CB_CLRCMP_*`). Consequently
    the TeraScale 1 physical device advertises the representable 1x1
    programmable sample-location grid rather than R8xx/R9xx's 2x2 grid. The
    exact CPU packet is covered, and RV710 property enumeration verifies the
    1x1 limit, but multisample rendering is still unsubmitted and unread back.
    `SPI_BARYC_CNTL`/`SPI_PS_IN_CONTROL_2` collide with the
    R600/R700-only `SPI_FOG_FUNC_SCALE`/`_BIAS`. `SQ_GPR_RESOURCE_MGMT_3`/
    `SQ_GLOBAL_GPR_RESOURCE_MGMT_1`/`_2` (0x008C0C/0x008C10/0x008C14) are
    `SQ_THREAD_RESOURCE_MGMT`/`SQ_STACK_RESOURCE_MGMT_1`/`_2` on R600/R700 --
    already correctly handled by the dedicated TeraScale 1 begin-atom
    writer, not by the shared R8xx code, so no action needed there, but
    listed here since it's the same class of finding, now independently
    reconfirmed by this mechanical pass rather than by the earlier
    by-hand read of `r600_init_atom_start_cs()`.

  - **Same offset and register, but divergent field layout -- needs real
    per-register TeraScale 1 value-computation logic, not just an offset
    fix**: `DB_EQAA` (0x028804) is `CB_BLEND_CONTROL` in field terms on
    R600/R700 (a completely different feature under a coincidentally
    DB-shaped name on Evergreen); `CB_COLOR_CONTROL` (0x028808, the
    already-known `SPECIAL_OP`-vs-`MODE` divergence at bit 4);
    `PA_SC_MODE_CNTL_1`/`_0` (0x028A4C/0x028A48) restructure most of their
    bits between generations. The TeraScale 1 path now combines both software
    entries into the single `PA_SC_MODE_CNTL` at 0x028A4C. It selects R600's
    `WALK_ALIGN8_PRIM_FITS_ST` baseline or R700's EOV_REZ/ZMM/scissor baseline
    from the runtime gfx level, and applies the tile-cover workaround only to
    RV770; it never writes Evergreen `MODE_CNTL_0` to TeraScale 1's unrelated
    `PA_SC_MPASS_PS_CNTL` at 0x028A48. Unknown future Evergreen bits are
    rejected. Exact CPU packet coverage does not prove rasterization on RV610
    or RV710 while submit remains blocked. `DB_SHADER_CONTROL`, `DB_RENDER_OVERRIDE(2)`,
    `DB_RENDER_CONTROL`, `DB_COUNT_CONTROL` (already covered above as
    address collisions, but even where the DB *name* is right on both
    sides the field layout still differs completely, so this is a
    double failure mode, not just a naming coincidence to work around).

  - **Same offset, register, and field positions, but a narrower mask on
    R600/R700 -- safe to reuse as-is within the narrower range, but a
    real range limit, not just a compatibility footnote**: the screen/
    window/generic scissor rectangle registers (`PA_SC_SCREEN_SCISSOR_TL`/
    `_BR`, `PA_SC_WINDOW_SCISSOR_TL`/`_BR`, `PA_SC_GENERIC_SCISSOR_TL`/
    `_BR`) use the same bit positions on both generations, but R600/R700's
    `TL_X`/`TL_Y`/`BR_X`/`BR_Y` fields are 15 bits (screen scissor) or 14
    bits (window/generic scissor) wide versus 16/15 on R8xx/R9xx --
    values within the narrower range need no change, but this is a
    real ceiling to respect once TeraScale 1 draw-time scissor emission
    is actually ported, not merely a historical curiosity.

  - **Confirmed safe, zero code needed, offset and every field bit-for-bit
    identical** (extending the DB_DEPTH_CONTROL/CB_TARGET_MASK finding):
    `VGT_PRIMITIVE_TYPE`, `SQ_VTX_START_INST_LOC` (both already emitted
    unconditionally for every chip family with no `is_r9xx`/`is_terascale_1`
    branch at all -- now confirmed genuinely safe as-is, not merely
    unguarded by oversight), `SQ_GPR_RESOURCE_MGMT_1`/`_2`, `CB_SHADER_MASK`,
    `SX_MISC`, `SX_ALPHA_TEST_CONTROL`, `DB_STENCILREFMASK`,
    `PA_CL_VPORT_XSCALE_0` (and by extension its `_1`/`_2`/`_3` siblings,
    same field layout), `SPI_PS_INPUT_CNTL_0..31`, `SPI_VS_OUT_CONFIG`,
    `SPI_INTERP_CONTROL_0`, `SPI_INPUT_Z`, `PA_CL_CLIP_CNTL`,
    `PA_SU_SC_MODE_CNTL`, `PA_CL_VTE_CNTL`, `PA_CL_VS_OUT_CNTL`,
    `PA_CL_NANINF_CNTL`, `SQ_PGM_START_PS`, `PA_SU_POINT_SIZE`,
    `PA_SU_POINT_MINMAX`, `PA_SU_LINE_CNTL`, `PA_SC_LINE_STIPPLE`,
    `VGT_PRIMITIVEID_EN`, `VGT_MULTI_PRIM_IB_RESET_EN`, `IA_MULTI_VGT_PARAM`,
    and the Cayman/NI (`is_r9xx`) address forms of `PA_SC_LINE_CNTL`/
    `PA_SC_AA_CONFIG`/`PA_SU_VTX_CNTL`/`PA_CL_GB_VERT_CLIP_ADJ`
    (0x028C00-0x028C0C) -- these happen to coincide with the R600/R700
    native addresses for the same registers with the same fields, an
    accident of the address space rather than a designed compatibility,
    worth double-checking again if this list is ever acted on rather than
    assumed to stay true.

  - **Evergreen-only concepts, absent on TeraScale 1 entirely -- not a
    porting gap, a real hardware difference**: tessellation-related state
    (`VGT_SHADER_STAGES_EN`, `VGT_LS_HS_CONFIG`, `VGT_TF_PARAM`,
    `VGT_HOS_*`), streamout config (`VGT_STRMOUT_CONFIG`/
    `_BUFFER_CONFIG`), `DB_ALPHA_TO_MASK`, `DB_PRELOAD_CONTROL`,
    `DB_SRESULTS_COMPARE_STATE0`/`1`, `SQ_LDS_ALLOC`/`_PS`, `SPI_LDS_MGMT`,
    `SPI_CONFIG_CNTL`/`_1`, `PA_SU_HARDWARE_SCREEN_OFFSET`,
    `SQ_DYN_GPR_RESOURCE_LIMIT_1`, `PA_CL_ENHANCE`, and the R8xx/R9xx-only
    `SQ_STATIC_THREAD_MGMT1`/`2`/`3` (already unreachable for TeraScale 1
    today, since it's part of the R8xx-only branch in
    `terakan_hw_config_shared_indirect_buffer_begun()` -- see the
    begin-atom wiring above -- so no action needed, just confirming that
    branch is correctly scoped). `PA_SC_EDGERULE` is a partial case: the
    register nominally exists on both, but `r600d.h` defines no field
    macros for it at all, matching what the earlier begin-atom port
    already found and handled (it's one of exactly the three R700-only
    registers `terakan_hw_config_shared_terascale_1_write_context_defaults()`
    gates on `is_r700`).

  No code changes accompany this pass -- it is a survey to work from, not
  a set of fixes. Only DB_DEPTH_CONTROL, CB_TARGET_MASK, VGT_PRIMITIVE_TYPE,
  and SQ_VTX_START_INST_LOC are confirmed to need nothing further; every
  other register above needs a real, individually-checked-against-
  `r600_state.c` port (for the divergent-field and Evergreen-only cases)
  or at minimum an explicit `is_terascale_1` guard to prevent the same
  class of bug already found and fixed for `SQ_THREAD_RESOURCE_MGMT_1`/
  `SQ_LDS_RESOURCE_MGMT` (for the same-offset-different-register cases)
  before any of this code becomes reachable.

- TeraScale 1 (R600/R700) DB render-control/override and depth-view, and
  guards for the whole dangerous DB block found by the audit above:
  `terakan_hw_config_draw_terascale_1_write_db_render_control_override()`
  writes `R_028D0C_DB_RENDER_CONTROL`/`R_028D10_DB_RENDER_OVERRIDE`
  together (confirmed against `r600_state.c`'s
  `r600_emit_db_misc_state()`: R600/R700 has no `DB_RENDER_OVERRIDE2`
  register at all, so unlike R8xx/R9xx this is two registers in one
  packet, not three across two), and
  `terakan_hw_config_draw_terascale_1_write_db_depth_view()` writes
  `R_028004_DB_DEPTH_VIEW`, confirmed field-for-field identical
  (`SLICE_START`/`SLICE_MAX`, both 11 bits) to R8xx/R9xx's own
  `DB_DEPTH_VIEW` at a different offset (`R_028008`) -- so, like
  `DB_DEPTH_CONTROL`/`CB_TARGET_MASK` before it, needs no new
  value-computation logic, only the offset-isolating wrapper. Every
  current caller of the R8xx/R9xx equivalents only ever passes zero, but
  those software values are not R700's hardware baseline. The R700 encoder
  now transcribes the no-query, no-HTILE branch of
  `r600_emit_db_misc_state()`: `ZPASS_INCREMENT_DISABLE` is set, HiZ is
  forced disabled, and both HiS directions are forced disabled. Nonzero
  Evergreen payloads are rejected rather than interpreted as R700 fields;
  query, HTILE, conservative-Z and depth/stencil-through-CB transitions
  remain unported.
  `terakan_hw_config_draw_emit_db_render_control()` now emits both
  registers together for TeraScale 1 (one PKT3, so the two
  independently-dirty-tracked R8xx/R9xx entries don't each try to emit
  half a pair); `_emit_db_render_override()` becomes a no-op for
  TeraScale 1 since the other entry already covered it.

  Also guarded, not ported -- these are the exact registers the audit
  flagged as colliding with unrelated R600/R700 registers at the same
  offset, and were confirmed to have no `is_r9xx`/`is_terascale_1` guard
  at all before this pass, the same latent-bug class already found once
  for `SQ_THREAD_RESOURCE_MGMT_1`/`SQ_LDS_RESOURCE_MGMT`:
  `terakan_hw_config_draw_emit_db_render_override2()` (no R600/R700
  equivalent exists at all -- confirmed against `r600_state.c`, not
  assumed from the header alone -- and its offset is `DB_DEPTH_INFO`
  there), `_emit_db_count_control()` (its offset is `DB_DEPTH_VIEW` on
  R600/R700, and no `DB_COUNT_CONTROL`-equivalent occlusion-query-hazard
  register was found in `r600_state.c` to replace it with -- not yet
  researched, not guessed), and `_emit_db_depth_stencil_buffer()` --
  the single most dangerous one, since its whole
  `R_028040_DB_Z_INFO..R_02805C_DB_DEPTH_SLICE` range is
  `R_028040_CB_COLOR0_BASE..R_02805C_CB_COLOR7_BASE` (render target
  *addresses*) on R600/R700. This last one is not merely unguarded but
  genuinely not portable yet regardless: R600/R700 binds depth and
  stencil through one combined surface and base address
  (`R_02800C_DB_DEPTH_BASE`, no separate stencil base at all, unlike
  R8xx/R9xx's four independent read/write Z/stencil base registers), and
  `DB_DEPTH_INFO`'s `ARRAY_MODE` field needs real tiling information this
  driver doesn't have for TeraScale 1 yet (`terakan_image.c`'s
  surface/macro-tile address math -- see the tiling/surface addressing row
  in the P0-equivalent table above and the bullet below for what exists
  of it so far).

- TeraScale 1 (R600/R700) surface pitch/height/base-alignment math:
  `terakan_image_tiling_terascale_1_alignments_linear_aligned()`/
  `_1d_tiled_thin1()`/`_2d_tiled_thin1()` (`terakan_image_tiling_terascale_1.c`),
  ported from libdrm's `radeon/radeon_surface.c` (`r6_surface_init_linear_aligned()`/
  `_1d()`/`_2d()`, the same reference already used for the
  `RADEON_INFO_TILING_CONFIG` decode and the begin-command-buffer atom) and
  unit-tested against the real RV710 tiling parameters
  (`terakan_physical_device_tiling_config_test` already established:
  `group_bytes=512`, `num_pipes=2`, `num_banks=8`). This is deliberately a
  much smaller port than R8xx/R9xx's AddrLib-derived tiling code in
  `terakan_image.c`: TeraScale 1 has no `TILE_SPLIT`, no per-surface
  `bank_width`/`bank_height`/`macro_tile_aspect` selection, and no
  macro-tile-aspect-ratio search at all -- confirmed by the tiling-config
  decode work (no `TILE_SPLIT` field exists in `r600d.h` for any CB/DB
  register) and by `r6_surface_best()` in the reference being a literal
  no-op ("no value to optimize for r6xx/r7xx") -- so the whole algorithm
  is a handful of fixed formulas over group bytes/pipes/banks/bytes-per-
  element/sample count, not an optimization search.

  A follow-up pass added per-level layout on top of that alignment math:
  `terakan_image_tiling_terascale_1_mip_extent()` (`mip_minify()` in the
  reference -- plain `u_minify()` for the base level, rounded up to the
  next power of two for every level above it, since higher mips need
  power-of-two dimensions for the tiled addressing scheme; confirmed this
  isn't R8xx/R9xx-specific by finding the same "pow2Pad" rounding already
  in `terakan_image_surface_aspect_compute()`, not assumed to carry over)
  and `terakan_image_tiling_terascale_1_level_layout()` (`surf_minify()`
  in the reference -- aligns already-minified pixel dimensions up to a
  tiling mode's pitch/height alignment granularity and computes
  pitch/slice byte sizes, including the degrade-to-1D-on-small-mip check
  `r6_surface_init_2d()` uses before calling back into
  `r6_surface_init_1d()` for a level that's too small to stay 2D-tiled).
  Both are pure functions taking already-computed inputs (minified pixel
  dimensions, alignment granularity from the `_alignments_*()` functions),
  so walking a full mip chain to cumulative byte offsets -- what
  `surf_minify()`'s own `offset`/`surf->bo_size` accumulation does across
  levels in the reference -- is still the caller's job and not yet
  written.

  A second follow-up pass added the base-level array-mode decision:
  `terakan_image_tiling_terascale_1_select_array_mode()` transcribes
  `terakan_image_surface_tiling_compute()`'s policy (prefer 2D-tiled
  unless linear tiling was requested, a debug override is set, the
  format requires linear, or it's a non-multisampled non-DB 1D image
  whose format doesn't require tiling) rather than re-deriving it, since
  that policy is a driver-level Vulkan design choice, not something
  hardware-specific to re-research -- only the array-mode constants
  (`V_0280A0_ARRAY_*` vs `V_028C70_ARRAY_*`, confirmed numerically
  identical) and the tiled-only format table (`terascale_format.h`
  already has `TERASCALE_FORMATS_TILED_ONLY_R6XX` sitting next to the
  R8xx one, unused until now) differ. Takes plain booleans rather than
  `VkImageType`/`VkImageTiling`/`terascale_format_index` so the file
  stays decoupled from Vulkan and format-table headers the way the rest
  of it already is. Also confirmed against `r6_surface_init()` in the
  reference that R600/R700 zbuffers (depth/stencil images) can never be
  plain linear -- only 1D-tiled or 2D-tiled -- and that MSAA surfaces
  must always be 2D-tiled; both are already true of the existing
  R8xx/R9xx policy this was transcribed from, so no additional branching
  was needed, just confirmed rather than assumed to still hold here.

  A third follow-up pass added the mip-chain-to-offsets walk itself:
  `terakan_image_tiling_terascale_1_mip_chain_layout()` calls
  `mip_extent()`/`level_layout()` for every level and accumulates
  offsets, mirroring the reference's own `offset`/`surf->bo_size` running
  state across `r6_surface_init_1d()`/`_2d()`'s per-level loop --
  including `r6_surface_init_2d()` calling back into
  `r6_surface_init_1d()` for the level that degrades and every level
  after it, ported here as a `degraded_to_1d` flag that latches once set
  and is never cleared, matching the reference exactly (a 2D-tiled chain
  never reverts to 2D for a smaller, later mip once it degrades).
  Array-layer/3D-depth-plane multiplication is folded in via a
  `depth_minifies_per_level` flag distinguishing a 3D image's `npix_z`
  (mip-minified like width/height) from a 2D image's array layer count
  (constant across the chain) -- the same distinction
  `terakan_image_surface_aspect_compute()` already makes for R8xx/R9xx,
  so this is Vulkan-level surface shape rather than something to
  re-derive from the reference, unlike the tiling math itself.
  Unit-tested with six hand-derived cases (a chain that never degrades,
  one that degrades at the base level and stays degraded, a
  fixed-1D-from-the-start chain with no degrade check at all, array
  layers, 3D depth planes, and a BC1 chain that converts texels to 4x4
  blocks before alignment), reusing the same RV710 alignment
  parameters as every prior tiling test in this pass.

  A fourth follow-up pass wired all of the above into
  `terakan_image.c`'s real `terakan_image_surface_compute()`, behind an
  `is_terascale_1` branch that returns early before any R8xx/R9xx-shaped
  code runs (so this cannot regress R8xx/R9xx -- verified by full local
  and real-hardware test suite runs after the change, both green).
  `terakan_image_surface_compute_terascale_1()`
  (`terakan_image_surface_terascale_1.c`) assembles the pieces above into
  a complete `terakan_image_surface` the same shape
  `terakan_image_surface_compute()` itself produces, including the
  combined-depth-stencil array-mode sharing R8xx/R9xx already does.
  The integration now has real RV710 create/layout/allocate/bind coverage:
  linear RGBA8 reports 9728-byte size and 512-byte alignment, a 2D-tiled
  RGBA8 mip chain reports 139264/8192, and a 2D-tiled BC1 mip chain reports
  348160/8192. All three bind real Radeon BOs without a kernel fault. This
  is still not GPU readback validation: queue submission remains disabled,
  so treat tiled addressing as provisional until a copy/readback test passes.
  Two real mistakes were caught by review during this
  pass specifically because of that lack of a safety net, worth recording
  so a future re-check knows to look here first: an earlier draft
  pre-multiplied the base width by `surfels_per_block` before calling
  `mip_chain_layout()`, which rounds the wrong quantity (surfel count
  instead of texel count) to a power of two for mip levels above 0 --
  initially contained by excluding 3x-expand formats (8_8_8, 16_16_16,
  32_32_32) rather than getting the multiplication order right blind. A
  later focused pass now accepts them by passing `surfels_per_block=3` into
  the per-level loop: it minifies in texels, expands width, applies the
  higher-mip power-of-two padding, and gives the base pitch the additional
  three-times-normal alignment required by the descriptor's texel pitch.
  An exact RV710 CPU oracle distinguishes this from both the old
  bytes-per-block path and omission of the base-pitch rule; real RV710 also
  creates, lays out, allocates and binds a two-level R32G32B32 image. This is
  not a sampled or render-target readback, since submit remains blocked.
  The same oracle now also checks the exact absolute surfel and byte addresses
  produced by the generation-neutral clear/copy NIR convention
  `level_offset + y * aligned_pitch + 3 * x + component`, for an interior
  texel and the last real texel of both levels, including the remaining row
  padding. This proves that the R700 layout and the meta-shader address units
  agree on paper; it still does not prove that R700 UAV execution observes
  those addresses until queue submission is enabled and read back.
  The second review mistake was a copy-paste error passing
  `bytes_per_element * surfels_per_block`
  (i.e. `bytes_per_block` again) to three tiling calls instead of the
  intended per-surfel byte size, caught the same way. Block-compressed and
  8x1/2x1 or 2x1 subsampled formats now use the reference ordering:
  minify texel extents first, then divide by block dimensions, then align.
  Each aspect base is also aligned independently; the previous draft added
  the lower-bound offset directly and never populated the aspect base field,
  which could overlap the second aspect with the first.

  Still not done, and each a substantial piece of its own: the
  `DB_DEPTH_INFO`/`CB_COLOR_INFO` register field computation this
  unblocks, which still needs its own per-field compatibility check
  against `r600d.h` (not yet done -- these registers weren't in the
  CB/DB/PA/SPI/SQ audit above, since that audit only covered registers
  the R8xx/R9xx code currently references, and this tiling work is what
  would make TeraScale 1's own field set relevant for the first time;
  spot-checked while writing an earlier pass, though, and R600/R700's
  `CB_COLOR0_INFO`/`DB_DEPTH_INFO` field sets are consistent with the
  simpler algorithm above -- no `BANK_WIDTH`/`BANK_HEIGHT`/`NUM_BANKS`/
  `MACRO_TILE_ASPECT` fields on either register, matching the tiling math
  needing none of those inputs); and GPU copy/readback validation.

  A fifth follow-up pass ported the emission (not value-computation) side
  of the actual depth surface binding registers:
  `terakan_hw_config_draw_terascale_1_write_db_depth_size()`
  (`R_028000_DB_DEPTH_SIZE`, `PITCH_TILE_MAX`/`SLICE_TILE_MAX` -- no
  R8xx/R9xx equivalent at this offset or shape, since R600/R700 packs the
  whole slice tile count into this one register instead of splitting
  pitch/height across `DB_DEPTH_SIZE` and a separate `DB_DEPTH_SLICE`
  register the way R8xx/R9xx does; R600/R700 has no `DB_DEPTH_SLICE` at
  all) and
  `terakan_hw_config_draw_terascale_1_write_db_depth_base_info()`
  (`R_02800C_DB_DEPTH_BASE`/`R_028010_DB_DEPTH_INFO` together, matching
  `r600_state.c`'s own `radeon_set_context_reg_seq(cs,
  R_02800C_DB_DEPTH_BASE, 2)` sequencing). Both take fully
  caller-computed values, same as every other TeraScale 1 register
  emission function. The depth-only value computation described below is
  now written, but the combined depth/stencil case remains a genuinely open
  research question: R600/R700 binds depth and stencil through one packed
  surface and base address (no separate stencil base at all), which doesn't
  map directly onto Terakan's existing split-aspect
  `terakan_depth_stencil_descriptor`.

  A sixth follow-up ported the first complete color-target descriptor path rather than merely its
  register shape. `terakan_hw_config_draw_terascale_1_cb_color_encode()` repacks the existing
  field-level intermediate descriptor into R600/R700 `CB_COLORn_INFO`, `SIZE` and `VIEW`, and
  `terakan_hw_config_draw_terascale_1_write_cb_color()` emits the seven separate R600/R700 register
  arrays (`INFO`, `BASE`, `FRAG`, `TILE`, `SIZE`, `VIEW`, `MASK`) used by
  `r600_emit_framebuffer_state()`. This is intentionally limited to single-sampled RTVs without
  FMASK/CMASK: in that exact case classic `r600_init_color_surface()` points `FRAG` and `TILE` at
  the color base and leaves `MASK` zero. Terakan does the same and rejects, rather than guesses,
  MSAA/metadata and UAV descriptors. `terakan_hw_config_draw_emit_cb_color()` now selects this path
  at runtime for TeraScale 1, never the Evergreen `CB_COLOR0_BASE..DIM` block; targets 8-11 and the
  compute path stay suppressed because R600/R700 has only eight color targets and its UAV/compute
  state is not ported. The existing `terakan_hw_config_draw_terascale_1` test (already present in
  both Meson and `bin/terakan-test`) checks the exact R700 field values, all seven packet offsets,
  disabled metadata bases, and rejection boundaries. Its negative control changed the
  implementation's `COMP_SWAP` value while leaving the oracle intact and made the test abort at
  the `INFO` comparison. This proves CPU-side packet construction only: no R700 command stream was
  submitted and no tiled GPU copy/readback has passed yet.

  A seventh follow-up connected the single-sampled depth-only DB path.
  `terakan_hw_config_draw_terascale_1_db_depth_encode()` transcribes the
  non-HTILE part of `r600_init_depth_surface()`: R8xx software depth formats
  are explicitly mapped to R700 `DEPTH_16`, depth-only `DEPTH_X8_24`, or
  `DEPTH_32_FLOAT`; `ARRAY_MODE`, `ZRANGE_PRECISION`, tile pitch/slice and
  `DB_PREFETCH_LIMIT = aligned_height / 8 - 1` are repacked into the R700
  registers. The command writer emits `DB_DEPTH_SIZE`, `DB_DEPTH_VIEW`, the
  `DB_DEPTH_BASE/INFO` pair and `DB_PREFETCH_LIMIT`, with one relocation at
  the real R700 base payload. Stencil-bearing and multisampled descriptors
  are explicitly unbound instead of leaving a previous DB surface active:
  classic R700 stores their depth and stencil in one packed allocation,
  while Terakan still lays out the two Vulkan aspects separately. The CPU
  oracle checks exact values, packet offsets and rejection boundaries. No
  DB packet has been submitted or read back on RV710; the submit guard stays
  in place, and combined depth/stencil remains unsupported rather than
  guessed.

- TeraScale 1 (R600/R700) `CB_COLORn_INFO`/`DB_Z_INFO` field-position
  comparison, following up on the spot-check above with the real
  per-field breakdown: none of R8xx/R9xx's `CB_COLOR0_BASE..CB_COLOR0_DIM`
  register offsets (0x028C60-0x028C78) exist at all in `r600d.h` --
  confirmed with the same mechanical offset/field comparison the
  CB/DB/PA/SPI/SQ audit used, not just read by eye -- consistent with
  R600/R700's entirely different, already-known CB_COLOR0 block location
  (`R_028040_CB_COLOR0_BASE`, `R_028060_CB_COLOR0_SIZE`,
  `R_0280A0_CB_COLOR0_INFO`). Comparing `R_0280A0_CB_COLOR0_INFO`'s
  fields against `R_028C70_CB_COLOR0_INFO`'s directly (different offsets,
  so the mechanical same-offset audit can't do this part, only checked by
  reading both) found a real partial overlap, not simply "compatible" or
  "incompatible": `ENDIAN`, `FORMAT`, `ARRAY_MODE`, and `NUMBER_TYPE` sit
  at the identical bit positions on both (bits 0-1, 2-7, 8-11, 12-14
  respectively), but every field after that diverges -- R600/R700 has an
  extra `READ_SIZE` bit at 15 that R8xx/R9xx's `COMP_SWAP` starts at
  instead, and the two field sets past that point are different sizes
  entirely (R600/R700 has `TILE_MODE`/`CLEAR_COLOR`/`BLEND_FLOAT32`, none
  of which exist on R8xx/R9xx; R8xx/R9xx has `FAST_CLEAR`/`COMPRESSION`/
  `RAT`/`RESOURCE_TYPE`, none of which exist on R600/R700), so nothing
  past `NUMBER_TYPE` can be assumed compatible even though the register
  is conceptually the same feature on both. `DB_Z_INFO` was already fully
  covered by the standout finding in the CB/DB/PA/SPI/SQ audit above (its
  R8xx/R9xx offset is R600/R700's `CB_COLOR0_BASE`), so this is recorded
  here as the `CB_COLOR0_INFO` counterpart to that finding, not a new
  discovery about `DB_Z_INFO` itself.

- Reducing stencil resolve, `VK_RESOLVE_MODE_MIN_BIT` and
  `VK_RESOLVE_MODE_MAX_BIT`: the same shaders and dispatch as the depth reducing
  modes, differing only in the reducing opcode (an unsigned integer comparison)
  and the export slot. `terakan_stencil_resolve_modes` covers it at 2x, 4x and
  8x the same way the depth test does: a different stencil value per sample,
  written through `STENCIL_REPLACE` with a static per-pipeline reference and a
  sample mask confining each draw to one sample.

  Getting there surfaced a real driver bug, worth recording so it is not
  rediscovered (**now root-caused and fixed -- see the RESOLVED note at the end
  of this entry**): **a stencil-only render target on a combined depth/stencil
  image writes and reads back the wrong values.** Writing a uniform stencil
  value with no depth attachment bound (`pDepthAttachment == NULL`,
  `pStencilAttachment` naming a `D32_SFLOAT_S8_UINT` image) reads back a
  per-column pattern with no relationship to what was written; a roundtrip of
  the same pattern through `vkCmdCopyBufferToImage`/`vkCmdCopyImageToBuffer`
  with no rendering involved is exact, which places the defect in the DB write
  path specifically, not the tiled surface or the copy engine. Binding a depth
  attachment of the same image alongside — even one neither read nor written —
  makes it disappear, which is what `terakan_stencil_resolve_modes` does and
  what `VK_KHR_depth_stencil_resolve` shapes a real resolve into anyway, since
  VUID-06085 requires the same view when both attachments are non-null. The
  driver's own masking of `DB_Z_INFO` when no depth attachment is bound already
  preserves the tiling fields the code identifies as shared with stencil
  (`ARRAY_MODE`, the bank fields), so the cause is not that masking; it is left
  open above as its own item rather than guessed at further.

  A follow-up pass found and fixed a real, related inconsistency in the same
  function, `terakan_image_create_depth_stencil_descriptor()`
  (`terakan_image.c`): `DB_Z_INFO`'s `TILE_SPLIT` field was set from
  `surface_depth->tiling.attrib_tile_split` only inside the
  `view_depth_format != TERASCALE_R8XX_DEPTH_FORMAT_INVALID` branch, unlike
  `ARRAY_MODE`/`BANK_WIDTH`/`BANK_HEIGHT`/`MACRO_TILE_ASPECT` just above it,
  which already correctly come from `surface_main_aspect` (falls back to the
  stencil surface when depth is invalid) -- so a stencil-only descriptor
  always got `TILE_SPLIT = 0` regardless of the stencil surface's real tile
  split, the same class of bug as the `ARRAY_MODE`/bank-field case already
  ruled out above, just missed by that earlier pass. Fixed to use
  `surface_main_aspect` like its neighboring fields do.

  **This fix was empirically confirmed to NOT reproduce or resolve the
  documented symptom above**, so it does not close this item, and is
  recorded here specifically so the next attempt doesn't re-derive and
  re-test the same hypothesis: a stencil-only clear-and-readback test
  (`vkCmdBeginRenderPass`/`vkCmdEndRenderPass` with a `LOAD_OP_CLEAR`
  stencil-only view of a `D32_SFLOAT_S8_UINT` image, common-runtime-translated
  into the same `terakan_CmdBeginRendering()` path a native dynamic-rendering
  call would take -- confirmed by reading `terakan_vk_render_pass.c`, not
  assumed) passed identically with and without the `TILE_SPLIT` fix at every
  size tried (8x8, 256x256, 2048x2048 on real CAICOS hardware), meaning
  `attrib_tile_split` stayed `0` for this aspect/format/size combination
  regardless -- either 2D macro-tiling is never selected for a stencil-only
  aspect of a combined depth/stencil image at these sizes on this hardware
  (in which case the field genuinely doesn't matter here, though the fix is
  still correct to keep for whatever case does exercise it), or the true
  root cause is unrelated to `TILE_SPLIT` entirely. The fix is kept because
  it is independently correct regardless of this investigation's outcome
  (the same reasoning that already justified the `ARRAY_MODE`/bank-field
  fallback), but the documented per-column corruption itself remains
  unreproduced by a clear -- the next attempt should try an actual draw
  through the rasterizer's stencil test/`STENCIL_REPLACE` path (matching
  what `terakan_stencil_resolve_modes` exercises when it works around the
  bug by binding a depth attachment alongside), since a bare `LOAD_OP_CLEAR`
  may simply not exercise whatever code path the original symptom came from.

  **A follow-up pass retracts the "deterministic" claim above -- it did
  not hold up under proper repeated testing, and the corrected story is
  important enough to leave in place rather than silently rewrite.** The
  finding above (`terakan_stencil_resolve_modes_test.cpp` with
  `pDepthAttachment` forced to `nullptr` at 2x/4x/8x samples, reading
  back a constant `0x5A` on every texel) was real -- it happened, was
  observed directly, and every value quoted above was actually printed by
  that run -- but was only ever run *once* per sample count before being
  written up as "deterministic and identical every run." A later pass
  that actually repeated it found the opposite: 75 additional runs of the
  identical scenario (50 back-to-back, then 15 more each deliberately
  preceded by a forced driver crash to test whether the first failure was
  caused by corrupted state left over from an earlier, unrelated crash in
  this investigation's own test harness -- an initial repro attempt had
  crashed with `SIGSEGV` from calling `vkCmdBeginRenderingKHR` without
  enabling `VK_KHR_dynamic_rendering`, confirmed by exit code 139) **all
  passed, 75/75, with zero reproductions.** Register-level instrumentation
  added during the same pass (temporary, not committed) showed
  `DB_Z_INFO`/`DB_STENCIL_INFO`/base-address values that looked correct
  and consistent between passing and failing states alike, and
  `DB_Z_INFO.NUM_SAMPLES`'s masking (the lead the previous version of
  this note pointed at) is identical in both the working and broken
  cases, so it is very unlikely to be the cause -- ruled out, not just
  deprioritized. Whatever produced the one observed failure remains
  unexplained: possibly a genuine, rare race condition this pass simply
  didn't get unlucky enough to hit again, possibly some transient
  condition specific to that one session unrelated to Terakan's own
  logic. The honest state of this bug going into the next pass is:
  **not reproduced under any controlled condition tried across two full
  investigation passes** (clear, single-sample draw, multisample draw,
  with and without a preceding crash), despite one genuine but
  unrepeated observation. Anyone picking this up next should not treat
  the specific repro recipe above as reliable, but also should not
  assume the underlying bug doesn't exist -- both a "real, rare bug" and
  "environmental fluke unrelated to Terakan" remain live possibilities,
  and this note exists so the next attempt spends its effort finding
  out which, rather than re-discovering that the "deterministic" repro
  doesn't actually reproduce on the first try and wondering whether it
  did something wrong.

  **RESOLVED.** A third pass reproduced this deterministically and found the root
  cause. Two things made the difference, both of them things the notes above had
  recorded as untried:

  1. **Draw, don't clear.** A bare `LOAD_OP_CLEAR` had already been tried and
     passed, which is why two passes found nothing; the clear does not go through
     the same DB base-address programming a draw does. Drawing through the
     rasterizer's `STENCIL_REPLACE` path -- exactly what the note above said the
     next attempt should try -- fails immediately and every time.
  2. **Many renders in one command buffer.** Every earlier pass ran the scenario
     once per process. Running sixteen renders back to back with a different
     stencil reference each makes a wrong result impossible to mistake for an
     uninitialised read.

  With that, the failure is 100% reproducible at single sample, and a four-way
  control matrix isolates it exactly: stencil-only fails with both a dynamic and a
  static stencil reference, and binding a depth attachment alongside passes in
  both cases. So it is purely "no depth attachment bound", and nothing to do with
  how the reference reaches the hardware.

  The cause is in `terakan_hw_config_draw_set_db_depth_stencil_buffer()`
  (`terakan_hw_config_draw.c`), which stored the two aspects' base addresses
  **swapped** when depth was not bound:

  ```c
  descriptor.z_base       = new_depth_bound ? descriptor->z_base : descriptor->stencil_base;
  descriptor.stencil_base = new_depth_bound ? descriptor->stencil_base : descriptor->z_base;
  ```

  `terakan_hw_config_draw_emit_db_depth_stencil_buffer()`'s single-aspect path then
  picks `stencil_bound ? descriptor->stencil_base : descriptor->z_base` and writes
  it to `DB_STENCIL_READ_BASE`/`DB_STENCIL_WRITE_BASE` -- so for a stencil-only
  render it received the **depth plane's** address, and every stencil write landed
  there instead of in the stencil plane. Confirmed with temporary register-level
  instrumentation before changing anything: the incoming descriptor carried
  `z_base=0x00000000` and `stencil_base=0x00000004` (the stencil plane sits 0x400
  bytes in) and the emit path chose `0x00000000`; after the fix it chooses
  `0x00000004` and the readback is exact.

  The swap accomplished nothing even in principle: that path does not write the
  unbound aspect's base registers at all, so there was no "point the other one
  somewhere safe" effect to preserve. It also silently broke the setter's own dedup
  comparison, which compares each stored base against the incoming descriptor's
  same-named field and therefore never matched for a stencil-only bind. Fixed by
  storing each base as itself.

  This also explains why the earlier `TILE_SPLIT` and `DB_Z_INFO.NUM_SAMPLES`
  hypotheses were both correctly ruled out: neither had anything to do with it, and
  the one genuine but unreproduced multisample observation recorded above is
  consistent with the same root cause (a stencil-only bind at any sample count),
  which is why that observation was real even though the specific repro recipe
  around it was not reliable.

  `terakan_stencil_only_render` covers it permanently, including the depth-bound
  shape existing stencil users rely on, so a future change that fixes one by
  breaking the other is caught. It is a verified negative control: reverting the
  one-line fix makes its stencil-only pass fail 15/16 iterations while its
  depth-bound pass still passes.

- Reducing depth resolve, `VK_RESOLVE_MODE_MIN_BIT` and `VK_RESOLVE_MODE_MAX_BIT`:
  every sample is fetched and combined in the pixel shader, one program per
  sample count, and `independentResolve` is now true because each aspect
  resolves from its own draw and its own shader. `terakan_resolve_modes` gives
  each sample of the attachment a different depth, by confining every draw to
  one sample with the pipeline's sample mask, then resolves all three modes out
  of that one attachment; the depths are picked so sample zero, the minimum and
  the maximum are three different values at 4x and 8x.

  Three things were learned the hard way and are worth not rediscovering. An
  ALU clause's count is in eight-byte slots, so each pair of literal dwords
  counts as a slot of its own alongside the instruction reading it -- counting
  only instructions truncated the clause and silently dropped the last samples.
  Push constants do not reach this shader: whatever it reads back is neither the
  uploaded values nor zeroes, which is why the sample indices are literals and
  the sample count selects the program instead of being clamped on the CPU.
  Grouping ALU copies of different channels aliases one source channel onto
  another, the same effect the image blit shader documents, so each copy is its
  own group. Fetching a sample that does not exist returns zero, which a maximum
  would survive but a minimum would not.

- Depth/stencil rendering into a mip level other than zero: the depth/stencil
  descriptor took its base address from the aspect while taking its pitch and
  slice from the requested level, so anything rendering, clearing or resolving
  into a level above zero wrote level zero's memory at another level's
  dimensions. `vkCmdClearDepthStencilImage` goes through the same descriptor, so
  a cleared level above zero was never actually cleared and read back as
  uninitialized memory. The base now comes from the level, whose offset already
  includes the aspect's. `terakan_depth_resolve_subresource` resolves into level
  one, layer one of a two-level, two-layer destination through a partial render
  area, which also covers the array layer and the render area offset reaching
  the resolve; against the previous code every texel it reads is uninitialized.

- Dynamic rendering: `VK_KHR_dynamic_rendering` is advertised. It is the
  driver's native rendering path rather than a new one — the common render pass
  implementation lowers `VkRenderPass` onto `terakan_CmdBeginRendering`, suspend
  and resume flags included — so exposing it was a matter of clearing its
  `VK_KHR_depth_stencil_resolve` dependency. `terakan_dynamic_rendering` covers
  the surface that render passes never reach: attachments described per
  recording, a pipeline built from `VkPipelineRenderingCreateInfo` with no
  render pass at all, and one render split across a suspending and a resuming
  half.

- Multi-stage push constant ranges: a `VkPushConstantRange` covering more than
  one stage used to lose every other stage, because the loop distributing the
  range's extent cleared the scanned bit twice — `u_bit_scan` already clears it,
  and an extra `&= x - 1` cleared the next one too. The canonical
  `VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT` range therefore
  reached the vertex shader and left the fragment shader reading zeros. Found
  while bringing up `terakan_dynamic_rendering`, which covers it: its fragment
  shader takes its colour from a push constant shared with the vertex stage.

- 3D image mip base alignment and descriptor addressing: the separately
  addressed 3D mip chain is aligned and each origin mip level's own array mode
  is used in its resource descriptor. The clear that previously returned
  `VK_ERROR_DEVICE_LOST` through the radeon CS validator, and the full selected
  color-clear matrix, now pass without device loss.
- Layered and 3D blit slice selection: a blit covering more than one slice
  wrote the wrong ones. Two defects combined. The destination slice count was
  forced to equal the source slice count, so a 3D region whose two depth ranges
  differ in size produced too few or too many slices, and the depth axis was
  reduced with `MIN2`, which discards the sign a reversed range carries, so a
  mirrored depth range was not mirrored. Separately, the r8xx pixel shader
  samples the source at a constant array layer, while the loop drew several
  destination slices per draw and advanced the source descriptor only between
  draws, so every layer of a multi-layer array blit received the source's first
  layer. The destination slice now selects its source slice through the
  region's signed, scaled depth range, and one destination slice is drawn per
  draw with the source descriptor re-based for it. `terakan_blit_3d` covers
  minified, magnified and mirrored depth ranges plus a four-layer array blit;
  against the previous code all four groups fail.

- Typed and mirrored 2D blits: `CopyImage` is no longer used for
  format-converting or mirrored operations, and the sign of mirrored coordinate
  transforms is preserved.
- Stencil-aspect view swizzle: a view of the stencil aspect alone is a
  single-component image whose value the Vulkan specification requires in R,
  but the driver was applying the combined depth/stencil format's swizzle,
  which places stencil in G, so sampling it returned a constant zero. View
  descriptors now expose the stencil channel as R. The aspect format tables are
  left alone because transfers use them for source and destination alike, where
  any consistent placement works, which is why transfers never showed this.
  `terakan_stencil_fetch` covers it.
- Multisample depth and stencil texture fetch: `MIP_ADDRESS` doubles as the
  FMASK pointer for multisample textures, and depth and stencil have no FMASK,
  but the driver was leaving it aliasing the base address instead of zeroing
  it, so the hardware treated the surface data itself as FMASK. r600 zeroes it
  explicitly for multisample depth textures. Comparing against r600 running
  OpenGL on the same CAICOS settled it after probes had ruled out addressing,
  tiling, the barrier and the write side: r600 fetches multisample stencil
  correctly there, which proved the hardware supports the fetch and the fault
  was Terakan's. `terakan_stencil_msaa_fetch` covers it.
- Sample-zero depth and stencil resolve: `VK_KHR_depth_stencil_resolve` and
  `VK_KHR_create_renderpass2` are exposed, advertising
  `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` for both aspects and
  `independentResolveNone`. The resolve samples the multisample source and
  exports from a meta pixel shader, one draw per aspect, instead of
  decompressing through DB-to-CB, which returned zero. `terakan_depth_resolve`
  and `terakan_depth_stencil_resolve` clear a 2x attachment, resolve it through
  a `vkCreateRenderPass2` subpass, and check the readback; the destination is
  pre-filled with a different value first, so a resolve that never runs fails
  rather than passing on leftovers.
- Shader clip/cull distance: `shaderClipDistance`/`shaderCullDistance` and
  `maxClipDistances`/`maxCullDistances`/`maxCombinedClipAndCullDistances` (8,
  matching the combined PA_CL_VS_OUT_CNTL CCDIST0/CCDIST1 hardware export
  budget) are exposed. The `terakan_clip_distance` test draws a fullscreen
  triangle with `gl_ClipDistance[0]` set from clip-space X and verifies on
  CAICOS that the negative half is clipped to the clear color while the
  non-negative half is rasterized normally.
- Dynamic uniform/storage buffer descriptor bounds (`#MemoryIntegrity`): the
  `resource[1]` (hardware SIZE) field for `VK_DESCRIPTOR_TYPE_*_DYNAMIC`
  descriptors is reclamped against the real remaining `VkBuffer` extent when
  a dynamic offset is applied, not just the static `range`. The
  `terakan_dynamic_offset_bounds` test poisons a guard region past a small
  buffer's declared size and confirms on CAICOS that a deliberately invalid
  dynamic offset near the end of the buffer no longer exposes it (negative
  control: reverting the clamp makes the test observe the poison pattern).
- R8xx PALM basic compute parity with R9xx CAICOS: the same safe CTS list
  produces 48/48 executed passes on both generations.
- Workgroup barriers nested in dynamically uniform control flow: SFN keeps
  barriers in separate scheduling blocks, and `branch_past_barrier` plus the
  preceding shared-memory trigger pass on R8xx and R9xx.
- Repeated compute dispatch: the regression now contains the required
  shader-write dependency and its iterative oracle passes on RADV, R9xx and
  R8xx with intact guard regions.
- Ordinary compute dispatch, loop constants and non-barrier shader control flow.
- Singleton subgroup `BASIC` and `ARITHMETIC` lowering.
- Dynamic SSBO offsets, `firstInstance` and graphics/compute state restoration.
- Layered buffer/image copies.
- Mipmapped array and cubemap slice layout, including BC6H cube, single-level
  and 2D-array views.
- Color MSAA resolve for the currently tested sample counts, regions, layers
  and RGBA/BGRA formats.
- `VK_EXT_memory_budget` with per-process allocation accounting and
  kernel-informed VRAM/GTT budgets.
- SSBO runtime-array length, writable-SSBO returning reads, compute image
  hazards and graphics/compute cache transition coverage.
- Single-sample formatless storage-image reads and writes: transfer-initialized
  reads and independently generated writes pass exact 17x13 `R32_UINT`
  readback with intact guards. Both corresponding feature bits are exposed.
- `shaderStorageImageExtendedFormats`: `terakan_extended_format_storage_image`
  round-trips imageStore/imageLoad exactly for two representative
  non-mandatory formats (`R16G16_SFLOAT`, `R32G32_SFLOAT`) on real CAICOS
  hardware. The format-capability advertisement in `terakan_format.c` was
  already broad enough; only the feature bit itself needed flipping.
- Storage-image atomics: `terakan_storage_image_atomic` runs 32768
  invocations across 512 workgroups, all hitting the same four `R32_UINT`
  texels with atomicAdd/Min/Max/Exchange. add/min/max land exactly on the
  value only a real RAT atomic operation could produce, repeatably across
  6 runs on real CAICOS hardware -- verified evidence the existing atomic
  lowering is correct, not just advertised.

## Layered depth rendering: the omni-shadow regression

`DB_DEPTH_VIEW`'s `SLICE_START`/`SLICE_MAX` is the only thing selecting which array
layer a depth render targets -- the base addresses deliberately point at the
surface rather than the slice, because DB indexes slices from the base itself.
`terakan_hw_config_draw_set_db_depth_stencil_buffer()`'s early-out for redundant
state compared every field of the descriptor except that one, so two consecutive
layers of the same image were indistinguishable to it: same buffer object, same
`z_info`, same `size`, same `slice`, same base addresses. The update was skipped,
`DB_DEPTH_VIEW` kept the layer it already held, and every cube face of an omni
light's shadow map rendered into face zero while faces one to five kept whatever
the previous owner of that memory contained.

Found by bisecting a real application fault rather than by a test, and worth
recording how, because the honest version of the story is not flattering. The
symptom was intermittent -- a Godot game rendered correct lighting about four
runs in ten, badly lit in three, and speckled in the rest -- and a ten-commit
bisect run at one observation per commit produced a confident but completely
wrong culprit. What actually settled it was measuring instead of observing:
ten runs per configuration, and a debug probe that pre-filled fresh allocations
so that anything read before being written became a known value. That took the
failure rate from six in ten to zero in ten, which proved an uninitialized read;
narrowing the fill to images, then to depth images, then to layered depth images
named the exact resource class, at which point the missing comparison was
obvious on inspection.

The lesson generalizes and has now been learned twice in this project: a single
observation of an intermittent fault carries almost no information, and a bisect
built on single observations is measuring noise. See also the retraction above
concerning the "deterministic" stencil-only MSAA repro.

`terakan_layered_depth_render` covers it: six single-layer views of one six-layer
depth image, each cleared to a value only it should hold, over an image
pre-cleared to a sentinel so an unwritten layer is deterministic rather than
stale VRAM. Against the unfixed driver every layer fails, and the failure names
the mechanism directly -- layer zero holds layer five's value, and layers one
through five still hold the sentinel.

## Cache and barrier coherency: what is covered so far

The single-hazard tests all pass while applications still show frame-to-frame
corruption, so `terakan_frame_chain` tests the composition instead: twenty-four
frames recorded into one command buffer, each producing a colour unique to that
frame, transitioning it to be sampled, sampling it into a second attachment,
transitioning that for transfer and copying it into its own slice of a readback
buffer, with only the barriers a correct application would issue. A frame that
observes a neighbour's colour has seen through a barrier, and the failure names
the frame whose colour it actually saw.

All variants pass on CAICOS: a render pass clear, a compute shader writing a
storage image, (`--compute-ssbo`) a compute shader writing both a storage
image and a storage buffer, with the sampling pass reading the image into RGB
and the buffer into alpha so a stale read of either channel is independently
visible, (`--multi-size`) a second, independently sized 4x4 chain recorded
within the same per-frame block as the main 16x16 one, with its own colour
range disjoint from the main chain's so a texel leaking between the two sizes
is unambiguous, and (`--compute-multi-pipeline`) six distinct `VkPipeline`
objects sharing one shader module and layout, bound round-robin across
frames instead of rebinding the same pipeline object every time. The image
write is chosen because the application that strobes issues thousands of
dispatches per second and the driver's compute RAT coherency is the least
covered path; the buffer producer closed the "storage buffers alongside
storage images" gap this note used to list, `--multi-size` closed "several
render target sizes in the same frame", and `--compute-multi-pipeline`
closed "many distinct compute pipelines rather than one" -- every concrete
gap this note originally named. So whatever the application hits is narrower
than this composition shape.

## Multisample colour copies that are not the whole of two identical surfaces

`vkCmdCopyImage` between two multisample colour images had one path: a byte copy of the whole
surface through CP DMA, which needs the two to be laid out identically and the region to cover
all of both. Anything else fell to the single-sample meta draw, which cannot stand in for it --
it samples the source, and sampling a multisample image resolves rather than copies, and it
writes a single-sample destination.

`dEQP-VK.api.copy_and_blit.core.resolve_image` is where that showed, since five of its groups
copy the multisample image before resolving it. The shape they use is a one-layer source
broadcast into five destination layers, one region per layer, so the two surfaces differ and the
byte copy cannot apply.

The colour counterpart of the depth and stencil path is what it wanted: bind the destination as a
multisample colour target, draw once per sample with `PA_SC_AA_MASK` restricting the draw to the
sample the shader fetched, and fetch that sample from the source with `txf_ms`. Unlike the depth
one it needs nothing from DB, and unlike the byte copy it does not care whether the layouts match
or which layer goes where, because the samples travel through the shader rather than through
memory. One layer per draw, since the source and destination layers are independent.

`resolve_image` goes from 33 failures to 9, all 24 of the copy cases fixed, and a 5906-case
sample of `image_to_image` and `depth_stencil_msaa_copy` stays at 2823 passing and none failing.

### Resolves that move the rectangle

The 9 left were `partial` and `with_regions`: resolves whose source and destination offsets
differ. Evergreen's fixed-function CB resolve reads and writes the same coordinate, so it cannot
express one, and those regions were being skipped -- the resolve silently did nothing for them.

The driver already had a shader path, used for integer formats because averaging is not what
Vulkan asks of them and `CB_RESOLVE` cannot be told to stop. It reaches the source through a
fetch, so it can offset it; what it lacked was a reason to run for anything else, and an average.
Both are now there: the path is taken whenever a region moves the rectangle, the source offset
arrives in the same constants the copy shaders use, and three NIR shaders average two, four or
eight samples for the formats that want an average rather than a selected sample.

The hand-written 2x averaging shader that has sat unused since the beginning is still unused --
it did not decode direct sample coordinates correctly and corrupted the whole frame, which is why
it was disabled, and the NIR ones replace it rather than revive it.

`resolve_image` is now 102 passing and none failing, from 33 failing when this row was written.
A 460-case sample of `renderpass`, `renderpass2` and `pipeline.multisample` has one failure,
`min_sample_shading.min_0_75.samples_2.primitive_triangle`, which fails identically without this
change.

## Multisample depth/stencil copies of anything but the whole image

`dEQP-VK.api.copy_and_blit.*.depth_stencil_msaa_copy` passed 216 of the 324
cases it supports and failed 108, split cleanly by one axis: every `whole` case
passed and every `partial` and `array_to_array` case failed, at all three
sample counts and every depth/stencil format. It now passes 324 of 324.

`whole` was, and still is, an aspect-plane byte copy. That is all it can be:
a partial rectangle is not a contiguous byte range of a tiled surface, and on a
macro-tiled one -- which these are, `TERAKAN_DEBUG_IMAGE_OPS` reports `mode=4`
-- the array layer feeds the bank and pipe swizzle, so the bytes of one slice
do not decode as another. Relaxing the layer condition for linear and 1D-tiled
surfaces was tried first and changed nothing for exactly that reason.

The obvious next shape was the one the single-sample copy uses: bind the
destination aspect as a colour target of the same block size, bind the source
as a texture, draw the destination rectangle, and for multisampling do one draw
per sample with `PA_SC_AA_MASK` restricting each. That was built and measured,
and it does not work:

- forced onto `terakan_copy_image_multisample`, a four-sample 8x8
  `r8g8b8a8_unorm` whole-surface copy, through
  `TERAKAN_DEBUG_DISABLE_IMAGE_CP_DMA=1`, it copies all 64 texels of all four
  samples correctly, so the mechanism itself is sound;
- on depth it fixed nothing and broke the `whole` cases the byte copy gets
  right -- same image, same region, only the mechanism differing, with texel
  (0,1) still holding the clear value while (0,0) was correct. A full sample
  mask left the same texel unwritten, so the mask was not the cause.

A multisample depth surface therefore does not carry its samples where the
colour block puts a colour surface's, and the reinterpretation that makes the
single-sample copy simple stops working past one sample.

What works is a DB destination: `terakan_meta_copy_image_multisample_depth_stencil`
binds the destination as a depth target, and the fragment shader exports the
sample it fetched, one draw per sample with the rasterization sample mask
restricting each to the sample it carries. Stencil needs no per-bit trick, which
is what this was expected to need: `STENCIL_EXPORT_ENABLE` lets the shader
supply the value a `REPLACE` operation writes, which the resolve path already
relied on, so both aspects are the same draw with a different export slot and
`DB_DEPTH_CONTROL`. The source offset reaches the shader through the constants
the single-sample copy already uses, which is what makes `partial` work, and the
source and destination layers are selected independently -- the source through
its texture descriptor's `BASE_ARRAY`, the destination through `SLICE_START` in
DB -- which is what makes `array_to_array` work.

`whole` still takes the byte copy: it is attempted first and this path runs only
when it declines.

## gl_SampleMaskIn reported the wrong number of bits -- fixed

`dEQP-VK.pipeline.*.multisample_shader_builtin.sample_mask` failed 10 of its 30 supported cases:
`pattern`, `correct_bit` and `write` passed, so the bit positions and the output path were right,
while `bit_count` failed at every sample count and `bit_count_0_5` at four and eight samples.

The SPI hands a fragment shader the whole fragment's coverage as `SampleMaskIn`. When the shader
runs once per sample, what it should see is that coverage narrowed to its own sample, and SFN does
the narrowing itself -- `FragmentShader::emit_load_sample_mask_in` computes
`(1 << sample_id) & coverage` -- but only when `r600_shader_key::ps::apply_sample_id_mask` is set.
Terakan left the whole key zeroed, as its own TODO said, so the shader saw the fragment's coverage
and the count was legitimately wrong.

The key is now set for the two cases Gallium r600 sets it for: per-sample iteration, and
multisampling being off at all, where sample zero is the only one to report. The group goes to
none failing.

The earlier note here recorded that masking the value with `BITFIELD_MASK(rasterization_samples)`
changed nothing and concluded that what the register holds had still to be established. That was
the wrong place to look: the register holds the fragment's coverage, which is correct for what it
is, and the surplus bits were not above the sample count but outside the shader's own sample.

Three failures remain in a 2415-case sample of `pipeline.monolithic.multisample`, and all three
fail identically with the key forced back off, so they are separate: two
`min_sample_shading.*.samples_2.primitive_point` cases reporting fewer unique colours than
`minSampleShading` asked for, and one `sample_locations_ext.verify_interpolation` case.

## Depth/stencil clears on small images -- fixed

`dEQP-VK.api.image_clearing.*.clear_depth_stencil_image` failed 101 of the 450 cases it supports
and was the whole of what `api.image_clearing` still failed. It now fails none of them. The clear
was never at fault; the fill before it was.

The size was the only axis that decided the outcome outright: 200x180 and 55x21x11 passed whatever
the format, aspect or layer range, while 1x33, 64x11, 33x128 and 32x29x3 failed, every format
failing at 1x33. Neither the layer range, the aspect, nor the mip level separated them, and the
depth descriptor was byte-identical between a failing and a passing case -- `DB_DEPTH_SIZE` and
`DB_DEPTH_SLICE` decoded exactly right for both. So it was not what the clear was asked to do.

`terakan_depth_clear_extent_probe` rebuilds dEQP's sequence ingredient by ingredient, and the
control that found it was `TERAKAN_PROBE_SPLIT_FILL`, which ends the command buffer between the
fill and the clear: with the fill in a submit of its own, every size passed. Ending it between the
clear and the readback instead (`TERAKAN_PROBE_SPLIT`) did not help the large images, which is
what told the two apart.

`vkCmdCopyBufferToImage` draws into its destination through the color block whatever the image
holds, and it recorded the flush that makes those writes visible in the command buffer's *color*
field. The barrier that followed named a depth image, looked in the depth field, found nothing,
and emitted no flush -- so the tail of the fill was still sitting in the color block and landed on
top of the clear. That is the whole of it, and the numbers are the block's: a near-constant
1216..1984 texels, at most about 8 KB, lost from the end of a large image in tiled order, and a
small image lost whole because all of it still fitted in the block. Hence the size axis, and hence
1x33 failing for every format.

`terakan_CmdCopyBufferToImage2` now picks the field by the destination's aspects.
`clear_depth_stencil_image` goes from 101 failures of 1098 to 0 of 1106, the probe's fifteen sizes
all come back clean in one command buffer and across a split submit alike, and a stride survey of
11239 cases has 39 failures, 38 of them the already-diagnosed `sampler.border_swizzle` group. With
this the whole of `api.image_clearing` passes: 0 failures of 45636, 29340 passed and 16296
unsupported.

Two things ruled out along the way are worth keeping, because they are what the wrong answer would
have looked like. The DB cache flush was not the problem: forcing the barrier down the
`FLUSH_AND_INV_DB_DATA_TS` path instead of `DB_CACHE_FLUSH_AND_INV`, and dropping
`DB_DEST_BASE_ENA` from the `SURFACE_SYNC`, each left the residue exactly as it was. And the loss
was in the image, not in the readback: the probe stamps its readback buffer with a third value
beforehand, and no texel ever came back holding it.

`terakan_CmdCopyImageToBuffer2` has the mirror-image asymmetry -- it writes to a *buffer* through
the CB UAV path but records its flush in the color image field, where a buffer barrier will not
look for it. No failing case has been tied to it yet, so it is left alone rather than changed
blind.

## Random descriptor sets: two remaining shapes

`dEQP-VK.binding_model.descriptorset_random` builds a random descriptor set
layout and has every invocation read every descriptor, writing 1 to its own
texel of an 8x8 storage image when all of them matched. A 1465-case sample of
the family passes 124 and fails 30. Every failing case falls into one of two
signatures, and they are distinct defects.

### All 64 texels wrong

Ten cases, compute and fragment alike, produce a wholly empty result. For
compute this was `CB_TARGET_MASK` overflowing at eight UAVs and is fixed; what
remains under this signature has another cause and has not been sliced yet.

### The same ten texels wrong

Thirteen cases fail on exactly the texels 44, 50-53, 58-62 and no others. The
set is identical across cases whose shaders differ substantially -- different
descriptor types, different counts, with and without input attachments -- and
identical across repeated runs, so it is not a race and not descriptor
dependent. Every invocation runs the same code, so a texel holding 0 means
either that invocation's final `imageStore` never landed or its accumulator was
non-zero.

What has been excluded by measurement, each by reproducing the ingredient in
`terakan_attachmentless_atomic` at the same 8x8 size and finding all 64 texels
correct:

- Rasterization coverage, including the test's own geometry: the case draws a
  four-vertex strip whose z runs from 1 to -1 so the near plane clips it to
  exactly the viewport. Reproducing that vertex shader and topology covers all
  64 texels.
- `imageStore` versus `imageAtomicAdd` as the final write.
- An 8x8 storage image rather than a larger one.
- UAV writes inside divergent control flow ahead of the final write, which the
  failing shaders all contain.
- Shader size and register pressure: the failing shaders use 6 and 9 GPRs
  against 13 for a passing one, and 458 and 544 dwords against 418.

Later rounds excluded three more, each reproduced the same way and each leaving
all 64 texels correct: two typed image RATs in one fragment shader, a
dynamically indexed array of uniform buffers read the way the failing shaders
read theirs (`values[accum + N]`, with `accum` an SSA value the compiler cannot
fold), and a dynamically indexed array of storage images written from divergent
control flow -- which is what makes the backend emit `MEM_RAT ... RATn[IDXm]`,
the indexed RAT writes the failing shaders contain and no earlier reproducer
had. Neither the copy mechanism nor mega-fetch coalescing moves the set either:
`TERAKAN_DEBUG_DISABLE_IMAGE_CP_DMA` and
`TERAKAN_DEBUG_DISABLE_MEGA_FETCH_COALESCING` leave it identical.

Two things are known to change the outcome and are the place to start next.
`TERAKAN_DEBUG_FORCE_LINEAR_IMAGES=1` changes the failing set entirely, to 33
different texels following the clean rule "missing iff `((x>>1)&1) != (y&1)`"
with exactly one exception, texel 62 -- which is also missing in the tiled case.
A rule that misses exactly half the texels is a pairwise address collision, not
a permutation, and a permutation would be invisible here because every written
texel gets the same value. So the write address loses a bit somewhere, and the
bit it loses depends on the tiling.

Order dependence was considered and ruled out for this family, though it is
real for the depth/stencil clears: the same case fails with the same ten texels
run on its own and run after fifteen unrelated passing cases in one process, so
reproducing these shapes in isolation is a valid method here even though it is
not for the clears.

Two further shapes were excluded the same way, both taken from what the failing
shaders actually contain: three separate 1x1 storage images bound alongside the
8x8 destination, and stores into them from divergent control flow through a
dynamically indexed array. Ten reproductions now stand at all 64 texels
correct.

`TERAKAN_DEBUG_DUMP_FRAGMENT_BYTECODE=1` dumps the fragment programs the way
the compute one already did, which is how the indexed RAT writes were found.

### The reduction

`terakan_descriptor_set_shape` is that case standing on its own: the four
descriptor sets it declares, the values it checks for, and its fragment shader
as a file next to the test, so statements can be deleted from it. It reproduces
the failure exactly -- the same ten texels, 44, 50-53 and 58-62 -- and is
registered with `should_fail : true`, so the suite stays green while the defect
is open and says so loudly when it closes.

Deleting statements gives a shape no single ingredient has:

- The reads alone pass and the conditional stores alone pass. Only together do
  they fail, which is why ten reductions that each took one ingredient came back
  clean.
- The store has to be **dynamically indexed**. Keeping only
  `ssbo3_0[accum + 0].val = 33` fails, and so does keeping only the two
  `simage2_4[accum + N]` stores; keeping only `simage1_10` or only `ssbo2_2`,
  the two that are not arrays, passes.
- The reads have to include a **storage buffer** read. Three texel-buffer reads
  plus one SSBO read fail, from either texel binding. Four texel-buffer reads
  without an SSBO read pass, whether they come from one binding or two. Five
  uniform-buffer reads pass. Two texel reads plus an SSBO read pass.

So it is not a count -- four fetches pass and four fetches fail depending on
what they are -- and not a single descriptor, since either texel binding serves.
The smallest failure so far is three texel-buffer reads, one storage-buffer
read, and one dynamically indexed storage-buffer store.

### The cause

Reducing further gave a pair of shaders differing in one thing only: statement
order. With the dynamically indexed store placed *before* the storage buffer
read it fails; with the same statements in the other order it passes. Writing
`accum` out instead of the pass/fail flag says what the failing lanes computed:
`-34`, which is `accum |= temp - 34` with `temp == 0`, so that one read returned
zero instead of the value it holds, and only for those lanes. The texels are
written, with the wrong value; they are not missing.

The bytecode of the failing order shows why:

    0016 ALU:  MOVA_INT __.x, R5.x ; SET_CF_IDX0     <- inside the if
    0022 MEM_RAT WRITE_IND RAT1[IDX0] STORE_TYPED
    0024 POP
    0026 VFETCH R0.x, R3.x, RID:36 ... SQ_CF_INDEX_0 <- outside, index not set

The indexed store inside the branch and the indexed read after it share one
`SET_CF_IDX0`, and it sits inside the branch. `SET_CF_IDX0` loads a wave-scalar
index from a lane of whatever is active when it runs, so the value the read
outside gets is the one an active lane of the branch produced -- here the store's
index, not its own -- and the read lands on the wrong resource and returns zero.
Whether a wave sees it depends on whether that wave took the branch, which is why
some fragments are wrong and not all: the draw is two triangles, and every wrong
texel is on the same side of the diagonal as invocation 33, the one the branch is
taken for.

This is not the assembler's to fix. `emit_index_reg` re-emits the index when the
source register changes or inside a loop, and adding branches to that condition
changes nothing, because the fetch does not ask the assembler for an index: its
`buffer_index_mode` is decided earlier and the index-setting ALU was already
placed inside the branch. The placement is SFN's, and the rule it is missing is
that a value carrying `addr_or_idx` must not be shared with a consumer outside
the block it is established in.

### The fix

`AddressSplitVisitor` in `sfn_split_address_loads.cpp` keeps `m_current_idx_src[2]`,
the source register each index register was last loaded from, and `reuse_loaded_idx`
skips emitting a load whenever the requested source matches one of them. `visit(Block *)`
resets everything else it carries between blocks -- the address load, the address
register, the pending uses, the non-ALU predecessors -- but not that pair, so a
`SET_CF_IDX` emitted inside a conditional block stayed on the books for consumers in
every block after it. Clearing the two entries at the start of each block is the whole
fix: each block now loads the index it uses, where it uses it.

It costs one `SET_CF_IDX` per block that indexes a resource, and buys 21 cases in the
1465-case `descriptorset_random` sample -- 145 passing and 9 failing where it was 124
and 30 -- and six more in the 26745-case stride survey, which goes from 3372 passing
and 13 failing to 3378 and 8. Nothing regressed in either. What still fails is a
different shape. Over the whole group the fix takes 649 failures of 2752 down to 211,
and every one of the 211 is `unifindexed` (100), `dynindexed` (60) or `runtimesize`
(51), where the descriptor *array* index is itself dynamic. Not one `noarray` case
fails any more, which is the shape this was reduced from.

The rule generalises past this driver. Gallium r600 runs the same pass on the same
hardware, so a GLSL shader that stores through a dynamically indexed image inside a
branch and reads an indexed resource after it had the same defect.

### A second one behind it: the scheduler did not wait for the index

With the placement fixed the remainder was all `unifindexed`, `dynindexed` and
`runtimesize`, so the next reduction started from the poorest of those in resources:
a compute case with no uniform buffer, no storage buffer, no sampled image and no
input attachment, just an array of three storage images indexed through a push
constant holding the identity. It is small enough to read whole, and 49 of its 64
invocations were wrong.

Its bytecode puts the fault in one line:

    0004 MEM_RAT WRITE_IND_ACK RAT1[IDX0] NOP_RTN   <- reads through IDX0
    0006 ALU 7 @68 ... SET_CF_IDX0                  <- sets IDX0, too late
    0008 WAIT_ACK
    0010 VFETCH R2.x, R2.z, RID:165, SQ_CF_INDEX_0

The `imageLoad` is a RAT read, and the instruction that loads the index register it
takes its RAT id from was scheduled after it. `split_address_loads` had inserted that
ALU before the RAT, correctly -- but that pass runs before scheduling, and
`RatInstr::do_ready()` reported the RAT ready once its data and coordinate registers
were, without ever consulting its resource offset. `GDSInstr`, `FetchInstr` and
`TexInstr` all check theirs; `RatInstr` was the one that did not, so the scheduler was
free to hoist the read past its own `SET_CF_IDX0` and let it land on whichever element
the register still held. A shader whose array index is a literal never notices, which
is why the rest of `binding_model` passed throughout.

Adding `resource_ready()` to the condition takes `descriptorset_random` from 211
failures of 2752 to 52, with the stride survey going 3378 and 8 to 3379 and 7.
`unifindexed` is now clean outright, as is every compute case; the 52 that remain are
30 `dynindexed` and 22 `runtimesize`, all fragment. `terakan_indexed_image_array` is
the reduced case, and it fails 48 of 64 texels without the fix.

### What the last 52 are, and what they are not

They are one thing, and it is not the rendered image. Every one of the 52 fails on the
write check and only on it -- `Failure in write operation; expected N and found -1` --
so the invocation ran, computed the right `accum` and wrote the right colour, and then
its conditional store to a dynamically indexed resource did not land anywhere. The
destination still holds the -1 it was filled with, and no other element of the array
holds the value instead, so the store was dropped rather than misdirected. Both storage
buffers and storage images are hit, and the index is `accum + 0` as often as `accum + 3`,
so neither the resource kind nor the index value is the variable.

Several things have been ruled out:

* **Not the compaction.** `TERAKAN_DEBUG_RAT` on a failing case gives app indices
  0, 5..8, 30..32 mapping to RAT 0..7 in order, so the four elements of the array the
  lost store targets are contiguous RATs 1..4, exactly what `RAT1[IDX0]` with the index
  in 0..3 addresses. The store that lands and the store that does not are the same
  instruction shape against the same array, one RAT apart.
* **Not the placement of the index load.** The bytecode of both branches is symmetric:
  `MOVA_INT` then `SET_CF_IDX0` inside the branch, then the `MEM_RAT`.
* **Not a lane of the fragment quad.** The failing invocation ids are 8 and 26, both at
  even x and odd y; but stores at 10, 12, 14 and 24 are at even x and odd y too and land.
* **Not a race, though it looked like one.** Running the 52 alone gives 52, 51 and 50
  failures over three runs with the membership moving, but running the whole 2752-case
  group twice gives exactly the same 52 both times. The subset runs differ because what
  precedes each case differs, which is order dependence of the same kind the depth and
  stencil clears show, not nondeterminism within a case.

### The cause: SET_CF_IDX inside a divergent branch

`terakan_descriptor_set_shape` turns out to already have the shape, so the reproducer was
four lines: grow `ssbo3_1` from two elements to four, store to elements 2 and 3 -- which
nothing reads, so a store landing cannot change what any other invocation computes --
under `if (8 == invocationID)` and `if (26 == invocationID)`, and check afterwards what
the buffer holds. Both stores go missing while all 64 texels stay right, exactly as in
dEQP, and printing the whole buffer says where they went: **not nowhere, but to element 1**.

Element 1 is what `CF_IDX1` held before the branch, from the read of `ssbo3_1[accum + 1]`
that precedes it. The `SET_CF_IDX1` the branch contains, correctly placed before the store,
did not take. `SET_CF_IDX` is wave-scalar and takes its value from one lane; inside a branch
one invocation matches, and the lane it reads did not run the `MOVA_INT`, so the index keeps
its pre-branch value and the store lands on the previous statement's resource.

The negative control is decisive: replacing `if (8 == invocationID)` with `if (0 == accum)`,
true for every lane, leaves everything else identical and the store lands on element 2.
Divergence is the variable, not the branch, the resource or the index.

Other things this explains and rules out:

* The store is misdirected, not dropped, so no element ends up holding the value only when
  the wrong element is one dEQP does not check -- which is why it reads as `found -1`.
* Which invocations lose their store is a property of the pixel, not of the shader: with
  every other conditional store removed and one parameterised store left, scanning all 64
  invocations gives exactly 8 and 26, the same two dEQP reports across 52 different shaders.
* Both ingredients are needed. A constant index under the same branch lands, and the same
  dynamic index without the branch lands.

The fix is not a small one, because it pulls against the rule above it. That rule says a
block must load the index it uses; this one says a divergent block cannot load an index at
all. Both are satisfied only by placing the load where the whole wave runs it -- the nearest
enclosing non-divergent block that the index's definition dominates -- which is a change to
where `split_address_loads` puts the load rather than to when it reloads. Gallium r600 runs
the same pass, so a GLSL shader storing through a dynamically indexed resource inside an
`if` has the same defect there.

### That placement was tried, and it costs more than it buys

The obvious form of it was written and measured: track the chain of enclosing blocks while
walking, put an index load whose consumer sits inside a branch into the outermost block
instead -- right before the branch, which is where the whole wave still runs -- and move the
arithmetic that computes the index along with it when that arithmetic had been sunk into the
branch. Keeping the per-block invalidation honest then means keeping a load whose block still
encloses the current one rather than clearing unconditionally.

It does what it was meant to. The bytecode puts `SET_CF_IDX0` in the clause before the
`JUMP`, ahead of the `PRED_SET`, so it runs under the pre-branch mask, and the store that had
been landing on the previous statement's resource lands on its own.

It also loses far more than that. Measured over the whole 2752-case group against the 52 the
current tree fails: moving the arithmetic as well gives **208** failures, and the same change
restricted to indices already established before the branch, moving nothing, still gives
**80**. The stride survey goes from 3379 passing and 7 failing to 3378 and 8. Reverted.

Two holes account for it, and neither is incidental:

* **Every hoisted load lands at the same point.** Two consumers in one branch needing two
  different indices fit, one per index register; a third has to evict one of them, and the
  eviction sits at the same point in the parent as the load it replaces, so it reaches the
  earlier consumer too. Guarding against that by refusing to hoist the third is what the
  measured version did, and it is not enough.
* **Keeping a load because its block encloses the current one is wrong inside a loop.** On the
  second iteration the register may have been rewritten by a load in the body, and the pass
  has no way to tell -- the block list is flat, with a nesting depth and no dominance or
  back-edge information at all.

So the placement this needs is not "the outermost block" but "the nearest enclosing block the
whole wave runs, that the definition dominates and that no other load of the same register
lies between" -- a dominance and liveness question. `split_address_loads` has neither, which
is the actual size of the work: give the pass that analysis first, then the placement follows.



## In-shader memory model: RAT returns, and a GPU hang

`dEQP-VK.memory_model` exercises coherence *inside* a shader, which is the one
thing the `terakan_frame_chain` composition above cannot reach -- it covers
barriers between commands, not two invocations of the same dispatch racing on
one storage image. Only the `core11` cases run; the `ext` ones need
`VK_KHR_vulkan_memory_model`, which is not exposed.

Of the 177 `core11` cases that complete, 47 pass and 51 fail. The failures
concentrate on two axes, both measured on the same run:

- `device` scope fails 45 of 66; `workgroup` scope fails only 7 of 32, and
  `payload_nonlocal.workgroup` (shared memory rather than a UAV) passes 17 of
  17. So the shared-memory path is sound and the UAV path is where device-wide
  visibility is lost.
- The fragment stage fails 21 of 24, against 31 of 74 for compute.

### The hang

118 cases abort the whole CTS process with `VK_ERROR_DEVICE_LOST`; the kernel
reports `ring 0 stalled` with no VM fault, so a wait never completes rather
than an address being wrong. The sharpest reproducer is

    dEQP-VK.memory_model.write_after_read.core11.u32.coherent.fence_fence \
        .atomicwrite.device.payload_local.image.guard_local.image.comp

and the slicing is:

- payload and guard both storage images -- hangs.
- either one replaced by a storage buffer -- fails with a wrong result, no hang.
- `noncoherent` instead of `coherent`, both images -- passes.

`coherent` is what routes the load onto the UAV `NOP_RTN` read path
(`nir_uav_returning_instr_r600` with `V_RAT_INST_NOP_RTN`) instead of a texture
fetch, so the hang needs two typed RATs each issuing a returning MEM_RAT.
`payload_nonlocal.buffer.guard_nonlocal.image` at `workgroup` scope also hangs,
so the shape is not strictly "two images" and the exact boundary is not yet
known.

Comparing the emitted bytecode of the hanging image/image shader against the
merely-failing image/buffer one: the two programs are instruction-for-instruction
identical in control flow, opcodes, acknowledgements and waits. The only
difference is the CB descriptor bound to RAT1 (`RESOURCE_TYPE` 2D versus
BUFFER). The hang therefore comes from the RAT state, not from what the shader
was asked to do.

### Hypotheses measured and rejected

Each of these was implemented, built and measured against the 177-case baseline
of 47 passing, then reverted:

- **A missing `WAIT_ACK` before end-of-program.** `AssamblerVisitor::finalize()`
  sets `end_of_program` without draining outstanding acknowledgements, and a
  marked `MEM_RAT ... STORE_TYPED` really can be the last RAT operation on a
  path (it is, in this shader, at CF 0034 and 0048). Emitting the wait there
  changed neither the hang nor the pass count.
- **Two UAVs sharing one `CB_IMMED` return buffer.** Gallium allocates a
  separate immediate buffer per image view (`evergreen_setup_immed_buffer`),
  while Terakan derives the address from the invocation alone and shares one
  region per element size, so two RATs returning for the same invocation write
  the same address. Giving every hardware UAV index its own region changed
  neither the hang nor the pass count (47 -> 46, within the noise of tests that
  race by construction).
- **`MEM_RAT_NOCACHE` for coherent accesses.** The `device`-scope axis suggests
  the per-render-backend CB cache, and Evergreen has a cacheless MEM_RAT opcode
  that Gallium leaves commented out. Using it for `ACCESS_COHERENT` regressed
  the group from 47 passing to 27. Reverted.

One further observation, not yet acted on: any shader containing a memory
barrier gets `Shader::InstructionChain::prepare_mem_barrier` latched for the
whole shader, so *every* RAT instruction is marked as acknowledged, including
plain stores -- which is exactly the "RAT/WAIT_ACK chain [that] can deadlock
Evergreen compute" the comment in `RatInstr::emit_uav_instr` warns about. The
passing `noncoherent` variants contain barriers too, so this alone is not
sufficient to hang, but it widens the acknowledgement chain far beyond what the
barrier needs.

Until this is understood, a full-suite CTS run must exclude
`payload_*.image.guard_*.image`, or it aborts partway through.

## Signed 2_10_10_10 vertex attributes: the two-bit alpha comes back unsigned

`dEQP-VK.pipeline.*.vertex_input` fails on `a2r10g10b10_sscaled_pack32` and on nothing
else of that family -- the `unorm`, `snorm` and `uscaled` members of the same packed
format all pass. That is four cases, and it would be easy to leave them; the reason not
to is that measuring what the hardware actually returns says the passing `snorm` is
broken too, in a way dEQP happens not to look at.

`terakan_vertex_format_2_10_10_10_probe` fetches one attribute per draw and prints the
four components against what the specification says they should be, over packed words
covering both signs of every field. On Caicos:

* Every one of the three **ten-bit** components is right, in all four number types,
  including at the sign boundary: `sscaled` reads 512 as -512 and 1023 as -1.
* The **two-bit alpha** is decoded as unsigned no matter what the number type says.
  `sscaled` returns 2 for the bit pattern that means -2, and 3 for the one that means -1.
  `snorm` returns 1/3, 2/3 and 1 where the specification asks for 1, -1 and -1.

So `snorm` is wrong for every alpha other than zero. dEQP's `vertex_input` case for it
passes because the values it feeds do not distinguish the two, which is why this went
unnoticed while the `sscaled` case, whose values are exact integers, failed. It matters
beyond conformance: `A2B10G10R10_SNORM_PACK32` is the ordinary way to hand the vertex
stage a packed normal, and its alpha is where a sign or a handedness flag lives.

This is what `TODO(Triang3l): Signed 2_10_10_10 and 10_10_10_2 alpha fixup on certain
chips` in `terakan_vertex_input.c` names. Gallium r600 carries the same fixup, from
`CHIP_PALM` onwards, in `r600_shader.c` -- but only under `!num_format`, the NORM case,
so it would not have covered `sscaled` either.

### The fix

It belongs in the fetch shader and nowhere else: vertex input can be dynamic, so the fetch
shader is regenerated at bind time and is the only place that always has the attribute's
format in hand. `terakan_vertex_input_fs_code` had pre-fetch ALU clauses and no post-fetch
ones, so the generator gained them, emitted between the fetch clause and the return, with
the first one carrying `BARRIER` so it sees what the fetch wrote.

The correction runs in place on the fetched component. For an integer attribute the raw bits
are already there, so two instructions suffice -- `LSHL_INT` by 30 and `ASHR_INT` back, which
is sign extension written out. For the float ones the hardware has already divided by three
in the normalized case, so:

    MULADD  v = v * (normalized ? 0.75 : 0.25) + 0.625
    FRACT   v
    MULADD  v = v * 4.0 - 2.5
    MAX     v = max(v, -1.0)                     (normalized only)

giving 0, 1, -2, -1 and, normalized, 0, 1, -1, -1. The bias is 0.625 rather than 0.5 on
purpose: the normalized path starts from inexact thirds, and an eighth of clearance keeps
every intermediate away from the integer boundary `FRACT` wraps on, where 2/3 would otherwise
be a coin toss.

Measured on Caicos. The probe reports no disagreement with the specification in any of the
four number types. The whole 10574-case `pipeline.monolithic.vertex_input` group goes from 9
failures to 3 -- the four `sscaled` cases and the two `sint` ones are fixed, and what remains
is `max_attributes.query_max_attributes`, which failed before this too and is unrelated. An
11261-case stride sample gives exactly the same 13 failures before and after, and the local
suite is 13/13 and 69/69.

For every format that is not a signed 2_10_10_10 or 10_10_10_2 the emitted fetch shader is
unchanged: no post-fetch clause is created and no control flow entry is added.

## Fragment side effects were lost wherever the depth or stencil test could never pass

A fragment shader's side effects are specified to happen whether or not the fragment survives:
the depth and stencil tests run after it unless the shader asks for early fragment tests. On
this hardware that is `DB_SHADER_CONTROL.Z_ORDER`, which has to be `LATE_Z` for a shader with
memory writes so DB does not reject the fragment before the shader has run.

Terakan set it from `nir->info.writes_memory`, and that was false for every shader here.
`nir_shader_gather_info` derives the flag from the portable intrinsics, and by the time a
shader reaches the backend its storage buffer and image writes have already been lowered by
`terakan_nir_lower_bindings` into `uav_instr_r600`, which `nir_intrinsic_writes_external_memory`
has never heard of. Dumping the NIR of a failing case shows it plainly: one `store_deref`, mode
`nir_var_shader_out`, and the storage buffer write sitting there as `@uav_instr_r600`.

So every fragment shader with side effects ran on `EARLY_Z_THEN_LATE_Z` and without
`EXEC_ON_HIER_FAIL`. `dEQP-VK.rasterization.frag_side_effects` is what noticed: its
`depth_never` and `stencil_never` cases failed with the storage buffer untouched, while `kill`,
`sample_mask` and `alpha_coverage` passed, because a killing shader is late by other means.

Recovering the flag after `nir_shader_gather_info`, by asking whether any UAV instruction is
something other than `NOP` or `NOP_RTN` -- the two that do not write, the second being how a
read is spelled -- makes the family 14 of 14 supported cases, from 4 failing. Two guesses were
tried first and measured to do nothing, and were reverted: `EXEC_ON_NOOP` in the same register,
and `NOOP_CULL_DISABLE` in `DB_RENDER_OVERRIDE`.

## Shared memory: a large copy runs out of registers

Every `_compute_shared` case of `dEQP-VK.glsl.atomic_operations` fails, all sixteen of them, and
every `_compute` and `_fragment` case passes -- eleven operations, signed and unsigned, without
exception. So it is not the atomic. It is the shader around it, which copies a structure of 161
integers from a storage buffer into shared memory and back:

    if (gl_LocalInvocationIndex == 0u) buf.data = result.data;
    ...
    if (gl_LocalInvocationIndex == 0u) result.data = buf.data;

The failure is `r600_schedule_shader: Register allocation failed`, and the shader never compiles,
so the pipeline creation returns `VK_ERROR_UNKNOWN`. Register allocation colours each component
separately and has 123 colours; 161 values wanting the same component do not fit.

They want it at once because of how the two halves are ordered. Every LDS instruction is chained
to the one before it in `LDSReadInstr::split`, so the shared-memory side runs strictly in
sequence, while the storage buffer loads that feed it are independent of each other and the
scheduler hoists them freely. The loaded values then wait, all of them, for a chain that consumes
one per step.

Two things were tried and measured to change nothing, and neither was kept:

* `nir_schedule` in its `fallback` mode, which exists for exactly this ("can be used as a fallback
  when register allocation fails"), run on a retry after the first attempt fails. The retry does
  happen -- the allocator reports its failure twice -- and the second attempt fails the same way,
  so the ordering NIR produces is not what creates the pressure.
* Adding `nir_var_mem_shared` to the load/store vectorizer's modes, which would have turned the
  scalar copy into vector accesses. No change either.

What would work is pressure awareness in the backend scheduler, which currently batches fetches
by count -- fifteen texture instructions, eight memory ones -- and not by how many results are
waiting. That is a change to `BlockScheduler`, and worth taking only with a way to measure it
across the whole suite.

## Two families measured but not yet reduced

Both were turned up by the wider stride sample and are recorded here with what a first pass
established, so a later reduction does not start from nothing.

### Block-compatible views: `texelFetch` was reading a coordinate nobody wrote

`dEQP-VK.image.texel_view_compatible` reads a block-compressed image through an uncompressed
view. Of the five operations it tries, four were clean in a 2160-case sample -- `image_load`,
`image_store`, `texture` and the graphics `texture_read` all passed 102 of 102 -- and
`texel_fetch` split exactly in half, 51 passing and 51 failing, along the shape rather than the
format: 1D failed with and without mipmaps, 2D failed only with them, 3D passed either way.

The 1D half had nothing to do with mipmaps. Every block-compressed image is tiled, and a tiled
1D image cannot be described by a 1D resource, so the driver promotes it to a 2D one -- with a
height of one, which sampling never notices because the address modes fold any row onto the only
row there is. A fetch does notice: an integer coordinate outside the image returns zero rather
than clamping, and the second coordinate was never written. `LowerTexToBackend::lower_txf` fills
the first coordinate and the level and leaves the second null for a non-array 1D fetch, so what
the fetch used was whatever the register held.

Writing a zero there closes the 1D half: 51 failures become 34, all of them `extended`, and
`glsl.texture_functions.texelfetch` stays at 240 of 240.

The `extended` half is a different defect, and it is not one the driver can reach from here. A
view of a non-base mip level is described with a fake base level twice the size, so the wanted
level is reached as level 1 -- the hardware derives the slice pitch of a non-base level from the
height, and a level bound as the base addresses multiple layers wrongly. `BASE_LEVEL` and
`LAST_LEVEL` are both set to 1, which is what keeps sampling on the right level; a fetch takes
its level from the instruction and lands on the fake base instead.

The fake base was measured to be load-bearing rather than merely conservative. Suppressing it
behind an environment variable and running the 540 `extended` cases gives 102 failures against 34
with it, so it is holding up three times as much as it costs -- the operations that pass today
pass because of it.

That leaves biasing the fetch's level by one, and the shader cannot do it. The level a
`texelFetch` names is relative to the view, and for a block-compatible view the only legal value
is zero, so the correction is a property of the descriptor -- which is bound long after the shader
is compiled. The same shader must serve views that need the bias and views that do not.

What would work is a second descriptor for such views, written for fetching rather than sampling
and selected by the instruction, or the count carried in the driver push constants. Both are the
same shape of answer as the cube array size query needs, and neither is small.

### Border colour: the registers hold normalized floats, and half of it is fixed

`dEQP-VK.pipeline.monolithic.sampler.border_swizzle` samples outside the image with a border
colour, through a view with a component swizzle. The border colour was the strongest axis by far,
and it turned out not to be about the swizzle at all: `INT_OPAQUE_WHITE` failed 37 of 44 sampled
cases while every fixed float type was nearly clean.

The per-sampler border colour registers hold **normalized floats**, not the raw values the border
colour was specified with -- Evergreen denormalizes what they hold by the format of the view being
sampled. Writing the integer 1 into them made it a denormal that reads back as zero, which is
exactly what the failures showed. Gallium r600's `evergreen_convert_border_color` divides by the
channel maximum for the same reason.

Three measurements pinned it down, each on `r8_uint.rgba.opaque_white.no_gather`:

- The register path returned zero for *any* value written, 1 and a marker of 7 alike.
- Switching that sampler back to the fixed white type returned 255, which is 1.0 denormalized by
  an 8-bit format -- so the fixed types work and it is the register path that was wrong.
- Writing 1/255 as a float made the case pass.

`terakan_CmdUpdateDescriptorSets` now divides on the way in, where a combined image sampler brings
the view's format together with the sampler. A 3000-case sample goes from 49 failures of 249
supported to 27, and the whole `clamp_to_border` group -- 45779 cases, 3664 supported -- still
passes entirely.

What is left is almost all 32-bit integer formats, and that is the hardware's limit rather than a
remaining bug. Writing a plain 0.5f into the registers and sampling the border reads back zero for
a 32-bit format, while the same probe on a 16-bit one reads back 32768 -- 0.5 * 2^16. So the
denormalization is by 2^bits and 32 bits is past what this path carries.

### What the swizzle does to the border colour, measured

`terakan_border_color_swizzle_probe` writes four *distinct* values into the border colour
registers and samples outside the image through a range of view swizzles, so every result names
the register it came from. dEQP cannot ask this: its border colours are `(1,1,1,1)`, `(0,0,0,1)`
and `(0,0,0,0)`, where a permutation of the components is invisible.

**An ordinary sample applies `DST_SEL` twice.** A view swizzled `argb` -- `(W, X, Y, Z)` -- returns
its border as `(Z, W, X, Y)`, that permutation composed with itself, and the self-inverse `bgra`
comes back as the identity. Every constant an ordinary sample is asked for is produced correctly,
in any component. The registers are now written with the swizzle applied backwards once, which
cancels it; the suite count cannot show this, since equal border components look the same either
way, but a border colour with distinct components needs it.

**A gather reads a constant in the swizzle correctly only in W.** `1gba` gathers its components as
`(Y, Z, W, Y)` rather than `(1, Y, Z, W)`, and `0gba` gathers them as the plain identity, while
`rgb0` and `rgb1` are right. This is the same defect the gather constant-swizzle note above
records, now measured on the border colour path as well, and it accounts for every non-32-bit
failure left in the group -- all of them gathers. Closing it needs a gather-specific descriptor,
not anything the sampler can do.

Immutable samplers still do not get the conversion at all, because their descriptors are written
before any view is known.

## An exclusive scan of a vector read past its identity

The driver reports a subgroup size of one, so an exclusive scan has nothing to its left and must
return the identity of its operation. `terakan_nir_lower_subgroups` builds that identity from
`nir_alu_binop_identity`, which is right, and then handed `nir_build_imm` a single
`nir_const_value` while telling it the result had one component -- whatever the scan's width
actually was. For a vector the builder read past the one value it was given.

Which is why the failures picked themselves out so oddly. Scalars passed; `vec2`, `vec3` and
`vec4` failed. And of the operations, only `mul` and `and` failed, because theirs are the
identities that are not zero: one and all-ones. `add`, `or` and `xor` have an identity of zero
and passed on whatever the read past the end happened to be, which was zero.

Filling one value per component fixes it. A 4029-case sample of `subgroups.arithmetic` goes from
53 failures to none, with 252 passing where 199 did; a 6958-case sample of all of `subgroups` has
one failure left, a compute pipeline that will not create, which is the shared-memory atomic
defect the compare-and-swap work already ran into.

## textureSize on a cube array has nowhere to read the cube count from

The ten `texturesize` failures left in `glsl.texture_functions.query` are all
`samplercubearray*`, and the report is short: for a 1x1 image with six layers, `Expecting: 1x1
and 1 cube(s)`, `Result: (1, 1, 1056964608)`. That number is `0x3F000000`, the bit pattern of
0.5 -- not a count at all, but whatever happened to be lying in the place the shader read.

The place is Gallium's. `TexInstr::emit_tex_txs` masks the third component out of
`GET_TEXTURE_RESINFO` for a cube array and loads it from `R600_BUFFER_INFO_CONST_BUFFER`
instead, which the Gallium driver fills with the cube count. Terakan has no such buffer and
fills nothing, so the shader reads an unrelated constant.

Two ways round it were tried and measured, and neither works:

* NIR's `lower_txs_cube_array` divides the third component by six. That is right where the
  descriptor's depth counts faces, and wrong here: Terakan already writes `TEX_DEPTH` in cubes,
  because that is what a cube map descriptor expresses on this hardware, while `BASE_ARRAY` and
  `LAST_ARRAY` go on addressing individual faces. One cube came back as zero.
* Turning the query into a 2D array one, so the backend takes the ordinary path and never looks
  for the constant buffer, then adding the one `TEX_DEPTH` is short of. One cube then reads
  correctly and two cubes read as one, which says the third component is zero whatever the
  descriptor holds: `GET_TEXTURE_RESINFO` does not report depth for a cube map resource. That is
  presumably why Gallium reaches for a constant buffer rather than the hardware in the first
  place.

So the count has to come from the driver. The natural home is the driver push constants that
already carry things of this kind -- `sampler_unnormalized` is the precedent -- but where that
one needs a bit per sampler this needs a number per texture, which is a good deal more constant
buffer for a query few shaders make. Left undone deliberately, with the measurement recorded so
the next attempt starts from it.

## Texture gather loses a component swizzled to one

`dEQP-VK.glsl.texture_gather` fails 81 of the 231 cases whose view swizzle contains a constant,
and the split is exact. Taking `graphics.basic.2d.rgba8.texture_swizzle`, whose six swizzles are
cyclic shifts of `red_green_blue_alpha`:

* `red_green_blue_alpha` -- passes, it is the identity.
* `green_blue_alpha_zero` -- passes, and it has a constant channel.
* `blue_alpha_zero_one`, `alpha_zero_one_red`, `zero_one_red_green`, `one_red_green_blue` --
  all fail, and each contains `ONE`.

So a channel the view maps to zero gathers correctly and one mapped to one does not, in every
gather mode -- `basic`, `offset`, `offset_dynamic` and `offsets` alike, and for `rgba8`,
`rgba8ui` and `rgba8i` -- which rules out the offset handling that the first sighting of this
in the stride sample suggested.

The swizzle reaches the hardware as `DST_SEL_X..W` of the resource descriptor, and `GATHER4`
picks which channel to gather with the `MODE` field of the fetch instruction. That zero works
says `DST_SEL` is consulted; that one does not says its `1` encoding is not, or does not mean
what it means for an ordinary sample.

### What comes back

`terakan_image_gather` was temporarily pointed at views with chosen swizzles to read the values
out. The image holds channel `k` of texel `n` as `k * 1000 + n`, so which channel a gather
actually reached is legible in the result.

| view swizzle | component 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| `(A, B, G, R)` | A | B | G | R -- all correct |
| `(R, G, B, ONE)` | R | G | B | 1 -- all correct |
| `(B, A, ZERO, ONE)` | B | **G** | **R** | 1 |
| `(ONE, R, G, ZERO)` | **G** | **B** | **A** | 0 |

A pure permutation is applied exactly, so `DST_SEL` is read and understood. A constant in the
last component is applied exactly too, in both the `ZERO` and the `ONE` spelling. A constant
anywhere else makes the gather reach some other channel, and not by a rule these four rows
settle -- the misses are not a consistent shift.

That boundary matches dEQP case for case. Of the six swizzles of
`graphics.basic.2d.rgba8.texture_swizzle`, the two that pass are the identity and
`green_blue_alpha_zero`, whose only constant is in W; the four that fail are exactly those with
a constant anywhere earlier.

So this is not about `ONE` and not about the offset. It is the hardware's channel selection for
`GATHER4` reading `DST_SEL` differently from the way an ordinary sample does, in a way that
happens to agree for a permutation and for a constant in the last slot.

Fixing it in the descriptor is not possible while the same descriptor also has to serve ordinary
sampling, which is correct as things are.

**A gather-specific descriptor would not help either, and the earlier note suggesting one was
wrong.** `terakan_border_color_swizzle_probe` shows what a gather actually does with a constant:
`1gba` gathers its components as `(Y, Z, W, Y)` rather than `(1, Y, Z, W)`, and `0gba` gathers
them as the plain identity rather than `(0, Y, Z, W)`. In both cases the constant is not produced
at all -- a channel is. A second descriptor can only change which channels `DST_SEL` names; it
cannot make the hardware emit a constant for a gather, and the component a gather asks for is the
`MODE` field of the instruction, fixed at compile time. So there is nothing to select.

**Half of it is now fixed, and the other half needs a second descriptor rather than more gathers.**
The shader substitutes the constant itself, told by driver push constants which components of which
slots the bound view makes constant -- `texture_gather_swizzle_constant` and `_one`, one bit per
texture slot per component. A 1200-case sample of `glsl.texture_gather` goes from 62 failures to
32, and the cube subgroup from 12 to 9.

The four-gather idea was investigated and does not work, because selecting between four gathers
requires knowing which component of the instruction reaches which channel. Measuring that directly
-- `terakan_border_color_swizzle_probe` now gathers inside the image, where each channel holds a
distinct value -- gives:

    rgba -> RGBA    rgb0 -> RGB0    rg0a -> RG0A    a01r -> A01R
    bgra -> BGRA    rgb1 -> RGB1    rg1a -> RG1A    gba0 -> GBA0
    0gba -> 0GBA    r0ba -> R0BA

    argb -> ABGR    1gba -> 1BAR    1rgb -> 1BAR    ba01 -> BG01    01rg -> 01BA

Ten of fifteen are correct, including every swizzle whose constants are trailing. The five that are
not follow no rule these fifteen points settle: `1gba` and `1rgb` return the *same* four channels
while asking for different ones, so `DST_SEL` is not being read for those components at all, and no
shift, inverse or double application accounts for `argb` and `ba01` together.

What would close it is a gather-specific descriptor after all, but for a different reason than the
one first recorded here: not to name a constant, which it cannot do, but to carry an identity
`DST_SEL`, whose row above is correct. The shader would then apply the whole swizzle itself --
permutation and constants -- out of the push-constant masks that now exist.

**There is no slot budget for a second descriptor per view.** `terakan_instance.c` gives sampled
images *all* the resource bindings left after uniform buffers, the RTV/UAV range and input
attachments, and `terakan_descriptor.h` asserts that what remains still covers Direct3D 11's 128
sampled images plus storage buffers. Duplicating every sampled image would halve
`maxPerStageDescriptorSampledImages`, taking it below that floor -- and the layout assigns slots
from the descriptor set layout, which cannot see which bindings a shader gathers from, so the
duplication cannot be confined to those.

What fits the budget is the same idea in one slot: program the slot with an identity `DST_SEL` when
the pipeline's shaders gather from it, and have those shaders apply the swizzle themselves for
*every* fetch through that slot, not just the gather. Then four gathers do become the way to get an
arbitrary channel, since with an identity descriptor component C gathers channel C. The pieces are
a per-slot "needs identity" mask carried from the pipeline to descriptor emission, the full swizzle
in the push constants (three bits per slot per component, on top of the constant masks already
there), and a NIR pass that emits the three extra gathers behind a uniform branch so an identity
swizzle keeps paying for one. That is a multi-session change touching descriptor emission, and it
is worth weighing against what it buys: 32 failures in a 1200-case sample.

Measured extent, on a 1200-case random sample of `glsl.texture_gather` (267 passing, 62 failing):
the swizzles whose constant is anywhere but W fail outright -- `one_red_green_blue` 8 of 8,
`alpha_zero_one_red` 11 of 11, `zero_one_red_green` 4 of 4, `blue_alpha_zero_one` 5 of 5 -- while
`green_blue_alpha_zero`, whose constant is in W, passes 5 of 7.

### Gather on a cube map: NUM_FORMAT switches seamless filtering off

The axis is the format, not the cube. Every `rgba8` and `depth32f` cube gather passes and every
`rgba8i` and `rgba8ui` one fails -- 46 of them, wrap modes, both sizes, base levels, the
nearest-filter variant and the corner-excluding variants alike -- while the same integer formats
gathered from a 2D or 2D-array image pass 192 of 192.

`terakan_cube_gather_probe` gathers one direction from two cube images holding the same numbers,
one `R8G8B8A8_UNORM` and one `R8G8B8A8_UINT`. In the middle of a face both return the same four
texels. Near a face edge the unorm image returns `75, 8, 4, 71` -- texels from both faces -- and
the integer image returns `8, 9, 5, 4`, clamped inside the one face. Near a corner the unorm image
reaches three faces and the integer image still reaches one.

The two descriptors differ in exactly one bit: `NUM_FORMAT_ALL`, `NORM` against `INT`. Forcing the
integer image to `NORM` makes it cross the edge like the unorm one. **So `NUM_FORMAT = INT` turns
seamless cube map filtering off**, and this is a property of the hardware rather than of anything
the driver arranges.

`SCALED` also crosses the edge, and unlike `NORM` it delivers the integer value itself: the probe
reads back exactly 75.0, 8.0, 4.0 and 71.0. So an integer cube fetch can be made seamless by
describing the resource as `SCALED` and converting the float back in the shader.

**That was implemented, measured and reverted.** It works: the cube gather group goes from 46
failures to 12, and the 12 left are the constant-swizzle defect above. But the shader cannot see
the view's format, so the descriptor and the conversion can only agree on the rule "an integer
cube view", which drags in 32-bit integer formats -- and a float holds those exactly only up to
2^24. A 2500-case sample of cube views with 32-bit integer formats goes from 0 failures to 219,
all of them `pipeline.monolithic.image...view_type.cube.format.r32_uint` reporting an image
mismatch. Trading 34 for 219 is not a trade, and silently losing precision on a format the suite
does happen to cover would have been worse than the seam.

Closing it properly needs the rule to depend on the channel width, which means telling the shader
which fetches were described as `SCALED` -- a bit per texture slot in the driver push constants,
the same machinery the gather constant-swizzle workaround above would need. Until then this is a
known limitation with an exact description rather than a mystery.


The same sample fails 20 of 44 cube gathers, and only some of them are about the swizzle. The rest
are the wrap-mode variants -- `clamp_to_edge_repeat`, `mirrored_repeat_clamp_to_edge`,
`repeat_mirrored_repeat` -- at both `size_pot` and `size_npot`, failing roughly half the time,
and the `no_corners` variants fail too, which is what rules out the face corners on their own as
the explanation. Shadow gathers on a cube (`compare_less`, `compare_greater`) pass, and so does
every `filter_mode` variant with a linear filter, while the all-nearest one fails. On a cube even
`green_blue_alpha_zero` fails, where on 2D it passes. None of this has been sliced yet.

## Compare-and-swap named its two values the wrong way round

`dEQP-VK.image.atomic_operations.compare_exchange` failed every one of its 64 supported cases,
across all eight image types, both formats and both checks, while every other atomic operation
in the same group passed 76 of 76 in a sample. That narrowness is the whole clue: what
compare-and-swap has and the others do not is a second value.

NIR names them in the order SPIR-V does. `OpAtomicCompareExchange` lists Value before
Comparator, and `fill_common_atomic_sources` reverses that when it fills the intrinsic, so an
image atomic swap gets the comparator in `src[3]` and the value in `src[4]`, and a storage
buffer one gets them in `src[2]` and `src[3]`. Gallium's own backend reads them that way.
`terakan_nir_lower_bindings` read both pairs the other way round, so the hardware compared
against what it should have written and wrote what it should have compared against.

Correcting both takes the image group from 64 failures to none, and
`glsl.atomic_operations.comp_swap*` from 6 failures and nothing passing to 4 passing and 2
failing. The two that remain are `_compute_shared`, atomics on shared memory, which fail at
pipeline creation with `VK_ERROR_UNKNOWN` and failed the same way before this -- a separate
defect in the compiler, not in the source order.

## textureQueryLod returned zero where derivatives are zero

`textureQueryLod` gives the level it would sample in `.x` and the unclamped level of detail in
`.y`. With derivatives of zero the scale factor is zero and that unclamped value is minus
infinity; `GET_LOD` computes in fixed point and hands back zero instead, which is also what a
one-texel derivative gives, so nothing downstream can tell the two apart.

`dEQP-VK.glsl.texture_functions.query.texturequerylod` says it exactly: `Expected: level in
range (0, 0), lod in range (-inf, -31.9961)`, `Result: level: 0, lod: 0`. The upper bound is the
negative of `maxSamplerLodBias`, which this driver reports as just under 32. Every one of the 38
failures was a `_zero_uv_width_fragment` case, one per sampler type, and the level was right in
all of them -- only `.y` was wrong.

NIR already carries the substitution, as `nir_lower_tex_options::lower_lod_zero_width`: it takes
the derivatives of the coordinate, and where they are all zero replaces the raw level of detail
with `-FLT_MAX`. It has to run while the coordinate is still in hand, which is why it belongs in
`nir_lower_tex` rather than anywhere later. Terakan calls `nir_lower_tex` itself, so enabling it
there is the fix, and Gallium r600's own call gets it too, since the hardware is the same.

The family goes from 38 failures to none, 349 passing of 435 with the 10 remaining being
`texturesize`, which failed before this and is unrelated.

## Non-uniform descriptor indexing is advertised and cannot work as things stand

All seven `*ArrayNonUniformIndexing` features are reported as supported. The hardware reaches
an indexed resource through `SET_CF_IDX`, which is wave-scalar: one lane's value decides the
resource for every lane of the wave. That is exactly right for a dynamically uniform index --
which is what `descriptorset_random`'s `unifindexed` and `dynindexed` use, and why they pass --
and wrong for anything else.

`dEQP-VK.descriptor_indexing` is where it shows: 13 of its 15 supported cases fail on image
comparison, across every descriptor type, and their shaders all index through `nonuniformEXT`.
The two that pass, `sampler` and `storage_image_lifetime`, do not.

Adding `nir_lower_non_uniform_access` before the binding pass was tried and measured to change
nothing -- 14 failures of 15 before and after -- although `nir_has_non_uniform_access` reports
the access and the pass reports progress. The reason is that the lowering it emits reads one
lane's index with a subgroup operation and loops until every lane has been served, and this
driver reports `subgroupSize = 1`. Under that model each invocation is its own subgroup, the
loop collapses to a single iteration using the lane's own index, and the wave-scalar hardware
index is right back where it started.

Two ways out, neither small:

* Report a real subgroup size and give the backend wave-level `readFirstInvocation` and ballot,
  after which the standard lowering would work. `SET_CF_IDX` is itself a read-from-one-active-lane
  primitive, so the piece the loop needs is not missing from the hardware, only from the compiler.
* Withdraw the seven features, as `descriptorBindingUpdateAfterBind` was withdrawn for being
  advertised and unimplementable. That makes the driver honest at the cost of the capability.

Until one of them is done the features are a claim the driver does not honour.

## Vertex-pipeline stores: writes work, reads in a vertex stage do not

`vertexPipelineStoresAndAtomics` is still `VK_FALSE`, and the reason recorded next to it -- "the
very small UAV binding count limit in the hardware" -- is not what stands in the way. Turning the
feature on temporarily and running a 265-case sample of
`dEQP-VK.synchronization.op.single_queue.*ssbo_vertex*` gives 48 passing and 92 failing of the 140
supported, and the split is entirely along one axis:

- **Writing a storage buffer from a vertex shader works.** `write_ssbo_vertex_read_copy_buffer`
  passes 8 of 8, and `read_ubo_compute`, `read_ubo_fragment`, `read_ubo_texel_compute` and their
  indirect variants all pass 4 of 4.
- **Reading one in a vertex stage does not.** Every `read_ssbo_vertex` case fails 8 of 8 whatever
  wrote the data -- a copy, a fill, a compute shader, a fragment shader or another vertex shader --
  and so do `read_ubo_vertex`, `read_ubo_texel_vertex` and `read_vertex_input`. All 92 report
  "Memory contents don't match".

The synchronization primitive makes no difference: barrier, event, fence and binary semaphore each
fail 23 and pass 12.

One explanation was tested and excluded. The vertex cache is not it: a shader read in a vertex
stage does not invalidate `VC`, but `terakan_physical_device.c` sets `has_vertex_cache = false` for
CAICOS, so vertex fetch there goes through the texture cache, which the barrier already
invalidates -- and adding the invalidation changed nothing, as it could not.

**The slot hypothesis was tested and is wrong.** The reasoning looked sound: reading a writable
storage buffer takes the UAV path, which needs an IMMED buffer resource, and both IMMED bases --
`UAV_IMMEDIATE_BASE_PIXEL` at 165 and `_COMPUTE` at 164 -- sit above the 160 resource bindings a
vertex stage has, in the sixteen extra ones only pixel and compute shaders get. On top of that,
`terakan_app_config_draw.c` writes the IMMED resource only through `set_resource_cs` or
`set_resource_fs`, so a vertex stage is never given one at all.

But instrumenting the decision shows a vertex stage never reaches that path: with a print on the
branch that would divert it, running a failing case produces nothing, and diverting reads in vertex
stages to the ordinary resource path changes the sample not at all -- still 48 passing and 92
failing. `terakan_nir_get_binding_uav` returns `UINT_MAX` for these bindings before the IMMED
question arises, so the read was already going through an ordinary VFETCH.

So the fault is in the ordinary read path in a vertex stage, not in UAV slot assignment. Worth
noting against that: `terakan_instance_dynamic_ssbo` reads a `readonly` storage buffer from a
vertex shader and passes, so the path works when nothing else writes the buffer. What separates it
from the failing cases is another agent writing the same buffer, which points back at visibility
rather than at addressing -- and the vertex cache, the obvious candidate, is already excluded
above because CAICOS has none.

There is also no slot budget to give vertex stages IMMED resources even if they were the answer:
the mutable range for non-pixel stages is 157 bindings against the 155 the file's own
`static_assert` requires for Direct3D 11, a margin of two where up to twelve would be needed.

## Vertex pipeline stage survey (geometry and tessellation)

Surveyed 2026-08-23 while starting the geometry and tessellation items. Both
feature bits remain `VK_FALSE`, so no application can create a pipeline with
these stages yet and everything below is inert until the whole chain lands.

Already present before this survey: the pipeline layer routes all stages and
picks the hardware stage mapping; `VGT_SHADER_STAGES_EN` covers every
LS/HS/ES/GS/VS combination; thread and stack resource management and the
LSTMP/HSTMP/ESTMP/GSTMP scratch ring sizing are implemented; the shared r600
SFN backend already provides `TCSShader`, `TESShader` and `GeometryShader`
plus `r600_lower_tess_io` and `r600_append_tcs_TF_emission`.

Landed while surveying (regression-tested, inert until the feature bits flip):

- `SQ_PGM_START/RESOURCES/RESOURCES_2` binding for the LS, HS, ES and GS
  stages (`TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_{LS,HS,ES,GS}`), replacing the
  "Bind all the shaders" TODO. A `static_assert` pins the assumption that the
  three registers are consecutive for each of those stages.
- Shader keys for the vertex pipeline stages: `vs.as_ls`, `vs.as_es`,
  `tes.as_es` and `tcs.prim_mode`. The tessellator primitive mode is declared
  by the evaluation shader but needed when compiling the control shader, which
  is compiled first, so the evaluation shader is translated once up front just
  to read it.

- `VGT_LS_HS_CONFIG` and `VGT_TF_PARAM` as ordinary tracked draw config
  entries with setters, emitters and reset defaults. Nothing computes their
  values yet, so both stay at a defined zero; the hardware ignores them while
  the LS and HS stages are disabled.

Evergreen tessellation is on-chip through LDS, not an off-chip factor ring.
There is deliberately no `VGT_TF_MEMORY_BASE` anywhere in the tree: SFN takes
the factor base from a pinned `R0.w` that the hardware supplies, and
`store_tf_r600` lowers to the dedicated `WriteTFInstr` bytecode instruction.
So no new buffer object is needed, which makes this cheaper than a ring would
have been. `evergreen_setup_tess_constants` in `evergreen_state.c` is the
reference implementation of everything below.

Still missing, in rough dependency order:

1. The LDS patch layout: input/output vertex and patch sizes, the patch count
   per thread group, `output_patch0_offset` and `perpatch_output_offset`,
   derived from the control and evaluation shader NIR plus `patchControlPoints`
   from `VkPipelineTessellationStateCreateInfo`.
2. An LDS info constant buffer carrying that layout, bound to the vertex,
   control and evaluation stages. **This collides with Terakan's push constants
   and is the trap to plan for.** SFN's `emit_load_tcs_param_base` fetches two
   vec4s from `R600_LDS_INFO_CONST_BUFFER` at byte offsets 0 and 16, and that
   constant is 15 — the same kcache buffer Terakan already uses for push
   constants, whose first 48 bytes are
   `buffer_uav_base_granularity_offset[12]`. As it stands the tessellation
   parameter bases would read UAV granularity offsets. Either the driver
   constants have to move so bytes 0..31 can hold the eight-`uint32_t` layout
   (`input_patch_size`, `input_vertex_size`, `num_tcs_input_cp`,
   `num_tcs_output_cp`, `output_patch_size`, `output_vertex_size`,
   `output_patch0_offset`, `perpatch_output_offset`, in that order, matching
   `struct r600_lds_constant_buffer`), or `emit_load_tcs_param_base` has to be
   redirected the way `emit_get_lds_info_uint` already was for Terakan.
3. Dynamic `SQ_LDS_ALLOC` for the LS/HS pair, carrying the computed LDS size
   and the wave count, currently emitted as a constant zero on the draw path.
4. Values for `VGT_LS_HS_CONFIG` (patch control point counts, patch count per
   thread group) and `VGT_TF_PARAM` (partitioning, topology, spacing) derived
   from the same state, applied through `terakan_app_config_draw`.
5. Per-stage members of `terakan_shader_static.stage`, which only has `vs` and
   `ps`, plus the matching cases in the `terakan_shader_sfn.cpp` stage switch,
   which only handles `MESA_SHADER_VERTEX` and `MESA_SHADER_FRAGMENT`.
6. Geometry shaders additionally need the copy shader that the hardware VS runs
   in `VS_STAGE_COPY_SHADER` mode, plus the GSVS ring and `VGT_GS_MODE` /
   `VGT_GS_OUT_PRIM_TYPE`. `generate_gs_copy_shader` in `r600_shader.c` builds
   one directly with the bytecode builder, but it takes an `r600_context` and
   would have to be adapted.
7. Tessellation and geometry limits in `terakan_physical_device.c`, which are
   still the two remaining TODOs there.

Tessellation without a geometry shader is the cheaper of the two to finish
first: the evaluation shader runs as the hardware VS (`VS_STAGE_DS`), which is
already bound, so it needs no copy shader.

## Completion policy

Depth/stencil implementation note: Evergreen's `DB_RENDER_CONTROL` copy path
writes to a separately allocated color-compatible flushed-depth surface. The
discarded direct experiment bound the final depth-layout image as a CB target
and left all 256 D32 readback values unchanged. Do not restore that path; use a
transient flushed-depth surface followed by a shader depth/stencil export.

The first staged CAICOS experiment narrowed the remaining problem further. A
single-sample R32 companion was safe and the R32-to-D32 pixel-shader depth
export wrote the destination, but DB-to-CB produced zero in the companion. A
matching 2x companion caused the submission fence to time out and must not be
retried without FMASK/CMASK-safe multisample allocation and addressing. The
next implementation should therefore either complete that metadata first or
extract sample zero into a single-sample surface without binding a multisample
color target.

Reading depth back needs no decompression pass, which narrows the remaining
problem considerably. The `terakan_depth_readback` probe clears a single-sample
`D32_SFLOAT` image through a render pass, copies the depth aspect into a buffer
and gets every texel back exactly, so the ordinary transfer path already reads
depth through an SQ texture fetch. That is consistent with Terakan not
implementing HTILE: depth is stored uncompressed, so there is nothing for the
DB-to-CB copy to decompress.

Multisample depth fetches per sample as well. The `terakan_depth_msaa_fetch`
probe clears a 2x `D32_SFLOAT` image, reads every sample of every texel with
`texelFetch` on a `sampler2DMS` from a compute shader, and gets all 128 values
back exactly, with no hang and no FMASK-style metadata involved. It never binds
a multisample color target, which is what previously timed out the submission
fence, so this is the escape hatch recorded above rather than a retry of the
path that failed. Depth compression uses HTILE, which Terakan does not
implement, so this is independent of the FMASK/CMASK item.

Both halves of a resolve therefore exist, and neither needs the DB-to-CB step
that returned zero: sample the source depth per sample, and write the
destination with the pixel-shader depth export the earlier experiment already
confirmed. `DEPTH_COPY_ENABLE` should not be revived at all. That reduces the
remaining work to a meta draw that binds the source as a multisample texture
and the destination as DB, with a pixel shader applying the requested
`VkResolveModeFlagBits` across the samples and exporting `gl_FragDepth`, plus
the stencil equivalent, the render pass plumbing for
`VK_KHR_depth_stencil_resolve`, and the readback tests that gate exposing it.

FMASK/CMASK memory layout and deferred initialization are now implemented for
2x/4x/8x color images. Memory-requirement checks and bounded CAICOS submissions
for identity FMASK plus compressed-state CMASK initialization pass. This does
not complete the item yet: per-sample texture reads and real color-target
resolve coverage are still required before reusing an MSAA color companion for
depth resolve.

Per-sample colour reads were probed directly for the first time (no test in
this suite had ever exercised `texelFetch` on a multisample colour image --
`terakan_color_msaa_fetch_test`, mirroring `terakan_depth_msaa_fetch`) and
found completely broken. **They now pass at 2x, 4x and 8x**, repeatably, with
each sample given its own distinct colour so that a fetch landing on the wrong
plane is distinguishable from one landing on the right plane. Three separate
bugs had to be fixed, and two of them were only findable once the test was
strengthened.

**1. The 2x identity FMASK constant used the wrong field width.** FMASK stores
one fragment index per sample, and the field width follows the surface's
element size rather than the sample count: 2x and 4x allocate one byte per
pixel and use **2 bits per sample**, while 8x allocates four bytes per pixel
and uses **4 bits per sample**. The identity values are therefore `0x04`
(`0b0100`) for 2x, `0xE4` (`0b11100100`) for 4x and nibbles `0..7`
(`0x76543210`) for 8x. Terakan inherited Gallium r600's constants, whose 4x and
8x values are right but whose 2x value `0x02020202` is a *1*-bit-per-sample
identity -- decoded at 2 bits per sample it asks for fragment 2, a plane a 2x
surface does not have, and measured as garbage for every sample 0. Corrected to
`0x04040404`.

**2. The identity initialization never ran when a render pass performed the
initial layout transition.** It was triggered only from an explicit
`VK_IMAGE_LAYOUT_UNDEFINED` image barrier in `terakan_CmdPipelineBarrier2()`,
but the common runtime does not lower a render pass's
`initialLayout = VK_IMAGE_LAYOUT_UNDEFINED` into a barrier -- when
`can_use_attachment_initial_layout()` succeeds it forwards it as a
`VkRenderingAttachmentInitialLayoutInfoMESA` chained onto the attachment and
deliberately skips the barrier (see `vk_render_pass.c`) -- and Terakan did not
look at that struct anywhere. Confirmed with temporary instrumentation: the
initialization fired 0 times through the render-pass path and 1 time when the
same test issued an explicit barrier, so any application letting its render
pass do the initial transition (legal, and extremely common) rendered with
whatever the previous owner of that memory left in FMASK. Fixed by extracting
`terakan_barrier_initialize_color_metadata()` and calling it from both entry
points.

**3. CB colour compression and fast clear were enabled for sampled images.**
`terakan_image_create_color_descriptor()` set `COMPRESSION` and `FAST_CLEAR` on
every multisample image with metadata. Those are only safe while every read
also goes through CB, which understands CMASK; a texture fetch does not --
`TXF_MS` reads FMASK and then addresses the fragment planes directly, with
nothing consulting CMASK -- so the CB was free to leave planes it had not
actually written holding stale memory, and a fast clear in particular updates
CMASK only. This was isolated by forcing FMASK to all-zeros, pointing every
sample at plane 0 (the one plane a fast clear does write): that made all of
2x/4x/8x pass, which proved the fetch and decode were fine and the planes
simply were not being written. Real drivers resolve this with an FMASK/CMASK
decompress before sampling; Terakan has none, so sampled multisample colour
images are now written uncompressed instead, costing the compression bandwidth
win for those images only.

Two hypotheses were tested and **ruled out** along the way, recorded so they are
not re-derived: the FMASK element size is not wrong (allocating 4x with two
bytes per pixel instead of one changed nothing), and the remaining failures were
not a tiling or bank-rotation addressing mismatch as the earlier notes had
suspected -- the run-to-run instability that suggested that was simply
uninitialized FMASK from bug 2, and it disappeared once that was fixed.

A methodological note worth keeping: the first version of this test cleared
every sample to the same colour, which meant it could only detect a decoded
plane index that was *out of range*, never one that pointed at the wrong but
valid plane. Under that weaker test the wrong constants looked better than the
right ones, because they happened to point more samples at plane 0 -- the only
plane a fast clear writes. Giving each sample its own colour reversed that
conclusion completely and is what made the real encoding legible.

Colour-target resolve was then covered the same way.
`terakan_color_resolve_multivalued` gives every sample its own colour and
checks the resolved single-sample result against the actual arithmetic mean,
with the colours chosen so the mean equals no individual sample's value -- so
a resolve that returns one sample instead of averaging is caught, which no
existing resolve test could do (they all resolve a surface whose samples hold
the same value, where any behaviour looks like an average). It passes at
2x/4x/8x. It deliberately does not declare `VK_IMAGE_USAGE_SAMPLED_BIT`, so
the multisample image keeps CB compression and fast clear enabled, making it
the only coverage of the compressed CB write and CB_RESOLVE path.

With that, this item's acceptance criteria are met: per-sample reads and
resolved reads both pass at 2x/4x/8x, and the rest of the suite stays green
alongside them. What remains is not part of the criteria: reusing an MSAA
colour companion for depth resolve (see the depth-resolve notes above), which
was the original motivation for this work but is tracked with that item.
A TODO is complete only when:

1. the implementation builds from a clean configuration;
2. a focused readback test covers normal, boundary and negative behavior;
3. the test explicitly selects and reports the Terakan CAICOS ICD;
4. existing CPU and GPU regression tests still pass;
5. a relevant game test confirms rendering, without treating a surviving
   process or a single screenshot as sufficient proof.
