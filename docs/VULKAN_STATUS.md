# Статус Vulkan API в Terakan

Актуально для ветки **`Terakan_state_rework`** + патчи **0001–0004**, **0010** из этого репозитория.

**API:** Vulkan 1.1 (заявлено), целевое железо — AMD TeraScale (R600–Northern Islands).

## Как читать таблицы

### Слой реализации

| Обозначение | Значение |
|-------------|----------|
| **HW** | Собственный путь Terakan — команды кодируются в hardware IB |
| **common** | Mesa `vk_common` — объекты и состояние; для `Cmd*` графического состояния сбрасывается в IB при draw/bind pipeline |
| **meta** | Внутренние meta-shader'ы Terakan (копирование, blit, clear, query copy) |
| **stub** | Вызов `vk_entrypoint_stub` — ошибка при использовании |

Устройство собирает dispatch так: сначала entrypoint'ы Terakan, затем `vk_common` (без перезаписи драйверных). Для command buffer — отдельная `command_dispatch_table` (Terakan + `vk_common`).

### Приоритет

| Приоритет | Смысл |
|-----------|--------|
| **P0** | Обязательно — без этого не работает базовый 3D + swapchain |
| **P1** | Важно для игр (STK, Proton, DXVK) |
| **P2** | Полезно, но есть обходные пути или редко нужно |
| **P3** | Опционально / невозможно на железе / не планируется |

### Статус

| Статус | Значение |
|--------|----------|
| ✅ | Работает |
| ⚠️ | Частично / нестабильно / только через обходной путь |
| ❌ | Не реализовано (feature не advertised или GPU-путь отсутствует) |
| 🚫 | Ограничение железа (нельзя «починить» софтом) |

---

## Сводка по категориям (по приоритету)

| P | Категория | Обязательность | Статус | Слой |
|---|-----------|----------------|--------|------|
| P0 | Instance / Device / PhysicalDevice | обязательно | ✅ | HW + common |
| P0 | Память (allocate, map, bind, external fd/dma-buf) | обязательно | ✅ | HW + common |
| P0 | Buffer / Image / ImageView / Sampler | обязательно | ✅ | HW |
| P0 | ShaderModule / PipelineLayout | обязательно | ✅ | common |
| P0 | Graphics pipeline | обязательно | ✅ | HW |
| P0 | Dynamic rendering (`CmdBeginRendering`) | обязательно | ✅ | HW |
| P0 | Draw / DrawIndexed | обязательно | ✅ | HW |
| P0 | Bind pipeline, viewport, scissor, dynamic state (`CmdSet*`) | обязательно | ✅ | common → flush на draw |
| P0 | Bind descriptors / push constants | обязательно | ✅ | HW |
| P0 | Barriers (`CmdPipelineBarrier2`) | обязательно | ✅ | HW |
| P0 | WSI (swapchain, acquire/present) | обязательно | ✅ | WSI + common |
| P0 | Fence / Semaphore (incl. timeline) | обязательно | ✅ | common |
| P1 | DrawIndirect / multi-draw | для STK | ✅ | HW (patch **0004**) |
| P1 | `VK_EXT_descriptor_indexing` | для STK / bindless-lite | ✅ | HW (patch **0004**) |
| P1 | Copy / UpdateBuffer / FillBuffer | для загрузки ассетов | ✅ | HW + meta |
| P1 | Blit (`CmdBlitImage2`) | UI-текстуры, масштаб | ⚠️ R8xx / ✅ R9xx | meta (0004–0009) |
| P1 | Clear color / attachments | рендер + UI | ✅ | meta + HW |
| P1 | Occlusion / timestamp queries | профилирование, игры | ✅ | HW |
| P1 | BC-текстуры (`textureCompressionBC`) | большинство игр | ✅ | advertised |
| P1 | `shaderDrawParameters` | STK, DXVK | ✅ | advertised |
| P1 | Compute MVP (SSBO write) | compute-игры | ✅ | HW (patch **0010**) |
| P0 | Multi-binding vertex fetch (STK) | STK | ⚠️ | HW — strides/bind OK; VTX fetch under investigation |
| P0 | vkQuake3 multi_texture | vkQuake3 | ⚠️ | HW — format/descriptors OK; spi_sid mismatch hypothesis |
| P2 | `CmdDrawIndirectCount` | некоторые движки | ❌ | TODO |
| P2 | Legacy render pass (`BeginRenderPass`) | старые приложения | ⚠️ | common (state only; целевой путь — dynamic rendering) |
| P2 | `CmdClearDepthStencilImage` | редко | ❌ | depth/stencil через `ClearAttachments` в dynamic rendering |
| P2 | Resolve MSAA | MSAA target | ❌ | TODO в `CmdEndRendering` |
| P2 | Pipeline cache | cold start | ❌ | TODO |
| P2 | `vkGetDescriptorSetLayoutSupport` / KHR_maintenance3 | валидация layout | ❌ | TODO |
| P3 | Geometry / Tessellation shaders | редко | ❌ | TODO, ограниченная польза на TS |
| P3 | Wide lines / large points | UI/debug | ❌ | TODO |
| P3 | Sample-rate shading | MSAA quality | ❌ | TODO |
| P3 | Ray tracing / mesh / task shaders | современные AAA | ❌ | нет железа |
| P3 | 512+ samplers «bind at once» (STK ideal) | STK perf | 🚫 | **18 samplers / stage** — STK использует fallback |

