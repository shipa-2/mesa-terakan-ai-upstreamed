# Arch Linux packaging

Package definitions live in `packaging/archlinux/`.

## Build package

```bash
cd packaging/archlinux
makepkg -sf
sudo pacman -U vulkan-terakan-*.pkg.tar.zst
```

Produces:

- `vulkan-terakan` — 64-bit Vulkan ICD + r600 in `/usr/local`, helper scripts
- `lib32-vulkan-terakan` — 32-bit ICD for Wine

## Helper scripts (installed to `/usr/bin`)

| Script | Purpose |
|--------|---------|
| `terakan-vulkan-setup` | Run app with Terakan ICD |
| `terakan-vulkan32-setup` | 32-bit ICD for Wine |
| `terakan-dx-setup` | Wine + Terakan convenience |
| `terakan-test-capabilities` | Safe vkgears/vkcube/vulkaninfo probe |

## Kernel module

After install, ensure `radeon` loads with UMS disabled (see `terakan.conf`):

```bash
sudo mkinitcpio -P && sudo reboot
```
