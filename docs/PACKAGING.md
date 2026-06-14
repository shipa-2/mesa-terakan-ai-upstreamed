# Arch Linux packaging

Package definitions live in `packaging/archlinux/`.

**Upstream branch:** `Terakan_state_rework` (см. [PORTING.md](PORTING.md)).

## Build package

```bash
cd packaging/archlinux

# makepkg ищет локальные source только по basename в каталоге PKGBUILD —
# после изменения patches/ обновите hardlink'и:
for p in ../../patches/*.patch; do ln -f "$p" .; done

makepkg -sf
sudo pacman -U vulkan-terakan-*.pkg.tar.zst lib32-vulkan-terakan-*.pkg.tar.zst
```

Produces:

- `vulkan-terakan` — 64-bit Vulkan ICD + r600 in `/usr/local`, helper scripts
- `lib32-vulkan-terakan` — 32-bit ICD for Wine

### PKGBUILD

| Поле | Значение |
|------|----------|
| `_branch` | `Terakan_state_rework` |
| `pkgrel` | 11 (state_rework + 0010) |
| `prepare()` | 0001, 0002, 0003, 0004, **0010** (без 0005–0009) |

`pkgver()` берётся из git rev upstream при сборке.

### Частые проблемы

| Симптом | Причина | Решение |
|---------|---------|---------|
| `0001-fix-c23.patch не найден` | Путь `../../patches/` — makepkg не видит | Hardlink патчей в `packaging/archlinux/` (см. выше) |
| `sha256sums` mismatch | Патч обновлён | `makepkg -g` в каталоге PKGBUILD, обновить массив |
| `terakan_meta_blit_image.c already exists` при 0004 | Остатки прошлой сборки в `/var/tmp/makepkg/` | `rm -rf /var/tmp/makepkg/vulkan-terakan`; в PKGBUILD `prepare()` — `git clean -fd` |
| Старый клон `packaging/archlinux/mesa` на другой ветке | Кэш VCS не на `Terakan_state_rework` | `rm -rf packaging/archlinux/mesa` и пересобрать |
| `terakan_app_config_compute.c does not exist` | Старый **0010** без новых файлов | Обновить `patches/0010-compute-mvp.patch`, синхронизировать hardlink |

## Helper scripts (installed to `/usr/bin`)

| Script | Purpose |
|--------|---------|
| `terakan-vulkan-setup` | Run app with Terakan ICD |
| `terakan-vulkan32-setup` | 32-bit ICD for Wine |
| `terakan-dx-setup` | Wine + Terakan convenience |
| `terakan-test-capabilities` | Safe vkgears/vkcube/vulkaninfo probe |
| `terakan-test-mipmap-blitz` | STK-style mipmap blit probe; checksums vs RADV reference |

## Kernel module

After install, ensure `radeon` loads with UMS disabled (see `terakan.conf`):

```bash
sudo mkinitcpio -P && sudo reboot
```

## Deploy на Palm без pacman

Для проверки свежей сборки до установки пакета:

```bash
scp build-vulkan/.../libvulkan_terascale.so shipa@192.168.1.80:/tmp/terakan-deploy/
scp scripts/terascale_icd.deploy.json shipa@192.168.1.80:/tmp/terakan-deploy/terascale_icd.json
```

На целевой машине:

```bash
export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json
export DISPLAY=:0
/tmp/terakan-deploy/terakan-test-compute   # PASS {1,2,3,4}
```

См. также [AGENTS.md](../AGENTS.md) §4–5.