---

## Расширения и features (заявленные)

### Включены и работают

| Расширение / feature | P | Примечание |
|---------------------|---|------------|
| `VK_KHR_dynamic_rendering` | P0 | Основной путь рендера |
| `VK_KHR_swapchain` (+ mutable format) | P0 | WSI |
| `VK_KHR_timeline_semaphore` | P0 | |
| `VK_KHR_external_memory` (+ fd, dma-buf на Linux) | P0 | |
| `VK_KHR_bind_memory2`, `map_memory2` | P0 | |
| `VK_EXT_descriptor_indexing` | P1 | patch **0004** |
| `VK_EXT_extended_dynamic_state` (+3) | P0 | Не все dynamic state3 bits |
| `VK_EXT_vertex_input_dynamic_state` | P0 | |
| `VK_EXT_depth_clip_enable` / `depth_clip_control` | P0 | |
| `VK_EXT_provoking_vertex` | P1 | |
| `VK_EXT_host_query_reset` | P1 | |
| `VK_EXT_sample_locations` | P2 | meta пока без custom locations |
| `VK_KHR_vertex_attribute_divisor` | P1 | instancing |
| `VK_EXT_4444_formats`, `non_seamless_cube_map`, `color_write_enable` | P2 | |
| `VK_KHR_format_feature_flags2` | P1 | |
| `VK_EXT_texel_buffer_alignment` | P0 | |

### Не включены / TODO

| Расширение | P | Причина |
|------------|---|---------|
| `VK_KHR_maintenance3` | P2 | нет `GetDescriptorSetLayoutSupport` |
| `VK_KHR_maintenance4` | P2 | maxBufferSize, 32-bit VA limits |
| `VK_EXT_extended_dynamic_state2` | P2 | не весь state |
| `VK_EXT_custom_border_color` | P3 | нужно исследование форматов |
| Compute extensions (apps) | P1 | базовый dispatch работает; indirect/base group — TODO |
| RT / mesh extensions | P3 | нет GPU-пути |

### Vulkan 1.0 features (выборочно)

| Feature | P | Статус |
|---------|---|--------|
| `multiDrawIndirect`, `drawIndirectFirstInstance` | P1 | ✅ (**0004**) |
| `robustBufferAccess` | P0 | ✅ |
| `independentBlend`, `dualSrcBlend`, `logicOp` | P0 | ✅ |
| `depthClamp`, `depthBiasClamp`, `fillModeNonSolid` | P0 | ✅ |
| `samplerAnisotropy`, `textureCompressionBC` | P0/P1 | ✅ |
| `shaderDrawParameters` | P1 | ✅ |
| Dynamic indexing (UBO/SSBO/image/sampler arrays) | P1 | ✅ |
| `geometryShader`, `tessellationShader` | P3 | ❌ |
| `wideLines`, `largePoints`, `sampleRateShading` | P3 | ❌ |
| `shaderFloat64`, `shaderClipDistance` | P3 | ❌ |
| `vertexPipelineStoresAndAtomics` (apps) | P2 | ❌ (только meta) |

