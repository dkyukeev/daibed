# DaiBed — multiplayer refactor index

Этот набор документов фиксирует направление большого сетевого рефакторинга:
не латать мультиплеер отдельными ветками, а прийти к единому gameplay pipeline
для singleplayer и multiplayer.

## Документы

- [MULTIPLAYER_QUALITY_TARGET.md](MULTIPLAYER_QUALITY_TARGET.md) — какую
  плавность, отзывчивость и player-visible планку считаем целью.
- [MULTIPLAYER_TARGET_ARCHITECTURE.md](MULTIPLAYER_TARGET_ARCHITECTURE.md) —
  целевая архитектура: `ClientFrontend`, `LogicalServer`, `Transport`,
  `LocalTransport`, event stream, prediction/reconciliation.
- [MULTIPLAYER_REFACTOR_PLAN.md](MULTIPLAYER_REFACTOR_PLAN.md) — фазы работ,
  оценка сессий, acceptance criteria и порядок первых шагов.

## Коротко

Цель:

```text
Singleplayer = ClientFrontend -> LocalTransport -> LogicalServer
Multiplayer = ClientFrontend -> UdpTransport   -> LogicalServer
```

Главное правило: gameplay код не должен расходиться между singleplayer и
multiplayer. Разница только в транспорте, задержке и presentation-адаптации.

Первый практический этап:

1. Ввести `PlayerControlKind`.
2. Убрать gameplay-смысл из `IsLocal()`.
3. Сделать human movement parity для singleplayer/predicted/server human.
4. Добавить parity smoke.
5. Перейти к action results для shop/inventory/block/combat.

## Связанные документы

- [NETWORK_PREP_PLAN.md](NETWORK_PREP_PLAN.md) — история и текущее состояние
  уже сделанного network-prep слоя.
- [ROADMAP.md](ROADMAP.md) — общий план проекта до релиза.
- [VISION.md](VISION.md) — продуктовая цель DaiBed.

