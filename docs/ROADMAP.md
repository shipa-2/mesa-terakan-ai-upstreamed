# Terakan development roadmap

Driver-focused tasks (not application-specific hacks).

## Done (this fork)

- [x] Vulkan API 1.1
- [x] `CmdBlitImage2` — 1:1 copy fallback
- [x] Scaled blit meta-shader (R8xx/R9xx)
- [x] Blit: single meta session per call, 128×128 tiles
- [x] Shader binding array dynamic indexing (feature flags)
- [x] `shaderDrawParameters` advertised
- [x] glibc 2.42 / GCC 16 build fixes
- [x] Safe test script `terakan-test-capabilities`

## In progress / next

- [ ] **CmdDrawIndirect** + `multiDrawIndirect` / `drawIndirectFirstInstance`
- [ ] **VK_EXT_descriptor_indexing** (update-after-bind, partially bound)
- [ ] Blit stability under heavy UI upload loads
- [ ] `geometryShader` (limited use on TeraScale)
- [ ] Pipeline cache UUID / `vkGetDescriptorSetLayoutSupport`

## Hardware limits (not fixable by lying to apps)

- **18 samplers per shader stage** — apps needing 512 bindless samplers require descriptor indexing
- Single shared GPU — GPU hang freezes the whole desktop session

## Target hardware

AMD TeraScale: R600, RV770, Cedar, Redwood, Palm, Sumo, Cayman, etc. (non-GCN).