---

## Command buffer: 28 HW + 245 common

Terakan переопределяет **28** `Cmd*` (включая compute dispatch). Остальные **245** идут через `vk_common` (запись состояния; GPU — при draw/meta).

### P0 — HW (`Cmd*` с GPU-путём Terakan)

| Команда | Статус | Файл / модуль |
|---------|--------|---------------|
| `CmdBeginRendering` | ✅ | `terakan_vk_render_pass.c` |
| `CmdEndRendering` | ✅ | `terakan_vk_render_pass.c` |
| `CmdDraw` | ✅ | `terakan_vk_draw.c` |
| `CmdDrawIndexed` | ✅ | `terakan_vk_draw.c` |
| `CmdDrawIndirect` | ✅ | `terakan_vk_draw.c` (0004) |
| `CmdDrawIndexedIndirect` | ✅ | `terakan_vk_draw.c` (0004) |
| `CmdDispatch` | ✅ | `terakan_vk_dispatch.c` (0010) |
| `CmdDispatchIndirect` | ⚠️ | `terakan_vk_dispatch.c` — CPU map at record time |
| `CmdBindIndexBuffer2` | ✅ | `terakan_vk_draw.c` |
| `CmdBindVertexBuffers2` | ✅ | `terakan_vk_state.c` |
| `CmdBindDescriptorSets` | ✅ | `terakan_pipeline_layout.c` |
| `CmdPushConstants2KHR` | ✅ | `terakan_push_constants.c` |
| `CmdPipelineBarrier2` | ✅ | `terakan_barrier.c` |
| `CmdCopyBuffer2` | ✅ | `terakan_cp_dma.c` |
| `CmdUpdateBuffer` | ✅ | `terakan_cp_dma.c` |
| `CmdFillBuffer` | ✅ | `terakan_cp_dma.c` |
| `CmdCopyImage2` | ✅ | meta |
| `CmdCopyBufferToImage2` | ✅ | meta |
| `CmdCopyImageToBuffer2` | ✅ | meta |
| `CmdBlitImage2` | ⚠️ R8xx | meta (patch **0004**; R8xx skips scaled blits — STK-safe, no mipgen) |
| `CmdClearColorImage` | ✅ | meta |
| `CmdClearAttachments` | ✅ | meta |
| `CmdBeginQueryIndexedEXT` | ✅ | `terakan_query.c` |
| `CmdEndQueryIndexedEXT` | ✅ | `terakan_query.c` |
| `CmdResetQueryPool` | ✅ | `terakan_query.c` |
| `CmdWriteTimestamp2` | ✅ | `terakan_query.c` |
| `CmdCopyQueryPoolResults` | ✅ | meta |

### P0 — common (графическое состояние, работает через flush)

`CmdBindPipeline`, `CmdSetViewport`, `CmdSetScissor`, `CmdSetPrimitiveTopology`, весь набор `CmdSet*` из `EXT_extended_dynamic_state` / `EXT_extended_dynamic_state3`, `CmdBindVertexBuffers`, `CmdBindIndexBuffer`, `CmdPushConstants`, `CmdSetVertexInputEXT`, и т.д.

### P1–P2 — common, но без GPU-пути Terakan (⚠️ или ❌)

| Команда | P | Статус | Комментарий |
|---------|---|--------|-------------|
| `CmdBeginRenderPass` / `2` | P2 | ⚠️ | State only; используйте dynamic rendering |
| `CmdClearDepthStencilImage` | P2 | ❌ | Используйте `ClearAttachments` |
| `CmdResolveImage` / `2` | P2 | ❌ | TODO |
| `CmdDrawIndirectCount` | P2 | ❌ | TODO |
| `CmdBlitImage` (v1) | P2 | ⚠️ | Перенаправление на v2 где возможно |
| Ray tracing / mesh / NV `Cmd*` | P3 | ❌ | stub или irrelevant |

