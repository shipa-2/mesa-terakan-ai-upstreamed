# AGENTS.md — mesa-terakan-mimo-development

## 1. Что это за репозиторий

**Путь:** `/home/shipa/Projects/mesa-terakan-mimo-development`

Это не полный Mesa, а очередь патчей + скрипты + упаковка Arch поверх upstream Terakan.

| Что здесь | Что здесь нет |
|-----------|---------------|
| `patches/0001…0010` | Исходников Mesa (клонируются отдельно) |
| `scripts/apply-patches.sh` | Собранного .so (собирается локально) |
| `scripts/terakan-test-*.c` | |
| `packaging/archlinux/` | |
| `docs/` | |

**Upstream Mesa (read-only reference):**
https://gitlab.freedesktop.org/Triang3l/mesa.git, ветка `Terakan_state_rework` (commit `a5fc39658`).

**Другой репозиторий (Cursor/AI):**
`/home/shipa/Projects/mesa-terakan-ai-development` — та же схема патчей; синхронизируйте `patches/` между ними после merge.

**Рабочая копия Mesa:**
`/home/shipa/terakan-mesa-state-rework` — здесь правится исходный код напрямую. После правок — перегенерировать патчи.

## 2. Архитектура

### Слой | Назначение
- `app_config_draw` / `app_config_compute` — логическое состояние приложения
- `hw_config_draw` / `hw_config_compute` / `hw_config_sqk` — регистры → IB
- `meta/` — внутренние pass'ы: copy, blit, clear, query
- `vk_common` — ~245 `Cmd*` только state; GPU flush на draw

### Структура патчей

| Patch | Содержимое |
|-------|-----------|
| 0001–0003 | glibc 2.42 / GCC 16 / API 1.1 |
| 0004 | Blit, draw indirect, VK_EXT_descriptor_indexing, STK stability (бывшие 0005–0009) |
| 0010 | Compute MVP: barriers, SFN kcache redirect, stride fix, dynamic VI stride propagation |

0010 накладывается поверх 0004. Патчи 0005–0009 не существуют — не создавать.

### Ключевые файлы (модифицированы)

| Файл | Что изменено |
|------|-------------|
| `terakan_vk_dispatch.c` | `CmdDispatch`, `CmdDispatchIndirect` + barriers |
| `terakan_vk_pipeline_compute.c/h` | `CreateComputePipelines`, bind CS |
| `terakan_hw_config_compute.c/h` | CS register packets |
| `terakan_app_config_compute.c/h` | app-side CS state, UAV sync |
| `terakan_app_config_draw.c` | CB UAV guard for compute, `apply_sq_resources_fetch` unbind fix |
| `terakan_hw_config_draw.c` | `TERAKAN_PACKET3_COMPUTE` flag for context regs |
| `terakan_command_buffer.c/h` | compute writer reset/init |
| `terakan_shader.c` | `nir_lower_compute_system_values` |
| `terakan_pipeline_layout.c` | `remaining_stages` infinite loop fix |
| `terakan_push_constants.h` | `base_vertex`/`base_instance` in driver constants |
| `sfn_shader.cpp` | SFN kcache redirect: `R600_LDS_INFO_CONST_BUFFER` 16→15, hardcoded offsets 48/52/56 |
| `r600_pipe.h` | `R600_LDS_INFO_CONST_BUFFER` = `R600_MAX_USER_CONST_BUFFERS` |
| `terakan_vk_draw.c` | driver constants population, draw indirect |
| `terakan_vk_pipeline_graphics.c` | stride init always from pipeline |
| `terakan_vk_state.c` | dynamic VI stride propagation in `apply_vi` |
| `terakan_physical_device.c` | multiDrawIndirect, descriptor_indexing, shaderDrawParameters features |

## 3. Рабочий цикл

### 3.1 Клон Mesa (один раз)

```bash
git clone --branch Terakan_state_rework --single-branch \
  https://gitlab.freedesktop.org/Triang3l/mesa.git /home/shipa/terakan-mesa-state-rework
```

### 3.2 Сборка

```bash
cd /home/shipa/terakan-mesa-state-rework
rm -rf build-vulkan
PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/share/pkgconfig \
meson setup build-vulkan \
  --prefix=/usr --libdir=lib --buildtype=release \
  -Dvulkan-drivers=amd_terascale \
  -Dgallium-drivers= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dopengl=false -Dllvm=disabled \
  -Dplatforms=x11,wayland \
  -Dc_args='-Wno-incompatible-pointer-types'
meson compile -C build-vulkan
```

Артефакт: `build-vulkan/src/amd/terascale/vulkan/libvulkan_terascale.so` (~10 MB)

**Важно:** `PKG_CONFIG_PATH` обязателен — без него meson находит 32-bit библиотеки из `/usr/lib32/`.

### 3.3 Как править код

Правки в `/home/shipa/terakan-mesa-state-rework/...` (удобно для сборки).
После правок — перегенерировать патч и проверить чистое применение.

### 3.4 Как править патчи

```
cd /home/shipa/terakan-mesa-state-rework
git diff > /home/shipa/Projects/mesa-terakan-mimo-development/patches/0010-compute-mvp.patch
```
Затем проверить чистое применение (§3.2) + сборку.

## 4. Целевая машина (Palm)

| Параметр | Значение |
|----------|----------|
| IP | 192.168.1.80 |
| user | shipa |
| password | 1 |
| GPU | AMD R8xx Palm (Terakan) |
| OS | Arch Linux |

### Деплой

