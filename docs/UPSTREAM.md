# Upstream Mesa / Terakan

Terakan is developed by **Vitaliy Triang3l Kuzmin** in the main Mesa tree:

- GitLab: https://gitlab.freedesktop.org/Triang3l/mesa
- **Base branch (2026-06):** [`Terakan_state_rework`](https://gitlab.freedesktop.org/Triang3l/mesa/-/tree/Terakan_state_rework) — pipeline/state management rework (`hw_config_*`, `app_config_draw`, `terakan_meta_impl.h`); reference commit `a5fc39658`
- Legacy backup (pre-rework): `Terakan_Backup_2026-06-10_2_Meta_MSAA` — **do not use** for new work

This repository is a **patch queue + packaging** for mimo-assisted development.

**Working Mesa tree:** `/home/shipa/terakan-mesa-state-rework`

## Sync workflow

```bash
git clone --branch Terakan_state_rework --single-branch \
  https://gitlab.freedesktop.org/Triang3l/mesa.git /home/shipa/terakan-mesa-state-rework

./scripts/apply-patches.sh /home/shipa/terakan-mesa-state-rework
```

When upstream merges our changes, drop or shrink the corresponding patch files from `patches/`.

Keep in sync with `/home/shipa/Projects/mesa-terakan-ai-development` after patch updates.

## Patch layout on `Terakan_state_rework`

| Patch | Role |
|-------|------|
| 0001–0003 | Build / API version |
| 0004 | All former 0004–0009 game features (blit, indirect, indexing, STK stability) |
| 0010 | Compute MVP — barriers, SFN kcache, stride/VI fixes; **PASS** on Palm (`terakan-test-compute`) |

Old numbered patches 0005–0009 are **removed** — their content lives in 0004 for this base.

## Port summary (2026-06)

Full migration notes: **[PORTING.md](PORTING.md)**.

- **Done:** 0001–0004 + 0010 apply cleanly; Vulkan ICD builds; Palm smoke (`vulkaninfo`, `vkcube`, `vkgears`) OK with deploy ICD; compute SSBO write PASS.
- **In progress:** multi-binding vertex fetch (vkQuake3, STK 3D textures).
- **Pending:** Arch package rebuild after patch sync (`pkgrel=11`, branch in PKGBUILD).
