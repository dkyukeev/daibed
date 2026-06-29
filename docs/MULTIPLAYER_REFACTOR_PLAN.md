# DaiBed — multiplayer refactor plan

> Цель документа: дать порядок работ, который приводит DaiBed к плавному,
> server-authoritative multiplayer уровня хороших PvP-серверов и одновременно
> упрощает поддержку singleplayer.

## Стратегия

Не переписывать игру с нуля. Делать архитектурный разворот через strangler-подход:

1. новая правильная модель появляется рядом со старой;
2. отдельные gameplay-системы переводятся на command/server/snapshot/event путь;
3. singleplayer постепенно начинает использовать тот же путь через LocalTransport;
4. старые direct paths удаляются только после parity-тестов.

Так мы сохраняем playable состояние проекта почти на каждом шаге.

## Оценка объема

| Уровень | Сессий | Результат |
|---|---:|---|
| Практический first win | 4-6 | Human/bot movement parity, явные роли, меньше резины, меньше различий single/multi. |
| Хороший playable multiplayer | 10-16 | Единый pipeline для movement/combat/shop/inventory/blocks, action results, event feedback. |
| Полная архитектурная цель | 20-35 | Singleplayer как integrated server, старые direct paths удалены, LogicalServer/ClientFrontend разделены. |

## Фаза 0 — зафиксировать цель и защитные тесты

Цель: не начинать большой рефакторинг без quality bar и regression rails.

Работы:

- Документы:
  - `docs/MULTIPLAYER_QUALITY_TARGET.md`;
  - `docs/MULTIPLAYER_TARGET_ARCHITECTURE.md`;
  - этот план.
- Добавить/обновить smoke list для каждого этапа.
- Зафиксировать текущие known debts:
  - speed/action command burst exploit;
  - float validation;
  - reconnect by name / DoS;
  - snapshot caps;
  - packet budgets/rate limits;
  - unstable dynamic ids.

Готово, когда:

- каждый следующий этап имеет acceptance criteria;
- перед изменениями понятно, какие smoke нужно прогонять.

## Фаза 1 — player control roles и movement parity

Цель: убрать gameplay-смысл из `IsLocal()` и сделать human movement одинаковым
в singleplayer, predicted client и authoritative server.

Работы:

- Ввести `PlayerControlKind`.
- Добавить helpers:
  - `IsHumanControlled`;
  - `IsBotControlled`;
  - `IsLocallyPredicted`;
  - `HasLocalCamera`.
- Найти все gameplay-использования `player.IsLocal()`.
- Заменить опасные места:
  - autostep;
  - terrain speed;
  - ice/ground control;
  - bot-only movement assists;
  - local-camera-only branches.
- Добавить parity smoke:
  - один и тот же `PlayerCommand`;
  - local human и remote human authoritative проходят одинаковую траекторию;
  - bot может сохранить bot-only assist, если это явно нужно.

Готово, когда:

- remote human на сервере больше не получает bot-like physics;
- автоподъем, лед и ground control совпадают у human в single/multi;
- `--client-input-smoke`, `--loopback-two-client-smoke`,
  `--network-smoke` проходят.

Оценка: 2-3 сессии.

## Фаза 2 — выделить ServerGameplay boundary

Цель: серверная gameplay-логика перестает зависеть от камеры, UI, audio и
локальности.

Работы:

- Сгруппировать серверные операции за фасадом `ServerGameplay` или похожим
  слоем внутри текущего `Game`.
- Начать с функций, которые уже вызываются из network path:
  - `ApplyPlayerCommand`;
  - `ApplyNetworkPlayerActions`;
  - `ApplyPlayerEconomyCommand`;
  - combat/ranged/place/break entry points.
- Вынести presentation side effects в result/event:
  - `SetMessage`;
  - `audio_.Play*`;
  - `AddFloatingText`;
  - `AddEventMessage`;
  - camera shake.
- Серверные методы возвращают result/event data, а не дергают UI напрямую.

Готово, когда:

- network-controlled player actions не читают host camera/UI;
- server path можно вызвать в headless smoke без presentation side effects;
- existing gameplay smoke остаются зелеными.

Оценка: 2-4 сессии.

## Фаза 3 — action command pipeline для экономики и инвентаря

Цель: shop/inventory/chest/drop работают одинаково в singleplayer и multiplayer.

Работы:

- Разделить:
  - `PlayerCommand` для per-tick input;
  - `PlayerActionCommand` для exactly-once действий.
- Довести `QueueEconomyAction` до общего `QueuePlayerAction`.
- Реализовать server-side actions:
  - `BuyItem`;
  - `DropStack`;
  - `MoveInventorySlot`;
  - `SplitStack`;
  - `ChestDeposit`;
  - `ChestWithdraw`.
- Добавить `PlayerActionResult`:
  - success/fail;
  - localized message id или text key;
  - sound cue;
  - affected slot/resource summary при необходимости.
- UI магазина/инвентаря в multiplayer не мутирует state локально, а отправляет
  action и показывает pending/result.

Готово, когда:

- покупка в singleplayer и multiplayer идет через один server method;
- отказ покупки виден клиенту сразу через result;
- inventory/chest/drop не откатываются непонятным snapshot flicker;
- есть smoke для buy/drop/move/chest actions.

