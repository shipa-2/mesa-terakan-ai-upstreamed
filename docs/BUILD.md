# Building Terakan from this repository

## Prerequisites (Arch Linux)

```bash
sudo pacman -S meson ninja python-packaging python-mako python-yaml \
  llvm clang glslang vulkan-headers wayland-protocols git
```

## 1. Clone upstream Mesa

```bash
git clone --branch Terakan_state_rework --single-branch \
  https://gitlab.freedesktop.org/Triang3l/mesa.git /home/shipa/terakan-mesa-state-rework
```

Or use any path; pass it to `apply-patches.sh`.

## 2. Apply patches

From this repo root:

```bash
chmod +x scripts/apply-patches.sh
./scripts/apply-patches.sh /home/shipa/terakan-mesa-state-rework
```

Order: **0001 → 0002 → 0003 → 0004 → 0010**. Patches 0005–0009 do not exist.

**Alternative:** edit sources directly in the Mesa tree and regenerate patches with `git diff` — see [AGENTS.md](../AGENTS.md) §3.

## 3. Configure Vulkan ICD only

```bash
cd /home/shipa/terakan-mesa-state-rework
rm -rf build-vulkan

PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/share/pkgconfig \
meson setup build-vulkan \
  --prefix=/usr \
  --libdir=lib \
  --buildtype=release \
  -Dvulkan-drivers=amd_terascale \
  -Dgallium-drivers= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dopengl=false -Dllvm=disabled \
  -Dplatforms=x11,wayland \
  -Dc_args='-Wno-incompatible-pointer-types'
```

**Important:** set `PKG_CONFIG_PATH` — without it Meson may pick 32-bit libraries from `/usr/lib32/`.

## 4. Compile and install

```bash
meson compile -C build-vulkan
sudo meson install -C build-vulkan   # optional
```

Artifact: `build-vulkan/src/amd/terascale/vulkan/libvulkan_terascale.so` (~10 MB).

For WIP testing on Palm, copy to `/tmp/terakan-deploy/` instead of system install — see [AGENTS.md](../AGENTS.md) §4.

System ICD after install:

```bash
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/terascale_icd.x86_64.json
vulkaninfo --summary
vkgears
```

## GCC 16 / glibc 2.42+

Patches `0001` and `0002` are required on recent Arch. The PKGBUILD applies them automatically.

## Verify compute

```bash
cc -O2 -o terakan-test-compute scripts/terakan-test-compute.c -lvulkan
export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json
/tmp/terakan-deploy/terakan-test-compute   # expect PASS {1,2,3,4}
```
