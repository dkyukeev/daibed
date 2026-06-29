# DaiBed — target multiplayer architecture

> Цель документа: описать архитектуру, к которой должен прийти проект, чтобы
> singleplayer и multiplayer развивались вместе, а сетевое ощущение было
> стабильным, плавным и поддерживаемым.

## Главная идея

Singleplayer и multiplayer должны отличаться транспортом, а не gameplay-путем.

```text
Singleplayer:
  ClientFrontend -> LocalTransport -> LogicalServer

Multiplayer:
  ClientFrontend -> UdpTransport   -> LogicalServer

Loopback/tests:
  ClientFrontend -> LoopbackTransport -> LogicalServer
```

Игрок нажал кнопку "купить", "ударить", "поставить блок" или "использовать
способность" — клиент всегда создает команду. В singleplayer команда уходит в
локальный сервер в том же процессе. В multiplayer команда уходит в удаленный
сервер. Сервер применяет одну и ту же gameplay-логику и реплицирует результат.

## Целевая схема

```text
GameApp
  ├─ ClientFrontend
  │   ├─ InputMapper
  │   ├─ ClientCommandQueue
  │   ├─ ClientPrediction
  │   ├─ Reconciliation
  │   ├─ RemoteInterpolation
  │   ├─ ClientFeedback
  │   ├─ Camera
  │   ├─ UI
  │   └─ Renderer
  │
  ├─ Transport
  │   ├─ LocalTransport
  │   ├─ LoopbackTransport
  │   └─ UdpTransport
  │
  └─ LogicalServer
      ├─ ServerSession
      ├─ MatchSimulation
      ├─ ServerGameplay
      ├─ WorldMutation
      ├─ Combat
      ├─ InventoryService
      ├─ ShopService
      ├─ HeroService
      ├─ BotController
      ├─ Replication
      └─ EventStream
```

`Game` может временно оставаться фасадом, но его роли должны постепенно
разъезжаться по этим компонентам.

## Ответственность слоев

### ClientFrontend

Читает устройства ввода, ведет камеру/UI/рендер, предсказывает локального игрока,
интерполирует удаленных, показывает feedback.

ClientFrontend может:

- строить `PlayerCommand` и `PlayerActionCommand`;
- применять prediction к своему игроку;
- временно показывать predicted block/place/break feedback;
- проигрывать звук, hitmarker, floating text, camera shake;
- хранить interpolation buffers;
- применять authoritative snapshot к render-state.

ClientFrontend не может:

- наносить настоящий урон;
- менять серверный inventory/economy;
- окончательно ставить или ломать блок;
- решать победу, смерть, respawn, cooldown или pickup.

### LogicalServer

Единственный authority для gameplay. Работает без raylib window/render/audio/UI.

LogicalServer делает:

- принимает команды клиентов;
- валидирует ownership, rate limit, sequence, cooldown и proximity;
- применяет movement/combat/world/economy/heroes;
- тикает `MatchSimulation` фиксированным dt;
- создает snapshots, deltas и event stream;
- держит per-client ack/baseline/reconnect state.

### Transport

Транспорт доставляет команды, snapshots, events и lobby state. Он не знает правил
магазина, урона или движения.

Минимальные реализации:

- `LocalTransport`: in-process channel без packet loss, используется для
  singleplayer/integrated server.
- `LoopbackTransport`: multi-client in-process, используется для smoke/integration.
- `UdpTransport`: реальная сеть с unreliable/reliable каналами, delta snapshots,
  resync и reconnect.

## Player control roles

`Player::IsLocal()` не должен решать gameplay. Нужна явная роль управления.

```cpp
enum class PlayerControlKind
{
    LocalHumanPredicted,
    RemoteHumanAuthoritative,
    BotAuthoritative,
    Replica,
    Spectator
};
```

Базовые helpers:

```cpp
bool IsHumanControlled(PlayerControlKind kind);
bool IsBotControlled(PlayerControlKind kind);
bool IsLocallyPredicted(PlayerControlKind kind);
bool HasLocalCamera(PlayerControlKind kind);
```

Правило:

