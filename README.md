# mesa-terakan-mimo-development

Patch queue, scripts, and Arch Linux packaging for **Terakan** — the Vulkan driver for AMD TeraScale GPUs (R600–Northern Islands, HD 2000–7000, pre-GCN).

This repository does **not** vendor Mesa. Clone upstream separately or use the working tree at `/home/shipa/terakan-mesa-state-rework`.

**Agent workflow:** see [AGENTS.md](AGENTS.md).

**Parallel repo (Cursor/AI):** `/home/shipa/Projects/mesa-terakan-ai-development` — keep `patches/` in sync after merges.

## Upstream

| Item | Value |
|------|--------|
| Project | [Mesa Terakan](https://gitlab.freedesktop.org/Triang3l/mesa) |
| Branch | [`Terakan_state_rework`](https://gitlab.freedesktop.org/Triang3l/mesa/-/tree/Terakan_state_rework) (`a5fc39658` and newer) |
| Author | Vitaliy Triang3l Kuzmin |

## Quick start

```bash
git clone --branch Terakan_state_rework --single-branch \
  https://gitlab.freedesktop.org/Triang3l/mesa.git /home/shipa/terakan-mesa-state-rework

/home/shipa/Projects/mesa-terakan-mimo-development/scripts/apply-patches.sh \
  /home/shipa/terakan-mesa-state-rework

cd /home/shipa/terakan-mesa-state-rework
PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/share/pkgconfig \
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
| `0004-implement-cmd-blit-image2.patch` | Blit meta, batching, STK stability, draw indirect, `VK_EXT_descriptor_indexing` (former 0005–0009) |
| `0010-compute-mvp.patch` | Compute MVP + barriers, SFN kcache redirect, stride/VI fixes, CB UAV guard for CS |

Patches **0005–0009** were merged into **0004** on the `Terakan_state_rework` rebase (2026-06).

**Port status:** [docs/PORTING.md](docs/PORTING.md).

## Current status (Palm, R8xx)

| Test | Result |
|------|--------|
| `terakan-test-compute` | ✅ PASS `{1,2,3,4}` |
| `vkcube` | ✅ |
| `vkgears` | ✅ |
| vkQuake3 (Vulkan) | ⚠️ runs; **multi_texture horizontal stripes** (single_texture OK) |
| SuperTuxKart (Vulkan) | ⚠️ runs; **wheels collapsed, textures torn** |

**Investigation progress:**
- `vkCmdCopyBufferToImage`: ✅ works — root cause was `layerCount=0` in test (sanitize fails); ioquake3 correctly uses `layerCount=1`
- VTX fetch format: ✅ **identical** for single/multi_texture (`0x07961000` = `FLOAT_32_32`, `DST_SEL=X,Y,0,1`)
- Resource descriptors: ✅ all 4 bindings correct (VA, stride=16/4/8/8, size)
- Descriptor sets: ✅ set 0=diffuse, set 1=lightmap bound correctly
- **Next hypothesis:** SPI_VS_OUT_ID / SPI_PS_INPUT_CNTL `spi_sid` mismatch between VS and FS for multi_texture

## Vulkan API status

Full matrix: **[docs/VULKAN_STATUS.md](docs/VULKAN_STATUS.md)**.

| P | Area | Status |
|---|------|--------|
| P0 | Core 3D, WSI, descriptors, barriers | ✅ |
| P1 | Draw indirect, descriptor indexing, blit/copy/clear | ✅ (R8xx scaled blit ⚠️) |
| P1 | Compute MVP (SSBO write) | ✅ patch **0010** |
| P0 | Multi-binding vertex fetch (STK, vkQuake3) | ⚠️ in progress |
| P2 | `CmdDrawIndirectCount`, resolve, pipeline cache, maintenance3 | ❌ TODO |

**Hardware limit:** **18 samplers per stage** — STK cannot bind 512 textures at once (uses fallback).

## Testing

```bash
# After deploy to Palm /tmp/terakan-deploy/
export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json

terakan-test-capabilities   # vkcube, vkgears, vulkaninfo
/tmp/terakan-deploy/terakan-test-compute   # expect PASS

# Build compute test locally
cc -O2 -o terakan-test-compute scripts/terakan-test-compute.c -lvulkan
```

Details and Palm deploy: [AGENTS.md](AGENTS.md) §4–5.

## Arch Linux package

```bash
cd packaging/archlinux
for p in ../../patches/*.patch; do ln -f "$p" .; done
makepkg -sf
```

See [docs/PACKAGING.md](docs/PACKAGING.md).

## Roadmap

[docs/ROADMAP.md](docs/ROADMAP.md) — vertex fetch (P0), then `CmdDrawIndirectCount`, pipeline cache, MSAA resolve, etc.

## License

Mesa is MIT and other licenses — see upstream `docs/license.rst`. Patches and scripts in this repo are MIT unless noted otherwise.
