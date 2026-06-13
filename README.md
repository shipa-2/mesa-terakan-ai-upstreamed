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

## Vulkan API status

Полная матрица: **[docs/VULKAN_STATUS.md](docs/VULKAN_STATUS.md)** (проверено по исходникам Terakan + патчи 0001–0008).

**Приоритеты:** **P0** — обязательно для базового 3D+WSI · **P1** — игры/STK · **P2** — полезно · **P3** — опционально / лимит железа.

**Слои:** **HW** — пакеты в IB · **common** — Mesa `vk_common` (состояние) · **meta** — внутренние shader-pass'ы.

| P | Область | Обязательность | Статус |
|---|---------|----------------|--------|
| P0 | Instance, device, memory, buffer, image | да | ✅ HW + common |
| P0 | Graphics pipeline, shader module, layout | да | ✅ HW + common |
| P0 | Dynamic rendering, draw, bind state | да | ✅ HW + common |
| P0 | Descriptors, barriers, WSI, sync | да | ✅ HW + common |
| P1 | Draw indirect, descriptor indexing | STK | ✅ patches 0007–0008 |
| P1 | Copy/blit/clear, BC textures, queries | игры | ✅ / blit ⚠️ |
| P1 | `shaderDrawParameters` | STK/DXVK | ✅ |
| P2 | Legacy render pass | редко | ⚠️ state only → use dynamic rendering |
| P2 | Compute dispatch | compute-игры | ❌ |
| P2 | Resolve, pipeline cache, `DrawIndirectCount` | — | ❌ TODO |
| P3 | GS/TS, wide lines, RT/mesh | — | ❌ |
| P3 | 512 samplers «at once» (STK ideal) | perf | 🚫 **18/stage** |

**Command buffer:** 25 `Cmd*` с GPU-путём Terakan (draw, transfer, barrier, query, dynamic rendering); ещё ~245 через `vk_common` (viewport, scissor, bind pipeline, dynamic state и т.д.).

**STK:** indexing + indirect ✅; bind 512 textures at once 🚫 (fallback на rebinding).

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