---

## Device-level: 39 HW + 315 common

### P0 — создание ресурсов (HW)

`CreateBuffer`, `CreateBufferView`, `CreateImage`, `CreateImageView`, `CreateSampler`, `CreateDescriptorSetLayout`, `CreateDescriptorPool`, `CreatePipelineLayout`, `CreateGraphicsPipelines`, `CreateCommandPool`, `CreateQueryPool`, `AllocateMemory`, `Bind*Memory2`, `UpdateDescriptorSets`, `AllocateDescriptorSets`, …

### P0 — common (объекты без HW-специфики)

`CreateShaderModule`, `CreateFramebuffer`, `CreateRenderPass`/`2`, `CreateSemaphore`, `CreateFence`, `BeginCommandBuffer`, `QueueSubmit`/`2`, …

### P2 — ❌ или неполно

| API | Статус |
|-----|--------|
| `CreateComputePipelines` | ✅ patch **0010** |
| `CreatePipelineCache` / cache UUID | ❌ TODO |
| `GetDescriptorSetLayoutSupport` | ❌ TODO (нет maintenance3) |

---

## Лимиты железа (🚫)

| Лимит | Значение | Влияние |
|-------|----------|---------|
| Samplers per stage | **18** | STK «bind 512 textures at once» = false; fallback `descriptorCount=1` |
| Sampled images per stage | ~**128** (non-PS) / меньше на PS | descriptor indexing помогает, но не 512 |
| Storage buffers (app) | **4** / stage | DXVK/Vulkan минимум — ок, большие bindless — нет |
| Vertex pipeline atomics (app) | **0** | только meta |
| Max buffer VA | **32-bit** в IB | maintenance4 не включён |
| MSAA resolve in EndRendering | — | ❌ TODO |

---

## STK / vkQuake3 — проверка capabilities

| Проверка | Terakan |
|----------|---------|
| `VK_EXT_descriptor_indexing` | ✅ |
| non-uniform indexing | ✅ |
| partially bound | ✅ |
| multi-draw indirect | ✅ |
| shader draw parameters | ✅ |
| bind ≥512 textures at once | 🚫 false (18/stage) |
| bind mesh textures at once (512×2) | 🚫 false |
| 3D rendering (multi-binding VI) | ⚠️ textures smeared / geometry collapsed |

STK: Vulkan renderer, 4 vertex bindings (position/16, color/4, texcoord/8, lightmap/8). Menu/2D OK; 3D wheels at center, textures torn.

vkQuake3: Vulkan init OK; multi_texture surfaces (walls/floors) show horizontal stripes. Single_texture surfaces (weapon, models, HUD) render correctly.

**Investigation results (2026-06):**
- `vkCmdCopyBufferToImage`: ✅ works — root cause was `layerCount=0` in test; ioquake3 correctly uses `layerCount=1`
- VTX fetch format_fetch_word1: **identical** for single/multi_texture (`0x07961000` = `FLOAT_32_32`, `DST_SEL=X,Y,0,1`)
- Resource descriptors: ✅ all 4 bindings have correct VA, stride=16/4/8/8, and size
- Descriptor sets: ✅ set 0=diffuse, set 1=lightmap bound correctly
- **Next:** SPI_VS_OUT_ID / SPI_PS_INPUT_CNTL `spi_sid` mismatch — SPI hardware matches VS exports to FS inputs by `spi_sid` (= `varying_slot + 1`); if spi_sid doesn't match, FS reads uninitialized data

STK uses `single_descriptor=false`, `descriptorCount=1` fallback for sampler limit — works but not optimal path.

---

## Compute (patch **0010** — работает)

**Проверено на Palm (AMD R8xx, 192.168.1.80).** Деплой: `/tmp/terakan-deploy/libvulkan_terascale.so` + ICD `scripts/terascale_icd.deploy.json`.