Оценка: 3-5 сессий.

## Фаза 4 — block/combat/ability parity

Цель: основные BedWars-действия не имеют отдельной singleplayer-логики и
отдельной multiplayer-логики.

Работы:

- Blocks:
  - place request;
  - break start/progress/finish;
  - optimistic local preview;
  - authoritative block result;
  - rollback/deny feedback.
- Combat:
  - melee attack command;
  - ranged attack command;
  - hit result event;
  - hurt/knockback/death event;
  - core damage event.
- Abilities/utilities:
  - ability cast command with target/aim data;
  - server validation;
  - cooldown/result event;
  - owner-only/private feedback.

Готово, когда:

- place/break/attack/ranged/ability smoke покрывают local + network path;
- combat feel не строится только по snapshot health delta;
- у клиента есть explicit hit/hurt/core events;
- hero abilities не читают host camera на сервере.

Оценка: 4-7 сессий.

## Фаза 5 — stable replicated entities и event stream

Цель: dynamic entities перестают быть index-based и становятся пригодными для
плавной interpolation/extrapolation.

Работы:

- Добавить stable spawn ids:
  - projectile id;
  - explosive id;
  - hazard id;
  - hero device id;
  - temporary block/effect id при необходимости.
- Snapshot обновляет сущности по id, а не по индексу vector.
- Delta snapshots используют id для add/update/remove.
- Добавить event stream:
  - spawn;
  - impact;
  - despawn;
  - hit;
  - pickup;
  - buy result;
  - block result;
  - death/respawn.
- Разделить reliable important events и lightweight sequenced events.

Готово, когда:

- быстрые стрелы, blaster, fireball, molotov не прыгают между снапшотами;
- impact/death/core/purchase feedback не теряется при обычном jitter;
- reconnect получает baseline state и не воспроизводит старые events как новые.

Оценка: 3-5 сессий.

## Фаза 6 — LocalTransport singleplayer

Цель: singleplayer становится integrated server mode.

Работы:

- Ввести `StartIntegratedServer`.
- Ввести `LocalTransport` pair для одного клиента.
- Запуск singleplayer:
  - создает LogicalServer in-process;
  - подключает ClientFrontend;
  - шлет команды через тот же pipeline;
  - получает snapshots/events.
- Сначала hybrid:
  - migrated systems идут через LocalTransport;
  - unmigrated systems могут временно оставаться direct.
- Затем удалить direct gameplay mutations из singleplayer.

Готово, когда:

- обычный "начать матч с ботами" использует тот же command/server/snapshot/event
  путь, что multiplayer;
- старый `UpdateLocalPlayer -> mutate world` больше не является главным
  gameplay путем;
- работа над shop/block/combat автоматически улучшает оба режима.

Оценка: 5-10 сессий.

## Фаза 7 — очистка и hardening

Цель: после архитектурного разворота закрыть сетевые риски и долговые хвосты.

Работы:

- Command rate limits и packet budgets.
- Float validation/sanitization.
- Snapshot size caps и fragmentation policy.
- Reconnect identity hardening.
- Better bad-network tests:
  - loss;
  - jitter;
  - reorder;
  - duplicate;
  - delayed full snapshot;
  - reconnect during combat.
- Performance profiling:
  - server tick time;
  - snapshot encode/decode time;
  - client snapshot fold time;
  - interpolation buffer cost.

Готово, когда:

- bad network smoke не ломает match state;
- malicious/invalid commands drop safely;
- сервер не тратит unbounded память/CPU на клиента;
- public multiplayer можно тестировать без разработчика рядом.

Оценка: 3-6 сессий.

## Рекомендуемый порядок первых сессий

1. `PlayerControlKind` + movement parity test.
2. Убрать `IsLocal()` из autostep/ice/human movement profile.
3. `ServerGameplay` result boundary для combat/economy без UI/audio.
4. `PlayerActionResult` и buy result pipeline.
5. Drop/move/chest actions.
6. Block place/break action parity.
7. Combat hit/hurt/death/core event stream.
8. Stable projectile/hazard/device ids.
9. LocalTransport skeleton для singleplayer.
10. Перевод singleplayer shop/block/combat на integrated path.

## Обязательные проверки после каждого этапа

- `cmake --build build-release --config Release --parallel 4`
- `--protocol-smoke`
- `--network-smoke`
- `--network-actions-smoke`
- `--network-ranged-smoke`
- `--client-dynamic-apply-smoke`
- `--client-input-smoke`
- `--loopback-two-client-smoke`
- `--mp-loopback-smoke`

Для фаз movement/combat/block дополнительно:

- parity smoke local human vs remote human;
- ручной запуск 2 клиентов;
- проверка ощущения на искусственном ping/loss, когда соответствующий harness
  будет готов.

## Правила для будущих изменений

- Новая gameplay-механика начинается с server-side command/action.
- Если механика имеет player-visible feedback, она добавляет event/result.
- Если сущность живет дольше одного тика, она получает stable id.
- Если клиент показывает optimistic state, должен существовать rollback/deny путь.
- Если singleplayer требует отдельного кода, архитектура еще не дошла до цели.

