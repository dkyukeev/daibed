# Hero asset pipeline

Для каждого героя рядом хранятся три файла:

- `*.bbmodel` — редактируемый source asset для Blockbench;
- `*.png` — непрозрачная 64×64 копия встроенного UV-атласа для ревью;
- `*.glb` — runtime asset, который один раз загружает raylib.

Структура: `radon`, `orbita`, `brom`, `konvoy`, `likho`, `witness`.

## Экспорт

После изменения модели в Blockbench оставьте основную PNG-текстуру
встроенной в `.bbmodel`, затем из корня репозитория выполните:

```powershell
node scripts/convert_bbmodel_to_glb.mjs assets/heroes/brom/Brom.bbmodel
```

Чтобы пересобрать runtime-файлы всех героев:

```powershell
node scripts/build_hero_assets.mjs
```

Конвертер не нужен игре и не парсит `.bbmodel` в runtime. Он поддерживает
используемый проектом Blockbench `free`-subset: кубы, per-face UV и Euler
rotation. Если модель начнёт использовать кости или другие типы геометрии,
экспортируйте GLB из Blockbench и положите его рядом с `.bbmodel`.

Скрипт `generate_hero_sources.mjs` детерминированно создаёт первоначальные
source-модели Брома, Конвоя, Лихо и Свидетеля. Он перезаписывает эти четыре
`.bbmodel`, поэтому после ручного редактирования в Blockbench запускать его
повторно не следует.

## Runtime и анимации

`HeroVisualLibrary` загружает каждую модель один раз, включает nearest
filter для пиксельного атласа и освобождает `Model`/skeletal animation data
при завершении. Отсутствующий или повреждённый GLB не вызывает падения:
рендер переключается на процедурную фигуру и пишет warning.

Сейчас поддерживаются состояния `Idle`, `Walk`, `Run`, `Jump`, `Fall`,
`Attack`, `Hurt`, `Death`, `Ability1`, `Ability2`, `Ultimate`, а также
существующие специальные фазы Радона и Орбиты. До появления skeletal clips
они реализованы model-level bob/scale/tint, attack swing и существующими VFX.
Ability-пропы в новых `.bbmodel` помечены как non-export: пылесосы, дроны,
капканы, tether и echo создаются runtime-системой как отдельные объекты.

В developer build клавиша `F8` показывает боевой hitbox, visual bounds,
текущее animation state и реально загруженный путь модели.

Экран выбора показывает idle-превью выбранного GLB; модель вращается drag-жестом.

TODO: skeletal clips внутри GLB и ability-preview clips на экране выбора героя.
