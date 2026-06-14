# Terakan development roadmap

Driver-focused tasks (not application-specific hacks).

## Done (this fork)

- [x] **Port to `Terakan_state_rework`** — 0004 consolidated, 0010 compute; see [PORTING.md](PORTING.md)
- [x] Vulkan API 1.1
- [x] `CmdBlitImage2` — 1:1 copy fallback
- [x] Scaled blit meta-shader (R8xx/R9xx)
- [x] Blit: single meta session per call, 128×128 tiles
- [x] Blit stability: R8xx skips scaled blit (mipmap gen) until shader hang fixed; R9xx barriers (in **0004**)
- [x] Shader binding array dynamic indexing (feature flags)
- [x] `shaderDrawParameters` advertised
- [x] glibc 2.42 / GCC 16 build fixes
- [x] Safe test script `terakan-test-capabilities`
- [x] **`CmdDrawIndirect`** + `multiDrawIndirect` / `drawIndirectFirstInstance` (patch **0004**)
- [x] **`VK_EXT_descriptor_indexing`** — update-after-bind, partially bound, runtime arrays (patch **0004**)
- [x] Vulkan status matrix — [VULKAN_STATUS.md](VULKAN_STATUS.md)
- [x] **Compute MVP** (patch **0010**): `CreateComputePipelines`, `CmdDispatch`, SSBO write — **`terakan-test-compute` PASS `{1,2,3,4}`** on Palm
- [x] Compute fixes: CB UAV guard, CS barriers, SFN kcache 16→15, push constants `base_vertex`/`base_instance`, stride/VI propagation, `remaining_stages` loop fix, `apply_sq_resources_fetch` unbind limit
- [x] Graphics smoke on Palm: `vkcube`, `vkgears` with deploy ICD
- [x] `vkCmdCopyBufferToImage` — root cause found: `subresource_range_sanitize` fails when `layerCount=0`; ioquake3 correctly uses `layerCount=1`
- [x] VTX fetch format investigation: format_fetch_word1 is **identical** for single_texture and multi_texture pipelines (0x07961000 = FLOAT_32_32)
- [x] Resource descriptors verified: all 4 bindings have correct VA, stride, and size
- [x] Descriptor sets verified: set 0=diffuse, set 1=lightmap bound correctly
- [x] SPI/SFN analysis: `spi_sid` matching mechanism identified in `terakan_shader_sfn.cpp` (lines 153-326)

## In progress / next

- [ ] **P0 — ioquake3 multi_texture horizontal stripes** — single_texture renders correctly (floor, weapon, models). Multi_texture shows horizontal stripes on walls/floors. VTX fetch format confirmed identical; resource descriptors correct. **Next: dump SPI_VS_OUT_ID / SPI_PS_INPUT_CNTL to verify spi_sid matching between VS and FS for multi_texture shaders**
- [ ] R8xx scaled blit meta-shader hang (root cause; `TERAKAN_ENABLE_R8XX_SCALED_BLIT=1` to test)
- [ ] `CmdDrawIndirectCount`
- [ ] MSAA resolve in `CmdEndRendering`
- [ ] `geometryShader` (limited use on TeraScale)
- [ ] Pipeline cache UUID / `vkGetDescriptorSetLayoutSupport`
- [ ] `CmdClearDepthStencilImage` (optional; `ClearAttachments` works for dynamic rendering)
- [ ] `VK_KHR_maintenance3`

## Hardware limits (not fixable by lying to apps)

- **18 samplers per shader stage** — apps needing 512 bindless samplers require descriptor indexing + rebinding (STK fallback)
- Single shared GPU — GPU hang freezes the whole desktop session
- SQ CF ALU kcache bank is 4-bit (0–15) — LDS info const buffer uses slot 15

## Target hardware

AMD TeraScale: R600, RV770, Cedar, Redwood, **Palm** (R8xx), Sumo, Cayman, etc. (non-GCN).
