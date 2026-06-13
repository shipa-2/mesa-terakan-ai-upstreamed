# mesa-terakan-ai-development

AI-assisted development fork of **Terakan** — the Vulkan (and r600 Gallium) driver for AMD TeraScale GPUs (R600–Northern Islands, HD 2000–7000, pre-GCN).

This repository contains **patches**, **build scripts**, and **Arch Linux packaging** on top of upstream Mesa. It does not vendor the full Mesa tree (clone upstream separately).

## Upstream

| Item | Value |
|------|--------|
| Project | [Mesa Terakan](https://gitlab.freedesktop.org/Triang3l/mesa) |
| Branch | `Terakan_Backup_2026-06-10_2_Meta_MSAA` |
| Original author | Vitaliy Triang3l Kuzmin |

## Quick start (developer)

```bash
git clone https://github.com/YOUR_USER/mesa-terakan-ai-development.git
git clone -b Terakan_Backup_2026-06-10_2_Meta_MSAA \
  https://gitlab.freedesktop.org/Triang3l/mesa.git mesa

./scripts/apply-patches.sh mesa

cd mesa
meson setup build-vulkan --prefix=/usr -Dvulkan-drivers=amd_terascale -Dgallium-drivers= ...
meson compile -C build-vulkan
```

See [docs/BUILD.md](docs/BUILD.md) for full build instructions.

## Patches (apply in order)

| Patch | Description |
|-------|-------------|
| `0001-fix-c23.patch` | glibc 2.42+ / C23 `once_flag` compatibility |
| `0002-fix-c23-pthread-casts.patch` | pthread cast fixes for GCC 16 |
| `0003-bump-api-version-1.1.patch` | Vulkan API 1.1 |
| `0004-implement-cmd-blit-image2.patch` | Basic `CmdBlitImage2` (1:1 via copy) |
| `0005-implement-scaled-blit-image2.patch` | Scaled blit meta-shader |
| `0006-blit-batch-and-dynamic-indexing-features.patch` | Blit batching/tiles + dynamic indexing features |
| `0007-implement-draw-indirect.patch` | `CmdDrawIndirect*` + `multiDrawIndirect` |
| `0008-vk-ext-descriptor-indexing.patch` | `VK_EXT_descriptor_indexing` (update-after-bind, partially bound) |
| `0009-fix-scaled-blit-stability.patch` | Scaled blit stability: R8xx `MULADD_IEEE` PS fix, Palm sampler/barrier workarounds |

## Vulkan API status

Полная матрица: **[docs/VULKAN_STATUS.md](docs/VULKAN_STATUS.md)** (проверено по исходникам Terakan + патчи 0001–0009).

**Приоритеты:** **P0** — обязательно для базового 3D+WSI · **P1** — игры/STK · **P2** — полезно · **P3** — опционально / лимит железа.

**Слои:** **HW** — пакеты в IB · **common** — Mesa `vk_common` (состояние) · **meta** — внутренние shader-pass'ы.

| P | Область | Обязательность | Статус |
|---|---------|----------------|--------|
| P0 | Instance, device, memory, buffer, image | да | ✅ HW + common |
| P0 | Graphics pipeline, shader module, layout | да | ✅ HW + common |
| P0 | Dynamic rendering, draw, bind state | да | ✅ HW + common |
| P0 | Descriptors, barriers, WSI, sync | да | ✅ HW + common |
| P1 | Draw indirect, descriptor indexing | STK | ✅ patches 0007–0008 |
| P1 | Copy/blit/clear, BC textures, queries | игры | ✅ / blit ✅ (0009) |
| P1 | `shaderDrawParameters` | STK/DXVK | ✅ |
| P2 | Legacy render pass | редко | ⚠️ state only → use dynamic rendering |
| P2 | Compute dispatch | compute-игры | ❌ |
| P2 | Resolve, pipeline cache, `DrawIndirectCount` | — | ❌ TODO |
| P3 | GS/TS, wide lines, RT/mesh | — | ❌ |
| P3 | 512 samplers «at once» (STK ideal) | perf | 🚫 **18/stage** |

**Command buffer:** 25 `Cmd*` с GPU-путём Terakan (draw, transfer, barrier, query, dynamic rendering); ещё ~245 через `vk_common` (viewport, scissor, bind pipeline, dynamic state и т.д.).

**STK:** indexing + indirect ✅; bind 512 textures at once 🚫 (fallback на rebinding).

## Vulkan 1.1 compatibility

Terakan заявляет **Vulkan 1.1** (`TERAKAN_API_VERSION`). Драйвер **не conformant** (в логе: *testing use only*). Ниже — пробелы относительно **полной** спецификации 1.1 (не 1.2/1.3, без RT/mesh).

### Уже закрыто (1.0 + promoted 1.1)

| Область | Статус |
|---------|--------|
| Graphics pipeline, dynamic rendering, draw / indirect | ✅ |
| Descriptors, push constants, `VK_EXT_descriptor_indexing` | ✅ |
| Memory: `Bind*Memory2`, dedicated, `map_memory2`, external fd/dma-buf | ✅ |
| `GetPhysicalDevice*2`, `GetDeviceQueue2`, `TrimCommandPool` | ✅ |
| Descriptor update template | ✅ (`vk_common`) |
| Barriers, copy / clear / blit, queries, WSI, timeline semaphore | ✅ (blit на R8xx ⚠️) |

**Оценка:** ~**80–85%** entrypoint'ов 1.1 **вызываются** (terakan + `vk_common`); ~**75%** имеют рабочий GPU-путь для типичного 3D.

### Чего не хватает

#### Команды без HW-пути (можно дописать)

| API | Сейчас | Реализуемо? |
|-----|--------|-------------|
| `CmdDispatch` / `Indirect`, `CreateComputePipelines` | объект есть, **CS не исполняется** | ✅ да (Evergreen compute; большой объём) |
| `CmdClearDepthStencilImage` | stub / enqueue | ✅ да (meta, как color clear) |
| `CmdResolveImage` | stub | ✅ да (MSAA resolve) |
| `CmdDrawIndirectCount` / indexed variant | stub | ✅ да (indirect уже есть) |
| `GetDescriptorSetLayoutSupport` | нет (`VK_KHR_maintenance3`) | ✅ да (валидация layout) |

#### Optional features — не включены

| Feature | Сейчас | Реализуемо? |
|---------|--------|-------------|
| `geometryShader` / `tessellationShader` | TODO | ⚠️ частично (NI HW; много работы) |
| `sampleRateShading`, clip/cull distance | TODO | ⚠️ да |
| `wideLines` / `largePoints` | TODO | ⚠️ ограниченно |
| `shaderFloat64` | TODO | 🚫 практически нет на TeraScale |
| `vertexPipelineStoresAndAtomics` (apps) | false | 🚫 мало UAV slots |

#### Расширения экосистемы 1.1 — не включены

| Extension | Сейчас | Реализуемо? |
|-----------|--------|-------------|
| `VK_KHR_maintenance3` | TODO | ✅ да |
| `VK_KHR_multiview` | нет | ⚠️ возможно |
| `VK_KHR_sampler_ycbcr_conversion` | нет | ⚠️ да (большой объём) |
| `VK_KHR_protected_memory` | нет | 🚫 нет на старом Radeon |
| `VK_KHR_device_group` | stub | 🚫 не приоритет (одна GPU) |
| Sparse binding / residency | нет | 🚫 практически нет |

#### Conformance / инфраструктура

| Что | Сейчас | Реализуемо? |
|-----|--------|-------------|
| Pipeline cache UUID | TODO | ✅ да |
| `driverVersion` conformant | заглушка | ✅ да |
| Khronos CTS 1.1 | нет | ⚠️ после compute + gaps |

### Сводка по покрытию 1.1

| Срез | Покрытие |
|------|----------|
| Core API 1.1 (вызовы не падают) | ~**80–85%** |
| Core + optional features (все биты spec) | ~**55–65%** |
| GPU-исполнение заявленного 1.1 | ~**70–75%** |
| Khronos conformance 1.1 | **0%** (testing only) |

**Итог:** для STK / DXVK-lite хватает заявленного; для **формальной полноты 1.1** главные блокеры — **compute**, **`Cmd*IndirectCount`**, **depth clear / resolve**, **maintenance3**.

## Testing (safe, no heavy apps)

```bash
terakan-vulkan-setup vkgears
terakan-test-capabilities   # after install from package
```

## Arch Linux package

```bash
cd packaging/archlinux
makepkg -sf
```

See [docs/PACKAGING.md](docs/PACKAGING.md).

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md) — blit stability, `CmdDrawIndirectCount`, pipeline cache, etc.

## License

Mesa is MIT and other licenses — see upstream `docs/license.rst`. Patches and scripts in this repo are MIT unless noted otherwise.