```bash
SO=/home/shipa/terakan-mesa-state-rework/build-vulkan/src/amd/terascale/vulkan/libvulkan_terascale.so
sshpass -p '1' ssh shipa@192.168.1.80 'mkdir -p /tmp/terakan-deploy'
sshpass -p '1' scp "$SO" shipa@192.168.1.80:/tmp/terakan-deploy/libvulkan_terascale.so
```

Для системной установки:
```bash
sshpass -p '1' ssh -t shipa@192.168.1.80 \
  'echo "1" | sudo -S cp /tmp/terakan-deploy/libvulkan_terascale.so /usr/lib/libvulkan_terascale.so'
```

### Запуск на Palm

```bash
export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json
export DISPLAY=:0
```

## 5. Тесты

### 5.1 Compute (главный тест Phase 1) — ✅ PASS

```bash
sshpass -p '1' ssh shipa@192.168.1.80 \
  'export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json && /tmp/terakan-deploy/terakan-test-compute'
```

Ожидаемо: `data[0..3] = {1,2,3,4}` → PASS.

### 5.2 Smoke (графика)

```bash
export VK_ICD_FILENAMES=/tmp/terakan-deploy/terascale_icd.json
vkcube        # крутится без crash ✅
vkgears       # 3D геометрия ✅
```

### 5.3 vkQuake3

```bash
sshpass -p '1' ssh shipa@192.168.1.80 \
  'export DISPLAY=:0 && /usr/bin/terakan-vulkan-setup \
   /home/shipa/Desktop/vkquake3/ioquake3.x86_64 \
   +set cl_renderer vulkan +set r_fullscreen 0 +devmap q3dm1'
```

Статус: Vulkan инициализируется, pipelines создаются, но **текстуры размазаны**. Корневая причина: vertex fetch stride/multi-binding issue (расследуется).

### 5.4 SuperTuxKart

```bash
sshpass -p '1' ssh shipa@192.168.1.80 \
  'export DISPLAY=:0 && /usr/bin/terakan-vulkan-setup \
   /home/shipa/Desktop/vkquake3/../stk/supertuxkart \
   --render-driver=vulkan --no-start-screen'
```

Статус: Vulkan рендерер, 4 bindings (position/16, color/4, texcoord/8, lightmap/8).
**Текстуры размазаны** — та же проблема multi-binding vertex fetch.

## 6. Исправлено в этой сессии

### Compute MVP — ✅ PASS
1. **CB UAV guard** — `apply_sq_pgm_fragment` обнулял `uav_used` при compute. Fix: `if (app_config_compute.shader == NULL)`.
2. **CB UAV resource path** — compute использует `set_resource_cs` вместо `set_resource_fs` + `UAV_IMMEDIATE_BASE_COMPUTE`.
3. **Barriers** — CB UAV flush + CS partial flush до dispatch, TC/VC/SH invalidation после.
4. **SFN kcache redirect** — `R600_LDS_INFO_CONST_BUFFER` 16→15, hardcoded offsets 48/52/56 для draw_id/base_vertex/base_instance.
5. **Push constants driver layout** — добавлены `base_vertex`/`base_instance` в `terakan_push_constants_driver`.
6. **Stride init** — всегда из pipeline даже при `MESA_VK_DYNAMIC_VI`.
7. **`remaining_stages` fix** — `terakan_pipeline_layout.c:328`.
8. **Dynamic VI stride propagation** — `terakan_vk_state_dynamic_apply_vi` теперь устанавливает strides из bindings.
9. **`apply_sq_resources_fetch` unbind fix** — ограничен `unbound_resources` до `< MUTABLE_BASE`.

### Что ещё не исправлено (текущая задача)
- **3D текстуры размазаны** (vkQuake3 + STK) — vertex fetch multi-binding issue
- Корневая причина: stride в `apply_sq_resources_fetch` правильный (16/4/8/8), ресурсы привязаны → проблема в другом месте pipeline

## 7. Текущие задачи

### P0 — Vertex fetch multi-binding (текущая)
- vkQuake3: текстуры стен размазаны как цветные полосы
- STK: колёса в одной точке, текстуры разорваны
- Debug показывает stride правильный (16/4/8/8), res_used=0x7/0xf
- Следующий шаг: расследовать VTX fetch инструкции, формат атрибутов, sampler state

### P1 — будущее
- `CmdDrawIndirectCount`
- MSAA resolve в `CmdEndRendering`
- Pipeline cache UUID
- `CmdClearDepthStencilImage`
- `VK_KHR_maintenance3`
- R8xx scaled blit hang

## 8. Документация в репо

| Файл | Содержание |
|------|-----------|
| `AGENTS.md` | Agent workflow, Palm deploy, tests, current P0 |
| `docs/BUILD.md` | Сборка |
| `docs/PORTING.md` | Схема патчей, deploy |
| `docs/VULKAN_STATUS.md` | Матрица API |
| `docs/ROADMAP.md` | Backlog |
| `docs/PACKAGING.md` | Arch Linux пакет |

## 9. Частые ошибки

- Править только `$MESA` без обновления `patches/` — изменения потеряются при reset.
- Тест с системным ICD вместо `/tmp/terakan-deploy/` — кажется, что патч не работает.
- 0010 генерировать от чистого upstream без 0004 — patch stack ломается.
- `PKG_CONFIG_PATH` не установлен → meson находит 32-bit библиотеки.

## 10. Target hardware

AMD TeraScale: R600, RV770, Cedar, Redwood, **Palm** (R8xx), Sumo, Cayman (pre-GCN).
- **18 samplers per stage** hardware limit
- **32-bit buffer VA** in IB
- SQ CF ALU kcache bank is 4-bit (0–15 hard limit)
