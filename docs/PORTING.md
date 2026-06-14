# Порт на `Terakan_state_rework` (2026-06)

Сводка переноса форка с legacy-базы **`Terakan_Backup_2026-06-10_2_Meta_MSAA`** на upstream **`Terakan_state_rework`**.

## Статус: порт патчей завершён

| Этап | Статус |
|------|--------|
| База upstream | ✅ `Terakan_state_rework` (`a5fc39658` и новее) |
| Патчи 0001–0003 (build + API 1.1) | ✅ без изменений по смыслу |
| Патчи 0004–0009 → один **0004** | ✅ слиты под новую архитектуру state (`hw_config_*`, `app_config_draw`, `terakan_meta_impl.h`) |
| Compute MVP → **0010** | ✅ PASS на Palm: `terakan-test-compute` → `{1,2,3,4}` |
| `scripts/apply-patches.sh` | ✅ 0001, 0002, 0003, 0004, 0010 |
| Сборка Vulkan ICD (dev) | ✅ `meson compile -C build-vulkan` (нужен `PKG_CONFIG_PATH`, см. [BUILD.md](BUILD.md)) |
| Smoke на Palm (192.168.1.80) | ✅ `vulkaninfo`, `vkcube`, `vkgears` с `/tmp/terakan-deploy/` |
| Игры (vkQuake3, STK) | ⚠️ Vulkan OK; **multi-binding vertex fetch** — текстуры размазаны |
| Arch `makepkg` | ⚠️ PKGBUILD обновлён; полная сборка пакета — отдельный шаг (см. [PACKAGING.md](PACKAGING.md)) |

## Что изменилось в upstream

Ветка **`Terakan_state_rework`** переработала управление состоянием:

- `terakan_hw_config_draw` / `terakan_hw_config_shared` / `terakan_hw_config_sqk`
- `terakan_app_config_draw` / `terakan_app_config_compute`
- meta через `terakan_meta_impl.h` и отдельные blit/clear модули

Старые патчи **0005–0009** **не применяются по отдельности** — их diff переписан в **`0004-implement-cmd-blit-image2.patch`**.

Файлы **0005–0009** в `patches/` **удалены** намеренно.

## Текущая схема патчей

| Patch | Содержимое |
|-------|------------|
| **0001** | C23 `once_flag` / `call_once` (glibc 2.42+) |
| **0002** | pthread casts в `cnd_monotonic.c` (GCC 16) |
| **0003** | Vulkan API 1.1 в `terakan_instance.h` |
| **0004** | Blit meta (scaled + STK stability), batching, draw indirect, `VK_EXT_descriptor_indexing` — бывшие 0004–0009 |
| **0010** | Compute MVP + barriers, SFN kcache redirect, stride/VI fixes, CB UAV guard, push constants layout |

Патч **0010** накладывается **поверх 0004**. В upstream уже есть записи compute-файлов в `meson.build`; 0010 добавляет исходники и логику.

### Файлы патча 0010

| Файл | Назначение |
|------|------------|
| `terakan_vk_pipeline_compute.c/h` | `CreateComputePipelines`, bind CS |
| `terakan_vk_dispatch.c` | `CmdDispatch`, `CmdDispatchIndirect` + barriers |
| `terakan_hw_config_compute.c/h` | CS register packets |
| `terakan_app_config_compute.c/h` | app-side CS state, UAV sync |
| `terakan_command_buffer.c/h` | compute writer reset/init |

Изменены также: `terakan_app_config_draw.c`, `terakan_hw_config_draw.c`, `terakan_shader.c`, `terakan_pipeline_layout.c`, `terakan_vk_pipeline_graphics.c`, `terakan_vk_state.c`, `terakan_vk_draw.c`, `terakan_push_constants.h`, `sfn_shader.cpp`, `r600_pipe.h`, `terakan_physical_device.c`.

## Рабочая копия Mesa

Рекомендуемый путь для правок и сборки:

```
/home/shipa/terakan-mesa-state-rework
```

После правок — `git diff > patches/0010-compute-mvp.patch` (или соответствующий патч). См. [AGENTS.md](../AGENTS.md) §3.

## Проверка после клона

```bash
git clone -b Terakan_state_rework --single-branch \
  https://gitlab.freedesktop.org/Triang3l/mesa.git /home/shipa/terakan-mesa-state-rework

./scripts/apply-patches.sh /home/shipa/terakan-mesa-state-rework

cd /home/shipa/terakan-mesa-state-rework
PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/share/pkgconfig \
meson setup build-vulkan ... && meson compile -C build-vulkan
```

Ожидаемый артефакт: `build-vulkan/src/amd/terascale/vulkan/libvulkan_terascale.so`.

## Деплой на целевую машину (Palm)

```bash
SO=/home/shipa/terakan-mesa-state-rework/build-vulkan/src/amd/terascale/vulkan/libvulkan_terascale.so
sshpass -p '1' scp "$SO" shipa@192.168.1.80:/tmp/terakan-deploy/libvulkan_terascale.so
sshpass -p '1' scp scripts/terascale_icd.deploy.json shipa@192.168.1.80:/tmp/terakan-deploy/terascale_icd.json

# на Palm
export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json
export DISPLAY=:0
vulkaninfo --summary
vkcube
vkgears
/tmp/terakan-deploy/terakan-test-compute   # PASS
```

Системный пакет `vulkan-terakan` на Palm может быть **старой** ревизии — для проверки WIP используйте `/tmp/terakan-deploy/`.

## Что осталось (не часть «порта ветки»)

- **P0:** multi-binding vertex fetch (vkQuake3, STK 3D)
- **Arch package:** `makepkg -sf` после синхронизации hardlink-патчей в `packaging/archlinux/`
- R8xx scaled blit hang (опционально `TERAKAN_ENABLE_R8XX_SCALED_BLIT=1`)
- `CmdDrawIndirectCount`, maintenance3, pipeline cache, MSAA resolve

См. [ROADMAP.md](ROADMAP.md) и [VULKAN_STATUS.md](VULKAN_STATUS.md).