### Исправления (сессия compute)

| Проблема | Исправление |
|----------|-------------|
| SSBO `{0,0,0,0}` | CB UAV guard: не затирать `uav_used` когда активен compute shader |
| CB UAV path | `UAV_IMMEDIATE_BASE_COMPUTE` + `set_resource_cs()` вместо FS path |
| Barriers | CB UAV flush + CS partial flush до dispatch; TC/VC/SH invalidation после |
| SFN draw params | `R600_LDS_INFO_CONST_BUFFER` 16→15; offsets 48/52/56; `base_vertex`/`base_instance` в push constants |
| Stride = 0 | Stride всегда из pipeline; dynamic VI propagation в `terakan_vk_state.c` |
| `remaining_stages` loop | `remaining_stages &= remaining_stages - 1` в `terakan_pipeline_layout.c` |
| Fetch unbind | `apply_sq_resources_fetch`: limit `unbound_resources` to `< MUTABLE_BASE` |

### Текущий статус

| Компонент | Статус |
|-----------|--------|
| `CreateComputePipelines` | ✅ |
| `CmdDispatch` → IB | ✅ `PKT3_DISPATCH_DIRECT` + `TERAKAN_PACKET3_COMPUTE` |
| Fence после dispatch | ✅ без GPU hang |
| SSBO write | ✅ `{1,2,3,4}` |
| `vkcube` / `vkgears` | ✅ с тем же `.so` |
| `CmdDispatchIndirect` | ⚠️ CPU map at record time |
| `CmdDispatchBase` (non-zero base) | ❌ TODO — `VGT_COMPUTE_START_*` |
| GPU indirect dispatch | ❌ TODO — `PKT3_DISPATCH_INDIRECT` |

### Тест

```bash
cc -O2 -o terakan-test-compute scripts/terakan-test-compute.c -lvulkan

export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json
/tmp/terakan-deploy/terakan-test-compute
# ожидаемо: PASS, data[0..3] = {1,2,3,4}
```

---

## Известные проблемы

### P0 — vkQuake3 multi_texture horizontal stripes

BSP surfaces with multi_texture pipeline (diffuse + lightmap) show horizontal stripes. Single_texture surfaces work correctly. VTX fetch format, resource descriptors, and descriptor sets all verified correct. Root cause hypothesis: SPI_VS_OUT_ID / SPI_PS_INPUT_CNTL `spi_sid` mismatch between VS and FS for multi_texture shaders. Key file: `terakan_shader_sfn.cpp` (lines 153-326). Debug needed: dump `SPI_VS_OUT_ID` and `SPI_PS_INPUT_CNTL` values during shader compilation.

### P0 — STK 3D vertex fetch (multi-binding)

3D models use separate vertex buffers per attribute via `vkCmdBindVertexBuffers`. Strides and resource descriptors appear bound correctly in debug; rendering still wrong. Next investigation: VTX fetch shader instructions, vertex attribute formats, sampler state.

### R8xx scaled blit hang

`CmdBlitImage2` with scaling (mipmap gen) hangs on R8xx. Disabled by default; test with `TERAKAN_ENABLE_R8XX_SCALED_BLIT=1`.

---

## Связанные патчи этого форка

| Patch | Что меняет в статусе |
|-------|----------------------|
| 0003 | API 1.1 |
| 0004 | Blit (scaled + STK stability) + draw indirect + descriptor indexing |
| 0010 | Compute MVP + stride/VI/SFN fixes |

---

## Как перепроверить локально

```bash
terakan-vulkan-setup vulkaninfo | less
terakan-test-capabilities
terakan-vulkan-setup vkgears

export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json
/tmp/terakan-deploy/terakan-test-compute

# STK / vkQuake3 on Palm — see AGENTS.md §5.3–5.4
```

Полный список entrypoint'ов: `build-vulkan/.../terakan_entrypoints.c` и сравнение с реализациями в `src/amd/terascale/vulkan/`.
