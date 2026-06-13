# Building Terakan from this repository

## Prerequisites (Arch Linux)

```bash
sudo pacman -S meson ninja python-packaging python-mako python-yaml \
  llvm clang glslang vulkan-headers wayland-protocols git
```

## 1. Clone upstream Mesa

```bash
git clone -b Terakan_Backup_2026-06-10_2_Meta_MSAA \
  https://gitlab.freedesktop.org/Triang3l/mesa.git
```

## 2. Apply patches

From this repo root:

```bash
chmod +x scripts/apply-patches.sh
./scripts/apply-patches.sh /path/to/mesa
```

## 3. Configure Vulkan ICD only

```bash
cd mesa
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

## 4. Compile and install

```bash
meson compile -C build-vulkan
sudo meson install -C build-vulkan
```

Set ICD:

```bash
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/terascale_icd.x86_64.json
vulkaninfo --summary
vkgears
```

## GCC 16 / glibc 2.42+

Patches `0001` and `0002` are required on recent Arch. The PKGBUILD applies them automatically.
