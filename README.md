# DaiBed

Учебный 3D-прототип voxel/block-based PvP arena game на C++17 и raylib.

Проект не использует чужие ассеты, названия, карты или защищенные элементы других игр.
Игровая цель: защищать командный `EnergyCore`, собирать ресурсы, покупать предметы,
строить блоки, атаковать врага и остаться последней активной командой.

## Сборка на Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
.\build\Release\DaiBed.exe
```

Если raylib не установлен, CMake попробует скачать raylib 5.0 через `FetchContent`.
Для полностью офлайн-сборки установите raylib заранее и оставьте
`DAIBED_USE_SYSTEM_RAYLIB=ON`.

## Управление

- WASD - движение
- Mouse - поворот
- Space - прыжок
- LMB - атака / ломать блок / повреждать чужой EnergyCore
- RMB - поставить блок
- E - открыть/закрыть магазин в зоне базы
- 1-4 - покупки в магазине
- R - debug-respawn на базе
- ESC - выход