- human movement profile одинаков для local human и remote human на сервере;
- bot-only assist, autostep или tuning зависят от `IsBotControlled`, а не от
  `!player.IsLocal()`;
- local camera/UI зависят от `HasLocalCamera`;
- prediction зависит от `IsLocallyPredicted`.

## Command model

Нужно разделить per-tick input и discrete actions.

### PlayerCommand

Per-tick намерения:

- movement axes;
- jump/sprint/sneak/bridge;
- aim yaw/pitch;
- selected slot;
- attack held/pressed/released;
- place held/pressed;
- quick utility flags.

### PlayerActionCommand

Discrete exactly-once действия:

- buy item;
- drop stack;
- move inventory slot;
- split stack;
- chest deposit/withdraw;
- ability cast with target;
- interact;
- respawn/spectator actions.

У каждого action есть `actionSeq`, owner client/player и server result.

```cpp
struct PlayerActionResult
{
    std::uint32_t actionSeq;
    PlayerActionType type;
    bool success;
    int messageId;
    int soundCue;
};
```

Action result нужен, чтобы клиент показывал "куплено", "нельзя купить",
"нет ресурсов", "сундук далеко" без гадания по снапшоту.

## Replication model

Сеть должна передавать два потока:

1. **State snapshots** — "что сейчас правда".
2. **Gameplay events** — "что произошло и как это почувствовать".

Snapshot нужен для resync, interpolation и позднего join/reconnect.
Event stream нужен для ощущения игры.

### Snapshot содержит

- tick/time/phase/winner;
- players: public state + owner-private inventory для владельца;
- cores/generators/pickups/dropped items;
- block deltas или block state changes;
- projectiles/explosives/hazards/devices/status effects;
- per-client `lastProcessedCommandTick`.

### Event stream содержит

- hit/hurt/kill/final death;
- core hit/core destroyed;
- projectile spawned/impacted;
- block placed/broken/denied;
- pickup collected;
- buy/drop/inventory/chest result;
- ability cast/denied/expired;
- respawn/reconnect/team/lobby events.

Events могут быть small reliable или sequenced unreliable в зависимости от типа.
Важные result/death/core events должны быть reliable или восстановимы из snapshot.

## Prediction and reconciliation

Own player:

- клиент применяет локальную команду сразу;
- команда получает monotonic client command tick;
- сервер echo-ит per-client `lastProcessedCommandTick`;
- клиент удаляет подтвержденную историю и переигрывает неподтвержденные команды;
- большая ошибка snap/correct, малая ошибка smooth correction.

Remote players:

- не симулируются полностью клиентом;
- хранят snapshot buffer;
- позиция/yaw берутся из interpolation target time;
- при packet loss interpolation delay адаптируется;
- при teleport/respawn/death используется snap, а не длинная интерполяция.

Dynamic entities:

- каждый spawn получает stable `spawnId`;
- snapshot обновляет состояние по id;
- client visual state может двигаться между снапшотами;
- impact/despawn приходит event'ом или устойчиво выводится из state.

## Integrated singleplayer

Целевое поведение:

```text
StartSingleplayer()
  create LogicalServer in-process
  create LocalTransport pair
  create ClientFrontend
  connect client to local server
  run same command/snapshot/event loop
```

Пока миграция не завершена, допускается hybrid-режим:

- старый singleplayer path остается рабочим;
- новые gameplay systems сначала переводятся на command/server path;
- когда достаточно систем перенесено, singleplayer switch становится
  LocalTransport-only.

## Инварианты архитектуры

- Серверный код не включает UI/audio/render/window.
- Клиентский presentation не мутирует authoritative gameplay.
- `IsLocal()` не используется для gameplay-физики или правил.
- Любая новая механика обязана иметь command/action путь и replication/feedback.
- Snapshot не заменяет action result/event feedback.
- Tests должны иметь LocalTransport/Loopback путь без GUI.

## Не делаем сейчас

- Полный rewrite renderer.
- MMO-архитектуру.
- Rollback fighting-game style для всего мира.
- Перенос всего проекта в отдельный server process до появления чистого
  `LogicalServer`.
- Удаление старого singleplayer path одним большим коммитом.

