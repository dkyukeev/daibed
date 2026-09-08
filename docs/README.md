# Документация DaiBed

В этой папке хранятся только действующие продуктовые ориентиры, архитектурные
контракты, QA-матрицы и документация сложных подсистем. История завершённых
этапов доступна через Git и не дублируется отдельными отчётами в `docs/`.

## Продукт и текущая работа

- [VISION.md](VISION.md) — основная продуктовая формула и неизменяемые принципы.
- [GAMEPLAY_PRIORITY_BACKLOG.md](GAMEPLAY_PRIORITY_BACKLOG.md) — приоритеты
  игрового цикла, баланса и качества.
- [GAMEPLAY_TEST_MATRIX.md](GAMEPLAY_TEST_MATRIX.md) — обязательная матрица
  ручных, сетевых и automatch-проверок.
- [VISUAL_REWORK_CODEX_PROMPT.md](VISUAL_REWORK_CODEX_PROMPT.md) — рабочий
  промт комплексной визуальной стандартизации.

## Multiplayer

- [MULTIPLAYER_QUALITY_TARGET.md](MULTIPLAYER_QUALITY_TARGET.md) — целевое
  ощущение сети и измеримые требования качества.
- [MULTIPLAYER_TARGET_ARCHITECTURE.md](MULTIPLAYER_TARGET_ARCHITECTURE.md) —
  границы client, server, simulation, transport и presentation.
- [MULTIPLAYER_REFACTOR_PLAN.md](MULTIPLAYER_REFACTOR_PLAN.md) — оставшаяся
  миграция к единому gameplay pipeline.
- [P2P_IMPLEMENTATION.md](P2P_IMPLEMENTATION.md) — текущее устройство UDP,
  Steam P2P, лобби, reconnect и универсального релиза.

## Карты, боты и навигация

- [CASTLE_BEDWARS.md](CASTLE_BEDWARS.md) — устройство и проверка Castle.
- [BOT_PARKOUR.md](BOT_PARKOUR.md) — parkour-карта и тренировочный curriculum.
- [FAST_NAVIGATION_KERNEL.md](FAST_NAVIGATION_KERNEL.md) — быстрый поисковый
  контур навигации.
- [NAVIGATION_ACTION_LIBRARY.md](NAVIGATION_ACTION_LIBRARY.md) — библиотека
  навигационных действий и телеметрия.
- [CORRIDOR_SEGMENT_REPAIR.md](CORRIDOR_SEGMENT_REPAIR.md) — локальный ремонт
  маршрута в коридоре.
- [DIRTY_NAVIGATION_FALL_PARKOUR.md](DIRTY_NAVIGATION_FALL_PARKOUR.md) — dirty
  regions, падения и калибровка прыжков.
- [MOMENTUM_TRANSITIONS_EDGE_BRAKING.md](MOMENTUM_TRANSITIONS_EDGE_BRAKING.md) —
  переходы с учётом импульса и торможение у края.
- [STUCK_BRIDGE_ACTION_RECOVERY.md](STUCK_BRIDGE_ACTION_RECOVERY.md) — выход из
  застревания и восстановление строительства мостов.

## Правила поддержки

- Не создавать отдельный документ-отчёт для каждого завершённого шага.
- Обновлять существующий контракт системы либо описание текущего состояния.
- Удалять исполненные промты и планы, если они больше не являются спецификацией.
- Не хранить в `docs/` автоматически созданные логи, метрики и скриншоты.
- При удалении документа сначала переводить ссылки кода на актуальную замену.
