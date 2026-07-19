# DaiBed — Network preparation plan

> Производное от `docs/ROADMAP.md` (Фаза 0/2) и `docs/VISION.md`. Это карта кода
> для следующего агента, который продолжит путь к authoritative-серверу. Цель
> прохода: заложить безопасный фундамент (типы, сессия-мок, snapshot, CLI),
> **не** делая полноценный сетевой мультиплеер и не ломая single-player/automatch.

Целевая модель: `Client input -> Server authoritative simulation -> Replicated
state -> Client rendering/prediction`. Реализованы: client-команды и
server-snapshot как типы, локальный мок-сессия, headless диагностика, бинарный
wire-протокол, **реальный UDP-транспорт**, **первый closed multiplayer**
(Phase 0.1S: `--server` authoritative-loop + `--connect` клиенты по IP/port/password,
два процесса видят один матч), **GUI-рендер клиента** (Phase 0.1T) и **играбельный
сетевой клиент** (Phase 0.1U: `--connect` управляет своим героем по сети — ввод →
команда → сервер, позиция authoritative из снапшота, лаг = RTT). Осталось:
prediction/reconciliation, interpolation (чтобы убрать RTT-лаг), reliability/ack,
delta-compression.

---

## 1. Инвентаризация: где что живёт сейчас

`Game` — один класс (`src/Game.h`), определения размазаны по многим TU (см.
память `game-cpp-decomposition`). Симуляция всё ещё перемешана с презентацией
внутри `Game`, но уже сгруппирована по методам.

| Что | Где | Заметки |
|---|---|---|
| **Состояние симуляции** | `Game.h` — `world_`, `teams_`, `players_` (storage), projectiles/devices/status-векторы (всё ещё у `Game`). **tick + match clock + `generators_` + `pickups_` + `droppedItems_` + `cores_` + winner + match phase + block-delta buffer** уже в `matchSimulation_`; **доступ к players** — через `matchSimulation_.Players()`/`GetPlayer()` (6A; storage пока в `Game`). `Generator`/`ResourcePickup`/`DroppedItem`/`EnergyCore` — raylib-free; `Player` хранит pos/vel/spawn как `Vec3` (сигнатуры/`World`/`Hero` ещё на raylib). | Остаток (6B): raylib-free `Player` → storage `players_` в `MatchSimulation`; `teams_` (Vec3 в `Team`); `world_`. |
| **Чтение ввода** | `InputSystem::Poll()` → `currentInput_` в `Game::HandleInput()` (`Game.cpp:309`) | `PlayerInput` (`InputSystem.h:56`) — «сырой» кадровый ввод (raylib key/mouse). Меню/пауза обнуляют `currentInput_`. |
| **Применение действий игрока** | `Game::UpdateLocalPlayer()` (`Game.cpp:1344`), `UseUtilityInputs`/`UseHeroAbilityInputs` (`GamePlayerActions.cpp`), `HandlePlaceBlock`/`UpdateAttackOrBreak` (`GamePlacement.cpp`) | Локальный gameplay-mutation path command-driven (0.1J); `currentInput_` остался в input/UI/camera/menu + `BuildLocalPlayerCommand`. |
| **Тик ботов** | `Game::UpdateBots` → `UpdateSingleBot` (`GameBotAI.cpp`); utility-скоринг, роли, интенты; тюнинг `BotTuningGenome` | Movement/aim ботов уже через `PlayerCommand`/`ApplyPlayerCommand` (0.1K), hero ability casts через `ApplyPlayerActionCommand`; utility/combat/place/core assault ещё прямые. |
| **Тик героев/способностей** | `UpdateHeroPassives`, `UpdateHeroTemporaryBlocks`, `UpdateBromDevices`, `UpdateKonvoyDevices`, `UpdateLikhoBleeds`, `UpdateSvidetelEffects` (`GameWorldTick.cpp`); касты — `src/Hero/HeroAbilities.cpp` | Data-driven `HeroSystem`; состояние per-player в `HeroRuntimeState` (`Hero.h:83`). |
| **Projectiles / devices / status** | `UpdateProjectiles`, `UpdateExplosives`, `UpdateHazardZones`, `UpdateBlockHazards`, `UpdateDroppedItems`, `UpdatePickups` (`GameWorldTick.cpp`); векторы в `Game.h:596-614` | Реплицируются в snapshot (Phase 0.1M: projectiles/explosives/hazardZones/heroDevices/statusEffects, public state + owner/team + `visibility`). Storage всё ещё у `Game` (raylib `Vector3`); конвертация `Vec3` на границе `BuildNetworkSnapshot`. |
| **Правила матча** | `GameRules::CheckWinCondition` (`Game.cpp:730`), `TriggerCoreCollapse`, sudden death | Чистая логика, легко переносится в server. |
| **Главный тик** | `Game::UpdateMatchSimulation(float dt)` (`Game.cpp:670`) | Один шаг симуляции. Headless-ветка уже не зовёт рендер/камеру/звуковой listener (`if (!headless_)`). |
| **Headless-прогон** | `RunAutomatchBatch` / `StartAutomatch` / `UpdateAutomatch` (`GameAutomatch.cpp`) | Доказывает, что симуляция живёт без окна — основа для authoritative-сервера. |

### Что уже годится для server-authoritative / headless

- `UpdateMatchSimulation` + automatch-цикл — симуляция шагает без raylib-окна.
- `GameRules` — чистые правила победы.
- Геттеры `Player`/`EnergyCore`/`Team` — публичны и raylib-независимы по
  смыслу (кроме `Vector3`), пригодны для извлечения snapshot’а.

### Что выносить позже

- **`MatchSimulation`** — отдельный владелец `world_/players_/teams_/cores_/…`
  и шага тика, без raylib (Фаза 0.1). `Game` станет клиентом.
- ~~**Fixed tick**~~ — **сделано (0.1O)**: симуляция тикает фиксированным
  `FixedDeltaSeconds()` (60 Гц) через accumulator, render отдельно. Осталась
  только render-interpolation между тиками.
- **Боты → полный `PlayerCommand`** — movement/aim уже есть; осталось перенести
  utility/combat/place/core assault и, если понадобится, bot slot/action-target state.
- **Полный input pipeline** — локальный gameplay уже command-driven; остался
  input/UI слой, включая place-click trigger в `HandleInput`.
- **raylib `Vector3` в симуляции** — заменить своим вектором при выделении
  `MatchSimulation` (snapshot/command уже raylib-free и используют плоские float).

---

## 2. Новые сетевые типы (raylib-free)

Каталог `src/Network/` уже существовал (низкоуровневый мок сообщений). Добавлены
типы более высокого уровня; дубликаты не плодились — низкоуровневый
`NetworkMessage`/`Client`/`Server`/`LocalNetworkMock` сохранён как заготовка
байтового транспорта.

| Файл | Содержит |
|---|---|
| `src/Network/NetTypes.h` | `enum class NetworkMode { LocalSinglePlayer, LocalHost, LocalClient, DedicatedServer }`; `ServerConfig { listenAddress, port, maxPlayers, privateServer, serverName, password }` с `HasPassword()`/`ValidatePassword()` (без криптографии — только хранение/валидация). |
| `src/Network/PlayerCommand.h` | `PlayerCommand` — все действия за тик: `controlledPlayerId`/`tick`, movement (`moveForward/moveStrafe/aimYaw/aimPitch/jump/sprint/sprintTapped/sneak`), `selectedSlot`, attack (`attackPressed/Held/Released`), place (`placePressed/placeHeld/scopeHeld`), `interact`, `useAbility1/2/Ultimate`, utility (`useHeal/Teleport/Dash/Shoot/Fireball/Molotov/Alarm`). |
| `src/Network/NetworkSnapshot.h` | `MatchPhase`; `PlayerSnapshot` (id/team/hero/**`Vec3` pos/vel**/health/alive/eliminated/slot); `CoreSnapshot`; `GeneratorSnapshot`; `PickupSnapshot` (resourceType/amount/`Vec3`); `DroppedItemSnapshot` (itemType/count/`Vec3`); `MatchSnapshot` (tick/matchTime/phase/winner + players/cores/generators/pickups/droppedItems; место под visibility filtering). |
| `src/Network/LocalServerSession.{h,cpp}` | In-process transport/snapshot-канал: `SubmitCommand` → `DrainCommands` → `PublishSnapshot`/`LatestSnapshot` (tick живёт в `MatchSimulation`, не здесь). Реальный транспорт подменит класс, сохранив форму command-in / snapshot-out. |
| `src/Simulation/SimMath.h` | Raylib-free `Vec3 { float x,y,z }` (+ `Length`/операторы). Vector-тип симуляции/replication. Используется в `NetworkSnapshot`, `Resource`/`Generator`. |
| `src/Simulation/MatchPhase.h` | Raylib-free leaf: `enum class MatchPhase { Lobby, Playing, Finished }` + `ToString`. Общий для `MatchSimulation` (владелец) и `NetworkSnapshot`. |
| `src/VecConvert.h` | Граница raylib↔sim: `ToVector3(Vec3)` / `ToVec3(Vector3)`. Включает `raylib.h`, поэтому живёт **вне** `Simulation/`; sim-заголовки остаются raylib-free. |

Все типы без raylib (позиции/скорости — `Vec3`, не raylib `Vector3`), чтобы
будущий сервер/транспорт собирались без рендера.

---

## 3. CLI-режимы

`src/main.cpp` расширен. Неизвестные аргументы по-прежнему игнорируются
(не крашат). Существующие `--startup-smoke` / `--automatch` / `--crash-test` /
`--automatch-worker` не тронуты.

| Флаг | Поведение |
|---|---|
| `--server --port N [--password X] [--server-seconds S]` | **Реальный authoritative UDP-сервер** (Phase 0.1S), headless (без окна): bind порта, `SERVER_READY`, гоняет матч (боты + подключившиеся клиенты), применяет команды, шлёт per-client snapshots. Без `--server-seconds` крутится до прерывания; `S>0` — bounded (для теста). |
| `--host` | Выставляет `NetworkMode::LocalHost` и запускает обычную игру. В матче живой путь `PlayerCommand → ServerTick → snapshot` крутится через `LocalServerSession` (без влияния на геймплей — локальная симуляция остаётся authoritative). |
| `--connect <host:port> [--password X] [--server-seconds S]` | **Реальный сетевой клиент** (Phase 0.1S): connect по IP/port/token; при успехе — sustained loop (auto-move команда каждый тик + печать принятого snapshot'а = CLI-render матча, default 6s). Denied/timeout/мёртвый сервер — graceful (exit 0). |
| `--port <n>` | Заполняет `ServerConfig.port`. Валидируется `1..65535`; при пустом/нечисловом/вне-диапазона значении — ошибка в stderr и **exit 5** (без silent wrap через `uint16_t`). |
| `--server-name <s>` / `--password <s>` / `--token <s>` / `--private` | Заполняют `ServerConfig` (token = алиас password storage). |
| `--network-smoke` | Headless self-test (см. §7). |
| `--protocol-smoke` | Headless serialize→deserialize→equality self-test для wire-протокола (без окна/Game). Печатает `PROTOCOL_SMOKE_OK`, exit 0. Проверяет также truncated/bad-magic/version-mismatch/wrong-type. См. Phase 0.1P. |
| `--loopback-two-client-smoke` | Headless «server + 2 клиента в одном процессе» (без сокетов): 2 mock-клиента на разных игроков, разные команды, per-client snapshots. Печатает `LOOPBACK_SMOKE_OK`, exit 0. См. Phase 0.1Q. |
| `--localhost-net-smoke` | Headless **реальный UDP** server+client в одном процессе (loopback): client connect → server отдаёт snapshot → client его получает; bad connect (мёртвый порт) — graceful. Печатает `LOCALHOST_NET_SMOKE_OK` (или `…_SKIPPED` если `DAIBED_ENABLE_NETWORK=OFF`), exit 0. См. Phase 0.1R. |
| `--mp-loopback-smoke` | Headless **closed multiplayer** integration-тест (Phase 0.1S): authoritative server + 2 реальных UDP-клиента в одном процессе. Проверяет: оба коннектятся (разные игроки), wrong-password denied, оба видят матч, движение одного видно другому, disconnect не валит сервер. Печатает `MP_LOOPBACK_SMOKE_OK` (`…_SKIPPED` при OFF), exit 0. |

---

## 4. Локальный server/client мок

`LocalServerSession` задаёт **направление зависимостей**:

```
client  -> SubmitCommand(PlayerCommand)
server  -> DrainCommands() ; ServerTick()         // authoritative tick++
server  -> PublishSnapshot(MatchSnapshot)
client  -> LatestSnapshot()
```

Всё в одном процессе, без сокетов. Реальный ENet/UDP заменяет этот класс, не
меняя API `Game`/симуляции. Низкоуровневый `LocalNetworkMock` оставлен для
паритета connect/disconnect (`Game::Initialize/Shutdown`).

`LoopbackTransport` (Phase 0.1Q) — **multi-client** вариант той же модели: один
сервер + N клиентов с `clientId↔playerId` маппингом и per-client снапшотами
(`Connect`/`SubmitCommand(clientId,…)`/`PublishSnapshot(clientId,…)`/
`LatestSnapshot(clientId)`). `LocalServerSession` остался одно-канальным для
host/network-smoke.

---

## 5. Применение команды (`ApplyPlayerCommand`)

`PlayerCommand` — реальный источник, а не просто контейнер/очередь. Поток:

```
BuildLocalPlayerCommand()  // currentInput_ + камера -> PlayerCommand
        │
        ▼
ApplyPlayerCommand(Player&, const PlayerCommand&, dt)  // единая точка применения
```

`Game::ApplyPlayerCommand()` (`Game.cpp`) **реально применяет** к игроку:
- **aim** — `player.SetYaw(command.aimYaw)`; именно `command.aimYaw` является
  источником (а не `cameraController_.GetYaw()`). `command.aimPitch` сохраняется
  в `localAimPitch_` для будущих aim/actions (пока не используется);
- **movement** — базис берётся из `player.Forward()/Right()` (после `SetYaw`),
  wish = `forward*moveForward + right*moveStrafe`;
- **jump / sprint / sneak** — из команды, передаются в `Player::Move`;
- **selected slot** — `command.selectedSlot` с валидацией диапазона
  `[0, kHotbarSlotCount)`; вне диапазона — игнор (слот не меняется).

`Game::UpdateLocalPlayer()` собирает `BuildLocalPlayerCommand()` и применяет
именно команду через `ApplyPlayerCommand()`. Поведение обычной игры не
изменилось (для локального игрока `command.aimYaw == cameraController_.GetYaw()`
и `player.Forward()` совпадает с `GetFlatForward()`, значения побитово равны;
регрессия automatch — seed 777 без изменений, см. «Проверки»).

Флаг `localPlayerServerDriven_` (по умолчанию `false`): когда внешний авторитет
(server-mock в `--network-smoke`) применяет команду напрямую, `UpdateLocalPlayer`
не само-двигает игрока (иначе двойной шаг). В обычной игре всегда `false`.

Ещё **не** через команду (осознанно отложено, всё ещё `currentInput_`):
- замедление при прицеливании scope/blaster (`placeHeld`/`scopeHeld`); исторический `bridgeMode` удалён,
  `sprintTapped` — локальные assist/aim-флаги внутри `ApplyPlayerCommand`;
- attack/place/break (`UpdateAttackOrBreak`, `HandlePlaceBlock`),
  hero ability/utility/inventory/shop.

**Боты:** с 0.1K movement/aim формируют `PlayerCommand` и применяют его через
`ApplyPlayerCommand`; utility/combat/place/core assault ещё прямые. У ботов
**нет** состояния «выбранный hotbar-слот»:
`selectedHotbarSlot_` — поле, относящееся только к локальному игроку, поэтому
`ApplyPlayerCommand` меняет слот лишь для `player.IsLocal()`, а snapshot отдаёт
slot только для local/controlled игрока (для ботов — `0`). Полный перевод bot
actions на `PlayerCommand` (включая собственный slot-state/action target, если
понадобится) — отдельная следующая задача.

---

## 6. Server-authoritative snapshot extraction

`Game::BuildNetworkSnapshot() const` (`GameNetwork.cpp`) собирает **реальные**
данные текущего матча (не заглушку): tick, matchTime, phase, winner, по каждому
игроку (id/team/hero/pos/vel/health/alive/eliminated/slot) и по каждому кору
(team/health/maxHealth/alive). `selectedSlot` отдаётся настоящим только для
local/controlled игрока (`selectedHotbarSlot_`); для ботов — `0` (у них нет
slot-state). Без рендера/UI/звука/транспорта. Используется в `--network-smoke`,
`--server` и живом `--host`.

---

## 7. `--network-smoke`

`Game::RunNetworkSmoke()` (`GameNetwork.cpp`), headless, exit 0 при успехе.
Команда **реально применяется** к controlled player, а не просто дрейнится:

1. создаёт `ServerConfig`, конфигурирует и стартует `LocalServerSession`;
2. поднимает реальный headless-матч (`SetupMatch`, FourTeams), ставит
   `localPlayerServerDriven_ = true`, запоминает стартовые pos/yaw/slot;
3. 90 тиков: client формирует `PlayerCommand` (инъекция `moveForward = 1`,
   `selectedSlot = i % 9`, фиксированный `aimYaw`) → `SubmitCommand` →
   server `DrainCommands` → **`ApplyPlayerCommand(controlled, …)`** →
   `UpdateMatchSimulation` → `ServerTick` → `PublishSnapshot`;
4. печатает диагностику — реально ли команда повлияла на позицию/yaw/slot
   (`movedDistance`, `yaw start->end`, `slot start->end`, `snapshot slot`);
5. успех = непустые players/cores, `serverTick==90`, `commandsProcessed==90`,
   `commandsApplied==90`, `snapshot.tick==90`, `movedDistance>0.5`,
   `endSlot==8` и в snapshot `==8`, `endYaw==aimYaw`.

Наблюдаемый результат: controlled player `id=1` смещается `-38 → -31.93` по X
(`movedDistance≈6.07`), `yaw` остаётся `1.5708` (= `command.aimYaw`),
`slot 0→8`, snapshot slot `=8`.

---

## MatchSimulation Phase 0.1A

Первая настоящая граница server-authoritative симуляции. **Маленькая безопасная
граница**, а не вынос всей игры: `MatchSimulation` пока владеет только
**simulation clock + fixed-step config + command intake queue**, мир остаётся у
`Game`.

**Файлы добавлены:**
- `src/Simulation/MatchSimulation.{h,cpp}` — raylib-free класс:
  - clock: `Reset()` / `AdvanceTick()` / `CurrentTick()`;
  - fixed-step: `TickRate()` (60) / `FixedDeltaSeconds()` (`1/60`);
  - command intake: `SubmitCommand()` / `DrainCommands()` / `PendingCommandCount()`.

**Источник simulation tick — единый.** Поле `Game::simulationTick_` удалено;
тик живёт только в `matchSimulation_`:
- `UpdateMatchSimulation()` → `matchSimulation_.AdvanceTick()`;
- `SetupMatch()` → `matchSimulation_.Reset()`;
- `BuildLocalPlayerCommand().tick` и `BuildNetworkSnapshot().tick` →
  `matchSimulation_.CurrentTick()`.
Чтобы не плодить второй tick-counter, у `LocalServerSession` убраны
`ServerTick()`/`CurrentTick()` — он остался чистым transport/snapshot-каналом.

**Команды (Вариант Б, две стадии):** transport-буфер (`LocalServerSession`) →
sim-intake (`MatchSimulation`). В `RunNetworkSmoke()` поток:
```
client  SubmitCommand → session
server  session.DrainCommands → matchSimulation_.SubmitCommand
sim     matchSimulation_.DrainCommands → ApplyPlayerCommand(controlled)
        UpdateMatchSimulation(FixedDeltaSeconds)  // AdvanceTick
        session.PublishSnapshot(BuildNetworkSnapshot)
```

**Ownership мира** (`world_`, `players_`, `teams_`, `cores_`) **пока у `Game`** —
это осознанно: следующий этап. Сейчас цель — устойчивые «часы + граница команд».

**raylib-free:** `MatchSimulation.h/.cpp` не включают `raylib.h`, не зовут
`Draw*`/Camera/audio/UI; используют только `PlayerCommand` и стандартные типы.

**Fixed-step:** конфиг (`TickRate`/`FixedDeltaSeconds`) добавлен, `--network-smoke`
использует `FixedDeltaSeconds()` вместо локальной `1/60`. Полный
accumulator + render-interpolation ещё **не** внедрён (Фаза 0.2) — обычный цикл
по-прежнему шагает на `dt` кадра.

**Почему это ещё не dedicated server:** нет транспорта (сокетов), `MatchSimulation`
не владеет миром/игроками и сам не тикает мир (тик мира всё ещё в
`Game::UpdateMatchSimulation`), боты не на `PlayerCommand`. Это только граница.

**Проверки Phase 0.1A (2026-06-24, Release `/W4`, чисто):**
- build ok; `--startup-smoke` exit 0;
- `--network-smoke` exit 0: `simulationTick=90 snapshot.tick=90 tickRate=60
  fixedDt=0.0166667 commandsProcessed=90 commandsApplied=90 movedDistance≈6.07
  slot 0→8`, `NETWORK_SMOKE_OK`;
- `--server …` exit 0; `--connect …` exit 0; `--server --port -1` exit 5;
- `--automatch --runs 3 …` exit 0; `git diff --check` чисто;
- регрессия seed 777 без изменений (`kills=19, coreDamage=126, 26524 байта`).

**Следующий этап (сделан ниже — 0.1B):** перенос ownership части состояния в
`MatchSimulation`, начиная с raylib-независимого — match clock.

---

## MatchSimulation Phase 0.1B — match clock ownership

Первый перенос **владения состоянием** в `MatchSimulation` (а не только часов
тика). Выбран самый фундаментальный, raylib-free и скалярный кусок — **match
clock** (`matchTime`, секунды). Маленький безопасный шаг: одно поле, чисто
механический перенос значения, поведение побитово идентично.

**Что перенесено:**
- Поле `Game::matchTime_` **удалено**; время матча живёт в `MatchSimulation`:
  - `MatchTimeSeconds() const` — чтение;
  - `AdvanceClock(float dt)` — шаг (Game вызывает только пока нет победителя);
  - `Reset()` теперь обнуляет и tick, и matchTime.
- `Game::UpdateMatchSimulation`: `matchTime_ += dt` → `matchSimulation_.AdvanceClock(dt)`;
- `Game::SetupMatch`: явный сброс убран (его делает `matchSimulation_.Reset()`);
- все 55 чтений `matchTime_` (Game/Automatch/BotAI/Network/WorldTick/UI) →
  `matchSimulation_.MatchTimeSeconds()`.

**Почему именно clock:** генераторы/мир используют raylib `Vector3` (через
`Generator.h`/`raylib.h`), поэтому их перенос в **raylib-free** `MatchSimulation`
требует сначала собственного vector-типа (ROADMAP: «математику векторов заменить
своей/raymath-копией»). Это отдельный этап. Match clock — скаляр, переносится
без этого.

**Что `MatchSimulation` теперь владеет:** simulation tick + **match clock** +
fixed-step config + command intake queue. Мир/игроки/команды/коры — всё ещё у
`Game` (и `Game::UpdateMatchSimulation` по-прежнему тикает их).

**Проверки Phase 0.1B (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0: `matchTime=1.5` (из `MatchSimulation`),
  `simulationTick=90`, `commandsApplied=90`, `NETWORK_SMOKE_OK`;
- `--server …` exit 0; `--connect …` exit 0; `--server --port -1` exit 5;
- `--automatch --runs 3 …` exit 0; `git diff --check` чисто;
- регрессия seed 777 без изменений (`kills=19, coreDamage=126, 26524 байта`).

**Следующий этап (сделан ниже — 0.1C):** raylib-free vector-тип как
prerequisite для переноса генераторов/мира.

---

## MatchSimulation Phase 0.1C — raylib-free vector type (`Vec3`)

Prerequisite для переноса **spatial-состояния** (генераторы, потом мир/игроки) в
raylib-free `MatchSimulation`: эти сущности используют raylib `Vector3` (через
`raylib.h`), поэтому сначала нужен собственный vector-тип. Маленький безопасный
шаг: тип + его внедрение в уже-raylib-free snapshot-слой (без геймплея).

**Файл добавлен:** `src/Simulation/SimMath.h` — header-only, **без `raylib.h`**:
`struct Vec3 { float x,y,z; }` + `LengthSquared()`/`Length()` + операторы
`+`/`-`/`*`. Конвертация в/из raylib `Vector3` живёт на границе `Game` (там, где
raylib уже подключён), напр. `Vec3{ v.x, v.y, v.z }` — сам заголовок raylib не
тянет.

**Внедрение (контейнерно, 0 геймплейного риска):**
- `MatchSnapshot::PlayerSnapshot` — поля `x/y/z/vx/vy/vz` заменены на
  `Vec3 position` / `Vec3 velocity` (у snapshot-позиций не было ни одного
  читателя, кроме самого построения — поэтому замена локальна).
- `BuildNetworkSnapshot()` конвертирует `player.GetPosition()/GetVelocity()`
  (raylib) → `Vec3` на границе.
- `--network-smoke` считает `movedDistance` через `Vec3` (`(end-start).Length()`)
  и дополнительно проверяет, что **`Vec3`-позиция в snapshot совпадает с живой**
  позицией controlled-игрока (`posError≈0`) — наблюдаемо: `posError=0`.

**Почему пока только snapshot:** генераторы/`World`/`Player` всё ещё на raylib
`Vector3`; их перевод на `Vec3` (и затем перенос ownership в `MatchSimulation`) —
следующий этап, теперь разблокированный наличием `Vec3`.

**Проверки Phase 0.1C (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0: `snapshotPos=(...) posError=0`, `NETWORK_SMOKE_OK`;
- `--server …` exit 0; `--connect …` exit 0; `--server --port -1` exit 5;
- `--automatch --runs 3 …` exit 0; `git diff --check` чисто;
- регрессия seed 777 без изменений (`kills=19, coreDamage=126, 26524 байта`).

**Следующий этап (сделан ниже — 0.1D):** перевести `Generator`/`ResourcePickup`
на `Vec3`.

---

## MatchSimulation Phase 0.1D — Generator/ResourcePickup на `Vec3`

Подготовка генераторов и resource pickups к переносу в `MatchSimulation`:
их **spatial-состояние больше не зависит от raylib `Vector3`**. Ownership пока
остаётся у `Game` — переносим только тип позиции, не владение.

**Raylib-free теперь:**
- `ResourcePickup::position` (`Resource.h`): `Vector3` → `Vec3`; `Resource.h`
  включает `Simulation/SimMath.h` вместо `raylib.h`.
- `Generator` (`Generator.h`/`.cpp`): `position_`, конструктор и `GetPosition()`
  на `Vec3`; `Generator.h` больше не включает `raylib.h`; `Generator.cpp`
  считает дистанции через `Vec3` (`(a-b).LengthSquared()`), raylib не нужен.
- Проверено: `grep raylib|Vector3` по `Resource.h`/`Generator.h`/`Generator.cpp`
  и по `src/Simulation/` даёт только комментарии — кода с raylib нет.

**Граница конвертации** — новый `src/VecConvert.h` (включает `raylib.h` +
`SimMath.h`): `ToVector3(Vec3)` / `ToVec3(Vector3)`. Живёт **вне** `Simulation/`.
Конвертация только на Game/render-границе, где raylib уже подключён:
- создание генераторов (`GameSetup` → `ToVec3(position)`);
- рендер генераторов/пикапов (`Renderer` → `ToVector3(...)`);
- бот-AI ресурсного планирования (`GameBotAI`), магнит/сбор/мерж пикапов и
  Brom-пылесос (`GameWorldTick`), Brom-ультимейт (`HeroAbilities`) — оборачивают
  `pickup.position`/`generator.GetPosition()` в `ToVector3(...)` перед
  raylib-математикой/эффектами. Доступ к `.x/.y/.z` не менялся (у `Vec3` те же
  поля).

**Ownership:** `generators_` и `pickups_` всё ещё у `Game`; тик —
`Game::UpdateGenerators`/`UpdatePickups`. `MatchSimulation` их пока не владеет.

**Не тронуто (осознанно):** `DroppedItem` (death-drops) остаётся на raylib
`Vector3` — живёт в presentation-heavy `Feedback.h` рядом с визуальными
структурами и не входит в generator-подсистему; его конвертация раздула бы этап
без пользы для цели.

**Проверки Phase 0.1D (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0 (`posError=0`, `NETWORK_SMOKE_OK`);
- `--automatch --runs 3 …` exit 0; `git diff --check` чисто;
- регрессия seed 777 без изменений (`kills=19, coreDamage=126, 26524 байта`) —
  миграция value-preserving (та же float-арифметика).

**Следующий этап (сделан ниже — 0.1E):** перенос ownership `generators_` в
`MatchSimulation`.

---

## MatchSimulation Phase 0.1E — генераторы принадлежат `MatchSimulation`

Первый перенос **владения коллекцией сущностей** в `MatchSimulation` (мир/игроки
**не** трогались). `Generator` уже raylib-free (0.1D), поэтому `MatchSimulation`
владеет генераторами, оставаясь raylib-free.

**Что перенесено:**
- Поле `Game::generators_` **удалено**. `std::vector<Generator>` теперь живёт в
  `MatchSimulation`. `MatchSimulation.h` включает `Generator.h`/`Resource.h`
  (оба raylib-free) — raylib в `Simulation/` так и не появился.
- Тонкий API в `MatchSimulation`:
  - `ResetGenerators()` — очистка (вызывается в `SetupMatch` вместо
    `generators_.clear()`);
  - `Generators()` / `Generators() const` — доступ (setup делает
    `.emplace_back(...)`, боты/рендер читают);
  - `template UpdateGenerators(dt, pickups, forgeBonusForTeam)` — тик.
- `Game::UpdateGenerators` теперь делегирует:
  `matchSimulation_.UpdateGenerators(dt, pickups_, [this](int t){ return GetForgeBonusForTeam(t); })`.

**Зависимости, оставшиеся в `Game` (намеренно):**
- `pickups_` (`std::vector<ResourcePickup>`) всё ещё у `Game`; передаётся в
  `UpdateGenerators` по ссылке (генераторы кладут pickups в общий пул `Game`).
- **forge-бонус** считает `Game::GetForgeBonusForTeam` (зависит от `Team.forgeLevel`
  и match-time) — `MatchSimulation` не владеет командами, поэтому бонус приходит
  через callback на границе.
- Рендер генераторов (`Renderer`) и бот-планирование (`GameBotAI`) читают через
  `matchSimulation_.Generators()` и конвертируют `Vec3→Vector3` (`VecConvert.h`).

**`BuildNetworkSnapshot` расширен (безопасно):** добавлен `GeneratorSnapshot`
(`resourceType`/`teamId`/`Vec3 position`) и `MatchSnapshot::generators`.
Заполняется напрямую из `matchSimulation_.Generators()` без конвертации (позиции
уже `Vec3`). Additive — существующие читатели snapshot не ломаются.

**Проверки Phase 0.1E (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0: `generators=15` в snapshot, `simulationTick=90`,
  `matchTime=1.5` (clock/tick как раньше), `NETWORK_SMOKE_OK`;
- `--automatch --runs 3 …` exit 0; `git diff --check` чисто;
- регрессия seed 777 без изменений (`kills=19, coreDamage=126, 26524 байта`) —
  генераторы тикают как раньше.

**Следующий этап (сделан ниже — 0.1F):** перенос `pickups_` и dropped items в
`MatchSimulation`.

---

## MatchSimulation Phase 0.1F — pickups + dropped items принадлежат `MatchSimulation`

`MatchSimulation` теперь владеет **resource pickups и dropped items** (мир/игроки
**не** трогались). Хранение перенесено, тик-логика (spawn/magnet/collect/merge)
осталась в `Game` — она связана с игроками/инвентарём/эффектами/звуком.

**Что перенесено:**
- Поля `Game::pickups_` и `Game::droppedItems_` **удалены** → живут в
  `MatchSimulation` (`std::vector<ResourcePickup>` + `std::vector<DroppedItem>`).
- `DroppedItem` сделан **raylib-free**: `Vector3 position/velocity` → `Vec3`, и
  **перенесён из `Feedback.h` в `Inventory.h`** (raylib-free, рядом с `ItemStack`).
  `MatchSimulation.h` включает `Inventory.h` (raylib-free) — raylib в `Simulation/`
  не появился. `ResourcePickup` уже был на `Vec3` (0.1D).
- Тонкий API: `ResetPickups()`/`Pickups()`, `ResetDroppedItems()`/`DroppedItems()`
  (мутабельный + const), как у генераторов.

**Зависимости, оставшиеся в `Game` (намеренно):**
- Тик-логика `UpdatePickups`/`UpdateDroppedItems`/мерж/магнит/сбор — в `Game`
  (читает игроков, пишет в инвентарь, шлёт эффекты/звук). Работает через
  `matchSimulation_.Pickups()`/`DroppedItems()` (кэш-ссылка в начале функции).
- Спавн pickups генераторами: `UpdateGenerators` получает `matchSimulation_.Pickups()`.
- Бот-планирование (`GameBotAI`), Brom-пылесос/ультимейт, рендер, дроп игрока —
  ходят через accessors; `Vec3→Vector3` конвертируется на границе (`VecConvert.h`),
  где нужна raylib-математика/эффекты. `.x/.y/.z` доступ не менялся.

**Snapshot расширен (visible world items, без visibility filtering пока):**
`PickupSnapshot {resourceType, amount, Vec3 position}` и
`DroppedItemSnapshot {itemType, count, Vec3 position}` + `MatchSnapshot::pickups`/
`droppedItems`. `BuildNetworkSnapshot` заполняет из `matchSimulation_` напрямую
(позиции уже `Vec3`), пропуская `collected`. Оставлено место под per-client
visibility filtering (комментарий в `NetworkSnapshot.h`). network-smoke печатает
`pickups=N droppedItems=M` и проверяет, что секции отражают живое состояние
(`worldItemsReplicated`), а не пустую заглушку.

**Проверки Phase 0.1F (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0: `pickups=4 droppedItems=0`, `generators=15`,
  `simulationTick=90`/`matchTime=1.5` (clock/tick как раньше), `NETWORK_SMOKE_OK`;
- `--automatch --runs 3 …` exit 0; `git diff --check` чисто;
- регрессия seed 777 без изменений (`kills=19, coreDamage=126, 26524 байта`) —
  ресурсы появляются/подбираются как раньше (dropped items в automatch не
  создаются: смерть отдаёт ресурсы убийце, дроп — только ручной у человека).

**Следующий этап (сделан ниже — 0.1G / 4A):** cores + winner + match phase в
`MatchSimulation`.

---

## MatchSimulation Phase 0.1G — cores + winner + match phase (4A)

`MatchSimulation` теперь владеет **EnergyCores, winner и match phase**. Это
**фаза 4A** из задания: cores переехали целиком (объекты), а **mutation** коров
(`Damage`/`Repair`/`Restore`) **по-прежнему вызывается из `Game`** через
мутабельный accessor. Teams **не** перенесены — у них raylib `Vector3`
(`spawnPoint`/`shopPosition`), это отдельный большой шаг.

**Что перенесено:**
- `EnergyCore` уже raylib-free (`GridPos`, не `Vector3`), поэтому поле
  `Game::cores_` **удалено** → `std::vector<EnergyCore>` живёт в `MatchSimulation`.
  API: `ResetCores()`/`Cores()` (+const). `MatchSimulation.h` включает `Core.h`
  (raylib-free) — raylib в `Simulation/` не появился.
- Поле `Game::winnerTeamId_` **удалено** → `std::optional<int>` в `MatchSimulation`.
  API: `HasWinner()`/`Winner()`/`WinnerTeamId()`/`SetWinner()`.
- **Match phase** в `MatchSimulation`: `MatchPhase` вынесен в leaf-заголовок
  `src/Simulation/MatchPhase.h` (общий для sim и snapshot). API: `Phase()`/`SetPhase()`.
  - `Reset()` → `Lobby` + winner cleared; конец `SetupMatch` → `Playing`;
    `SetWinner(value)` → `Finished`. (Раньше snapshot выводил phase из `screen_`;
    теперь — authoritative из `MatchSimulation`.)

**`GameRules` работает с MatchSimulation-state:** `CheckWinCondition(teams_, players_)`
не изменился (читает `team.coreAlive` — teams остались в `Game`), но его результат
теперь идёт в `matchSimulation_.SetWinner(...)`. Sudden-death tiebreak —
`matchSimulation_.SetWinner(suddenDeathTiebreakTeamId_)`.

**`BuildNetworkSnapshot`:** `cores`/`winnerTeamId`/`phase` берутся из
`matchSimulation_` (`Cores()`/`WinnerTeamId()`/`Phase()`) — **authoritative**.

**Сохранённая механика (проверено):** core damage / core destroyed / final death /
sudden death (collapse) / Radon core sacrifice работают как раньше — объекты коров
теперь в `MatchSimulation`, но мутируются тем же кодом `Game` через `Cores()`.
В automatch матч завершается победителем (`winner=Yellow, timeout=no` на seed
424242) — путь win-condition → `SetWinner` → `HasWinner` → конец рана работает.

**Честный остаток (отложено):**
- `teams_` (raylib `Vector3` spawn/shop) — у `Game`; `team.coreAlive` дублирует
  состояние коров (синхронизируется логикой `Game`).
- Core mutation (combat/collapse/Radon) инициируется из `Game`, а не из
  `MatchSimulation` (нет `MatchSimulation::DamageCore(...)` API ещё).
- `world_`/`players_` — у `Game` (raylib `Vector3` глубоко).

**Проверки Phase 0.1G (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0: `phase=Playing cores=4` (authoritative из MatchSimulation),
  `NETWORK_SMOKE_OK`;
- `--automatch --runs 3 …` exit 0 (9/6/7 kills, как baseline); финиш-ран
  seed 424242 → `winner=Yellow`;
- `git diff --check` чисто; регрессия seed 777 без изменений
  (`kills=19, coreDamage=126, 26524 байта`).

**Следующий этап (частично сделан ниже — 0.1H):** подготовка `Player` к
переносу — spatial state на `Vec3`.

---

## MatchSimulation Phase 0.1H — Player spatial state → `Vec3` (partial)

Подготовка `Player` к будущему переносу в `MatchSimulation`: **внутреннее
хранение позиции/скорости/спавна теперь `Vec3`**, а для raylib Game/render
оставлены **адаптеры** `Vector3`. Это **partial** (как разрешено заданием):
storage + Vec3-геттеры сделаны, gameplay-методы пока работают через `Vector3`-
адаптеры. `Player` ещё **не** raylib-free и **не** переехал в `Simulation/`.

**Что изменено:**
- `Player::position_`/`velocity_`/`homeSpawnPoint_` — `Vector3` → `Vec3`
  (`Player.h` включает `Simulation/SimMath.h`).
- Boundary helpers: `GetPositionVec3()`, `GetVelocityVec3()`, `SetPosition(Vec3)`.
- `GetPosition()`/`GetVelocity()`/`GetHomeSpawnPoint()` остались `Vector3` — теперь
  это **адаптеры** (конвертируют `Vec3`→`Vector3`). Все существующие call sites
  (movement/combat/render/bots) не трогались — работают через адаптеры.
- Внутренняя физика (`Move`/`TryMoveAxis`/`Teleport`/`Kill`/`RespawnAtHome`)
  использует `.x/.y/.z` (совместимо с `Vec3`); поправлены только точки
  присваивания целого объекта на границе `Vector3`↔`Vec3` (~10 мест).
- `BuildNetworkSnapshot` берёт `player.GetPositionVec3()`/`GetVelocityVec3()`
  **без конвертации**.

**Честный остаток (отложено):** сигнатуры `Move(Vector3)`/`Teleport(Vector3)`/
`ApplyKnockback(Vector3)`/конструктор и `GetPosition()`→`Vector3` всё ещё на
raylib (адаптеры); `Player.h` включает `World.h`/`Hero.h`/`raylib.h`. Поэтому
`Player` пока не переносится в `Simulation/` — это следующий шаг (raylib-free
сигнатуры + World/Hero без raylib).

**Проверки Phase 0.1H (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0: `movedDistance=6.06694`, `posError=0` (snapshot
  Vec3-позиция = живой позиции), `NETWORK_SMOKE_OK`;
- `--automatch --runs 3 …` exit 0 (как baseline);
- `git diff --check` чисто; регрессия seed 777 без изменений
  (`kills=19, coreDamage=126, 26524 байта`) — movement/combat/respawn/death
  (всё гоняется в automatch) не сломаны.

**Следующий этап (частично сделан ниже — 0.1I / 6A):** `MatchSimulation` как
authoritative точка доступа к players.

---

## MatchSimulation Phase 0.1I — players access via `MatchSimulation` (6A)

`MatchSimulation` стал **authoritative точкой доступа** к игрокам. Это **6A** из
задания (read/write access через `MatchSimulation`); **6B** (storage внутри
`MatchSimulation`) отложен — `Player` ещё не raylib-free, и владение вектором в
`Simulation/` потянуло бы raylib в слой. Чтобы этого избежать, `MatchSimulation.h`
**forward-declare**'ит `Player` и держит **указатель** на вектор (storage пока в
`Game`).

**Что сделано:**
- API в `MatchSimulation`: `SetPlayers(std::vector<Player>*)`, `Players()`/const,
  `GetPlayer(int id)`/const. `MatchSimulation.h` — `class Player;` (без `Player.h`,
  без raylib); реализация в `MatchSimulation.cpp` (там `#include "Player.h"`).
- `Game::Initialize` инжектит указатель: `matchSimulation_.SetPlayers(&players_)`
  (один раз; указатель на вектор стабилен при `push_back`).
- `SetupMatch` создаёт игроков **через** `matchSimulation_.Players()`
  (`.clear()`/`.push_back(...)`).
- `Game::GetLocalPlayer()` теперь = `matchSimulation_.GetPlayer(localPlayerId_)`
  (раньше искал по `localPlayerId_` сам — то же поведение).
- `BuildNetworkSnapshot` берёт игроков из `matchSimulation_.Players()`.
- `localPlayerId_`/controlled id остались в `Game` (как разрешено заданием).

**Честный остаток (отложено — 6B):**
- **Storage** `std::vector<Player> players_` всё ещё в `Game` (не в
  `MatchSimulation`). `matchSimulation_.Players()` указывает на него.
- Update-циклы (`UpdateLocalPlayer`/`UpdateBots`/combat/`HandleDeathsAndRespawns`)
  работают с **тем же** вектором (через `players_` напрямую) — он же
  `matchSimulation_.Players()`. Их роутинг через API — косметика, не делал.
- `MatchSimulation.h` raylib-free (forward-decl), но `MatchSimulation.cpp`
  включает `Player.h` → реализация транзитивно тянет raylib. Полная
  raylib-независимость слоя требует raylib-free `Player` (signatures + World/Hero).

**Проверки Phase 0.1I (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений (forward-decl `std::vector<Player>*` собирается);
  `--startup-smoke` exit 0;
- `--network-smoke` exit 0: `players=16` (из `matchSimulation_.Players()`),
  локальный игрок двигается (`movedDistance=6.06694`, `posError=0`),
  `NETWORK_SMOKE_OK`;
- `--automatch --runs 3 …` exit 0 — боты/combat/respawn работают;
- `git diff --check` чисто; регрессия seed 777 без изменений
  (`kills=19, coreDamage=126, 26524 байта`).

**Следующий этап (сделан ниже — 0.1J):** все действия игрока на `PlayerCommand`.

---

## MatchSimulation Phase 0.1J — действия игрока через `PlayerCommand`

Теперь **все** действия локального игрока (не только movement/aim/slot) идут через
`PlayerCommand`. Прямое чтение `currentInput_` убрано из gameplay-mutation кода;
оно осталось только в **input/UI/camera/menu** слое (`HandleInput`,
`HandleInventoryInput`) и в `BuildLocalPlayerCommand` (input→command builder).

**`PlayerCommand` расширен** action-полями: `sprintTapped`; исторический `bridgeMode` впоследствии удалён,
`attackPressed`/`attackHeld`/`attackReleased`, `placePressed`/`placeHeld`,
`scopeHeld`, `useAbility1/2/Ultimate` (были), `useHeal`/`useTeleport`/`useDash`/
`useShoot`/`useFireball`/`useMolotov`/`useAlarm`. `BuildLocalPlayerCommand`
заполняет их все из `currentInput_`.

**Command-driven теперь (читают команду, не `currentInput_`):**
- движение/aim/slot + aim-slow/sprint-modifiers — `ApplyPlayerCommand`;
- **hero abilities 1/2/ultimate** — `ApplyPlayerActionCommand(player, command)`
  (новый единый apply-метод; `UseHeroAbilityInputs` делегирует ему);
- **utility items** (heal/teleport/dash/shoot/fireball/molotov/alarm) — `UseUtilityInputs`;
- **attack/break/ranged** (bow/blaster/melee/device) — `UpdateAttackOrBreak`;
- **block place** — `UpdateFastPlacement` (held) + `BuildPlacementPreview`
  (bridge-mode позиция) + `TryPlaceBlockForPlayer` (сообщение);
- scope-preview/`IsSniperScopeRequested` — `scopeHeld` из команды.

Приём: каждый gameplay-метод строит `BuildLocalPlayerCommand()` и читает поля из
неё. `currentInput_` стабилен между `HandleInput`→`Update` для playing-экрана,
поэтому значения идентичны — **поведение не изменилось** (seed-777 без изменений).

**Что ещё `currentInput_` (осознанно — это input/UI слой, item 6):**
- `HandleInput`: меню/пауза/shop/inventory-навигация/camera/hotbar-колесо/drop,
  **триггер place-клика** (`placePressed`→`UseSelectedItem`), shop-suppression
  hero-клавиш — всё это input-layer (клик/UI), сама логика place — command-driven.
- **Боты** — пока пишут действия напрямую (item 4 разрешает); `BotCommand`-output
  не делал (отдельный шаг).

**network-smoke (action command):** инжектит `useAbility1` на тике 0, применяет
через `ApplyPlayerActionCommand`. Наблюдаемо: `actionCommandsApplied=90`,
`ability1ReadyBefore=yes`, `ability1CooldownAfterCast=12` → **controlled effect**
(каст стартовал кулдаун), при этом `movedDistance=6.06694` не изменился.

**Проверки Phase 0.1J (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0: action controlled effect (cooldown 12s), movement не
  тронут, `NETWORK_SMOKE_OK`;
- `--automatch --runs 3 …` exit 0; `git diff --check` чисто;
- регрессия seed 777 без изменений (`kills=19, coreDamage=126, 26524 байта`).

**Следующий этап (частично сделан ниже — 0.1K):** боты → `PlayerCommand`
(BotCommand output); перенос place-клик-триггера в command-path; затем 6B
(raylib-free `Player` → storage `players_` в `MatchSimulation`), `teams_`;
fixed accumulator; visibility filtering.

---

## MatchSimulation Phase 0.1K — bot movement/aim через `PlayerCommand` (partial)

Боты больше не двигают `Player` напрямую в `GameBotAI.cpp`: основной
`UpdateSingleBot` и repair/upgrade movement helpers формируют `PlayerCommand`
и применяют его через `ApplyPlayerCommand`. Это **partial** по заданию:
самый рискованный/объёмный кусок (movement/aim) переведён, прямые bot-actions
оставлены и перечислены ниже.

**Что изменено:**
- Добавлен локальный helper `BuildBotMovementCommand(...)`: берёт старый
  world-space `wish` и bot `aimDirection`, выставляет `controlledPlayerId =
  bot.GetId()`, `tick = matchSimulation_.CurrentTick()`, `aimYaw`, `jump`,
  `sprint`, а `moveForward/moveStrafe` считает проекцией `wish` на
  command-basis (`Forward/Right`) после `aimYaw`. Так `ApplyPlayerCommand`
  восстанавливает прежний world-space wish математически тем же путём, что
  будущий server-command consumer.
- `ApplyPlayerCommand` теперь сохраняет старое bot-поведение autostep:
  `allowAutoStep = !player.IsLocal()`. Для local player smoke/обычной игры
  осталось `false`.
- Для action-facing/mining aim добавлен безопасный aim-only режим:
  команда с `dt <= 0` и без movement intent только выставляет yaw и не вызывает
  `Player::Move`.
- Убраны прямые `bot.Move(...)` и `bot.SetYaw(...)` из `GameBotAI.cpp`
  (проверено `rg`).
- Bot hero ability cast теперь тоже проходит через `PlayerCommand`:
  `BotCastHeroAbility` выставляет `useAbility1/2/Ultimate` и зовёт
  `ApplyPlayerActionCommand`. `ApplyPlayerActionCommand` возвращает `bool used`
  и блокирует shop/inventory только для local player, чтобы UI локального
  игрока не глушил ботов.

**Что ещё прямое у ботов (осознанно отложено):**
- `BotUseUtility` всё ещё напрямую тратит utility, лечит, телепортирует,
  ставит alarm, кидает fireball/molotov, стреляет из bow/blaster и делает dash.
  Для полного command-path тут нужен bot action target/direction и, возможно,
  bot slot-state.
- Bot melee/core assault/break-defense/blocking-block progress ещё вызывают
  combat/world mutation напрямую (`TryPerformBotMeleeAttack`,
  `TryPerformBotCoreAssault`, `TryBotBreakCoreDefense`,
  `TryBotBreakBlockingBlock`).
- Bot bridge/repair place (`TryBotBridgeBlock`, `TryBotRepairCoreDefense`)
  всё ещё напрямую зовут `TryPlaceBlockForPlayer`.
- Локальный place-click trigger (`placePressed` → `UseSelectedItem`) всё ещё в
  `HandleInput`; сама placement logic остаётся command-driven.

**Детерминизм:** baseline seed 777 **изменился ожидаемо**: конвертация
world-space `wish` → `aimYaw + moveForward/moveStrafe` восстанавливает тот же
вектор математически, но меняет порядок float-арифметики, а sprint-gating теперь
считается command-path'ом относительно `aimYaw`. Это меняет bot path dynamics.
Новый baseline для `--automatch --runs 1 --speed 512 --minutes 4 --seed 777
--stats <tmp>`: `kills=15`, `coreDamage=278`, `coreDestroyed=2`,
`finalDeaths=8`, `timeouts=1`, JSON size `27663` bytes. Baseline до 0.1K на
том же рабочем дереве: `kills=19`, `coreDamage=126`, `coreDestroyed=0`,
`finalDeaths=0`, `timeouts=1`, JSON size `26524` bytes. Дрейф принят: smoke
без crash/hang, длинный матч завершается победителем.

**Проверки Phase 0.1K (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений;
- `--startup-smoke` exit 0;
- `--network-smoke` exit 0, `NETWORK_SMOKE_OK`;
- `--automatch --runs 3 --speed 128 --minutes 3 --seed 20260624` exit 0
  (3/3 без crash/hang; короткие матчи дошли до time limit);
- финиш-ран `--automatch --runs 1 --speed 256 --minutes 20 --seed 424242`
  exit 0: `winner=Yellow`, `timeout=no`, `duration=746.112`;
- регрессия seed 777: новый baseline выше.

**Следующий этап:** довести bot-actions до command-path (utility/combat/place,
bot action direction/target и slot-state), перенести place-click trigger
локального игрока в command-path; затем 6B (raylib-free `Player` → storage
`players_` в `MatchSimulation`), `teams_`; fixed accumulator; visibility
filtering.

---

## MatchSimulation Phase 0.1L — world block deltas for replication

Подготовлен слой world replication: snapshot больше не планирует пересылать весь
`World` каждый tick, а несёт rolling block-delta/event stream.

**Новые типы:**
- `src/Network/BlockDelta.h` — raylib-free `BlockDelta`:
  `tick`, `GridPos position`, `oldType/newType`, `oldTeamId/newTeamId`,
  `ownerPlayerId`, `BlockDeltaReason`.
- `BlockDeltaReason`: `PlayerPlace`, `PlayerBreak`, `Explosion`, `FireBurn`,
  `TemporaryPlace`, `TemporaryExpire`, `PhaseRemove`, `PhaseRestore`,
  `CoreDestroyed`, `CoreCollapse`, `MapSetup`, `ReplicationTest`.
- `MatchSnapshot::blockDeltas` — только diffs/events; полного `World` в snapshot
  нет.

**Буфер:** `MatchSimulation` хранит `blockDeltas_`, даёт
`RecordBlockDelta`, `BlockDeltas`, `ClearBlockDeltasThrough(tick)` и
`ResetBlockDeltas`. Буфер bounded (`4096` entries): при переполнении старые
events отбрасываются. Publisher (`RunNetworkSmoke`, `SendMockNetworkInput`)
копирует буфер в snapshot и очищает deltas through published tick.

**Runtime-мутирующие места переведены на wrappers в `Game`:**
- place: `TryPlaceBlockForPlayer` → `PlaceWorldBlock(..., PlayerPlace)`;
- break: local/bot mining → `BreakWorldBlock(..., PlayerBreak)`;
- explosion/TNT/fireball: `DetonateAt` → `BreakWorldBlock(..., Explosion)`;
- fire/molotov burn: `UpdateHazardZones` → `BreakWorldBlock(..., FireBurn)`;
- Orbita temporary blocks: place/expire → `TemporaryPlace`/`TemporaryExpire`;
- Svidetel phase blocks: remove/restore → `PhaseRemove`/`PhaseRestore`;
- core removal/collapse: player/bot core destroy and sudden-death collapse →
  `CoreDestroyed`/`CoreCollapse`.

**Не delta-stream:** static map/setup construction (`GameSetup`, world builders,
generators/core initial placement) остаётся initial world setup, не per-tick
replication delta. В конце `SetupMatch` block-delta buffer явно сбрасывается.

**network-smoke:** на последнем smoke tick создаётся synthetic
`ReplicationTest` delta (`Air -> StoneBlock`) и проверяется, что latest snapshot
получил `blockDeltas=1`, а live buffer после publish очищен (`0`).

**Проверки Phase 0.1L (2026-06-24, Release `/W4`, чисто):**
- build без предупреждений;
- `--startup-smoke` exit 0;
- `--network-smoke` exit 0, `NETWORK_SMOKE_OK`, `blockDeltas=1`,
  `deltaBufferAfterPublish=0`;
- `--automatch --runs 3 --speed 128 --minutes 3 --seed 20260624` exit 0.

**Следующий этап:** реальные transport snapshots должны слать initial/static
world separately (map seed/chunks/baseline) + затем применять `blockDeltas`;
после этого довести bot-actions до command-path и идти в 6B/fixed accumulator/
visibility filtering.

---

## MatchSimulation Phase 0.1M — dynamic entity snapshot sections

Snapshot расширен на **реальные динамические игровые сущности** (раньше — только
players/cores/generators/pickups/dropped items + block deltas). Чисто additive
read-only extraction: gameplay-структуры не трогались, способности не ломались
(seed 777 байт-в-байт идентичен 0.1K baseline).

**Новые секции `MatchSnapshot`** (`NetworkSnapshot.h`), каждая raylib-free:
- `projectiles` (`ProjectileSnapshot`) — из `projectiles_` (`EnergyProjectile`):
  `kind` (mirror `ProjectileKind`), pos/vel `Vec3`, owner/team, `remainingLifetime`,
  `fireZone`.
- `explosives` (`ExplosiveSnapshot`) — из `timedExplosions_` (TNT-style): block
  center `Vec3`, owner/team, `remainingTimer`, `radius`.
- `hazardZones` (`HazardZoneSnapshot`) — из `hazardZones_` (fire/molotov pools):
  pos `Vec3`, owner/team, `remainingLifetime`, `radius`, `blueFire`.
- `heroDevices` (`HeroDeviceSnapshot`) — один flat-раздел с тегом `HeroDeviceType`:
  `BromVacuumBot`/`BromTurretDrone` (из `bromVacuumBots_`/`bromTurretDrones_`),
  `KonvoyTrap`/`KonvoyTether`/`KonvoyDome` (`konvoyTraps_`/`konvoyTethers_`/
  `konvoyDomes_`), `SvidetelEcho` (`svidetelEchoes_`), `LikhoBleed` (`likhoBleeds_`),
  `LikhoDisguise` (из `HeroRuntimeState`, только пока ult-маскировка активна).
  Free-standing девайсы несут свою позицию; tether/bleed/disguise-маркеры берут
  позицию owner/target-игрока через `matchSimulation_.GetPlayer(id)`.
- `statusEffects` (`StatusEffectSnapshot`) — per-player баффы/дебаффы из таймеров
  `Player` (`SpeedBoost`/`JumpBoost`/`Shield`/`Invulnerability`/`ControlDebuff`,
  emit только если `remaining > 0`), hero-флаги (`RadonProtected`/`RadonOverloaded`),
  и applied DoT/marks (`RadonBurn` из `radonBurns_`, `KonvoyMark` из
  `konvoyIntruderMarks_`). Добавлен геттер `Player::GetControlDebuffTimer()`.

**Поля каждой сущности (по заданию):** `id` + `type` + position `Vec3` +
owner/team + lifetime/remaining + **только public state** (AI/nav/internal timers
не копируются). `id` пока = индекс в исходной коллекции на момент snapshot
(стабилен внутри тика, не между тиками) — у sim-сущностей ещё нет stable
spawn-id; поле уже есть, чтобы форма snapshot была правильной (нужно для
interpolation/prediction позже).

**Public/private split (подготовлен, не enforced):** добавлен enum
`SnapshotVisibility { Public, OwnerTeam, Private }`; каждая сущность несёт
`visibility`. Visibility filtering **не** реализован (каждый клиент пока получил
бы всё) — поле только записывается. Примеры скаффолдинга: закопанный
`KonvoyTrap`/`KonvoyTether`/`LikhoBleed`/`LikhoDisguise` → `OwnerTeam`;
projectiles/explosives/hazards → `Public`; self-баффы → `Private`;
`RadonBurn`/`KonvoyMark` → `OwnerTeam` (команда-источник видит, что пометила цель).

**Конвертация на границе:** `BuildNetworkSnapshot` (`GameNetwork.cpp`)
конвертирует raylib `Vector3`→`Vec3` локальной лямбдой `toVec3` (структуры
сущностей всё ещё в `Game` на raylib). Snapshot-слой остаётся raylib-free.

**network-smoke:** на последнем тике инъектится synthetic `EnergyProjectile`
(distinctive pos/kind/`fireZone`) после world-update и до snapshot-build (как
block-delta тест). Проверяется, что он round-trip'ит в `snapshot.projectiles` с
корректными public-полями и `visibility=Public`, и что
`snapshot.projectiles.size() == projectiles_.size()` (все живые projectiles
реплицированы, без фильтрации). Печатаются счётчики всех секций.

**Проверки Phase 0.1M (2026-06-25, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-smoke` exit 0, `NETWORK_SMOKE_OK`: `dynamicEntities projectiles=1`
  (synthetic, `syntheticProjectile=found`), `statusEffects=52` (**реальная**
  секция из живого состояния), `explosives=0 hazardZones=0 heroDevices=0` (рано в
  матче ещё нет);
- `--server …` exit 0; `--connect …` exit 0; `--server --port -1` exit 5;
- `--automatch --runs 3 --speed 128 --minutes 3 --seed 20260624` exit 0;
  `git diff --check` чисто;
- регрессия seed 777 без изменений (JSON `27663` байта — идентично 0.1K
  baseline; extraction read-only, gameplay не тронут).

**Следующий этап (сделан ниже — 0.1N):** реальный visibility filtering per client
(использовать `visibility` + owner/team), затем stable spawn-id для сущностей
(interpolation), delta-compression секций; плюс остаток дорожной карты (transport,
6B, fixed accumulator).

---

## MatchSimulation Phase 0.1N — per-client visibility filter

Появился слой **«что конкретный клиент имеет право видеть»**: full snapshot
(server/debug) остаётся, а из него выводится per-recipient view. Чисто
read-only/pure — gameplay не тронут (seed 777 байт-в-байт `27663`).

**Политика видимости (4 класса):**
- **public** — видят все: позиции/здоровье игроков, cores, generators, world
  items (pickups/dropped), block deltas, projectiles/explosives/hazards.
- **team** — только команда владельца: `SnapshotVisibility::OwnerTeam` сущности —
  закопанные Konvoy traps, tethers, Likho bleed-маркер, Likho disguise-маркер,
  RadonBurn/KonvoyMark.
- **owner-private** — только сам игрок: его inventory (новые
  `InventorySnapshot`/`ItemStackSnapshot` в `PlayerSnapshot`), exact buff-таймеры
  (`SnapshotVisibility::Private`).
- **hidden enemy** — чего враг видеть **не должен**: disguise-маркер (отсечён как
  team-state) и **реальная личность замаскированного Likho** (для врага
  переписывается на disguise team/hero).

**Новые файлы:**
- `src/Network/SnapshotVisibility.{h,cpp}` — raylib-free pure-функция
  `FilterSnapshotForClient(const MatchSnapshot& full, int clientPlayerId)` +
  inline-предикат `IsVisibleToClient(visibility, ownerTeamId, ownerPlayerId,
  targetPlayerId, clientPlayerId, clientTeamId)`. Команда клиента выводится из
  **нефильтрованного** `full.players` (клиент всегда знает свою команду честно).
  Неизвестный клиент (spectator) → team `-1` → только public state.

**`Game::BuildNetworkSnapshotForClient(int clientPlayerId) const`**
(`GameNetwork.cpp`) = `FilterSnapshotForClient(BuildNetworkSnapshot(), …)`.
Обычный `BuildNetworkSnapshot()` остаётся **full** server/debug-снапшотом (item 4).

**Что делает фильтр:**
- players: оставляет всех (позиции public — fog/LOS нет), но **обнуляет inventory**
  у всех, кроме самого recipient; для **врага** замаскированного Likho
  переписывает `teamId/heroId` на disguise-значения; затем чистит сам
  `disguise*`-hint, чтобы он не утёк.
- public-секции (cores/generators/pickups/dropped/blockDeltas) — копируются как
  есть.
- projectiles/explosives/hazardZones/heroDevices/statusEffects — по тегу
  `visibility` через `IsVisibleToClient`. (Konvoy mark в full получил настоящий
  `ownerTeamId` из игрока-владельца, иначе OwnerTeam-фильтр не совпал бы.)

**Likho disguise/hidden, Svidetel contours, traps, fog/LOS, inventory:**
- disguise — реализован identity-swap для врага + team-only маркер для своих;
- traps/tethers/bleed — team-only (враг не видит закопанное);
- inventory — owner-private (стрипается у всех, кроме хозяина);
- **fog/LOS — в игре нет** (арена рендерит всех игроков всем), поэтому позиции
  пока public; структура фильтра (per-player loop) — точка, куда позже
  встанет fog. **Svidetel contours** — именно такой будущий team-side reveal
  (Svidetel с активным ультом видит контуры врагов сквозь стены), появится
  вместе с fog/LOS.

**network-smoke (item 5):** на последнем тике инъектится synthetic Konvoy trap
(owner = команда controlled). Из published full snapshot строятся `ownerView`
(controlled) и `enemyView` (игрок другой команды) и проверяется:
- **team-state**: trap во full и в `ownerView` есть, в `enemyView` — нет;
- **owner-private**: inventory controlled-игрока во full/owner `present`,
  `iron=13`; в `enemyView` — `present=false`, пусто;
- **public**: synthetic projectile виден во всех трёх; `enemyView.players.size()
  == full.players.size()` (роутинг позиций public).
Наблюдаемо: `trap[full/owner/enemy]=1/1/0`, `inv.iron=13/13/0`,
`publicProjectile[owner/enemy]=1/1`, `players[full/enemy]=16/16`.

**Проверки Phase 0.1N (2026-06-25, Release `/W4`, чисто):**
- build без предупреждений (raylib CMake-deprecation — не наш код);
  `--startup-smoke` exit 0;
- `--network-smoke` exit 0, `NETWORK_SMOKE_OK`: visibility-секция как выше;
- `--server …` exit 0; `--connect …` exit 0; `--server --port -1` exit 5;
- `--automatch --runs 3 --speed 128 --minutes 3 --seed 20260624` exit 0;
  `git diff --check` чисто;
- регрессия seed 777 без изменений (JSON `27663` байта — фильтр read-only/pure,
  не в gameplay-пути; smoke-only inventory inject не влияет на automatch).

**Следующий этап (fixed accumulator — сделан ниже, 0.1O):** stable spawn-id для
сущностей (interpolation), fog/LOS + Svidetel-contour reveal на основе этого слоя,
delta-compression секций; плюс остаток дорожной карты (transport, 6B).

---

## Phase 0.1O — fixed-step simulation loop (60 Hz) + render decoupling

Симуляция теперь тикает **фиксированным шагом** `FixedDeltaSeconds()` (60 Гц)
через accumulator, а рендер/презентация — отдельно, со своей частотой кадров.
Раньше interactive-путь делал ровно 1 тик на кадр с **переменным** frame dt
(привязка симуляции к FPS).

**`Game::Update(float dt)` переписан (`Game.cpp`):**
- `dt = min(dt, 0.05)` как раньше; затем ветвление:
- **automatch / headless** — fast-forward без зависимости от FPS: `N =
  clamp(automatchTicksPerFrame_)` фиксированных тиков `StepSimulationProfiled(
  FixedDeltaSeconds())` за кадр. `FixedDeltaSeconds()` бит-в-бит `1.0f/60.0f` —
  ровно то значение, которое старый цикл передавал (RunAutomatchBatch кормит
  `Update(1/60)`), поэтому **value-preserving**. Сохранён ранний `break`, как
  только batch завершился (иначе лишние тики на доигранном матче).
- **interactive** — `simulationAccumulator_ += dt`; `while (acc >= fixedDt) {
  StepSimulationProfiled(fixedDt); acc -= fixedDt; }`. `renderAlpha_ = acc/fixedDt`.
- **spiral-of-death guard**: `acc` клампится к `maxSteps * fixedDt`
  (`maxSteps = 32` interactive / `1024` headless); каждый кламп считается в
  `accumulatorClampCount_`.

**Input при fixed-step (`MergePendingInput`/`ClearOneShotEdges`,
`pendingLocalInput_`):** кадр может дать 0, 1 или несколько тиков. Чтобы one-shot
edge-инпуты (attack/abilities/utility/place click) не терялись на кадре с 0 тиков
(FPS>60) и не срабатывали дважды на кадре с несколькими тиками (FPS<60), локальный
ввод аккумулируется: continuous-поля берут последнее значение, edge-поля
OR-аккумулируются и чистятся после первого потребившего тика. На дефолтных
**60 FPS (1 тик/кадр) поведение идентично** прежнему (edge потребляется один раз
за кадр). Camera-yaw мыши применяется в `HandleInput` per-frame (не в sim),
поэтому прицел остаётся отзывчивым независимо от тиков.

**Render отдельно — презентация вынесена из `UpdateMatchSimulation`:** новый
`Game::UpdatePresentation(float dt)` (per-frame) делает `UpdateCombatPreview` +
`UpdateFeedback` + `UpdateCamera`. В per-tick хвосте `UpdateMatchSimulation`
остались только **gameplay/replication**: `UpdatePlacementPreview` (его потребляет
`UpdateFastPlacement`) + `UpdateFastPlacement` + `SendMockNetworkInput`. Так
камера/частицы/превью не дёргаются на FPS выше тикрейта (0-тик кадры) и не
обновляются дважды на медленном кадре. На 60 FPS — тот же порядок эффективно
(презентация — read-only, перестановка без геймплейного эффекта).

**Interpolation (отложена по заданию — «fixed simulation first»):** `renderAlpha_`
вычисляется и доступен, но **позиционная интерполяция сущностей не внедрена**
(требует хранения prev-tick состояния — отдельный большой шаг). Это явно
разрешено заданием.

**Диагностика (item 7):** dev-оверлей (F8 bot-debug) рисует строку
`sim-loop steps/frame=… alpha=… stepMs=… clamps=… tick=… acc=…s`
(`lastSimStepCount_`/`renderAlpha_`/`lastSimStepMs_`/`accumulatorClampCount_`).
`StepSimulationProfiled` консолидировал прежний inline-профайлинг (тот же
`profileSimulationMs_`/`profileSimulationTicks_`).

**Детерминизм (объяснение):** automatch headless **не изменился** —
`UpdateMatchSimulation` шагает тем же `1.0f/60.0f`, презентационные функции,
вынесенные per-frame, для headless и так не вызывались (`!headless_`), а
break-on-complete сохранён. network-smoke использует **свой** цикл (прямой
`UpdateMatchSimulation(fixedDt)`), поэтому не затронут.

**Проверки Phase 0.1O (2026-06-25, Release `/W4`, чисто):**
- build без предупреждений; `--startup-smoke` exit 0 (прогоняет interactive
  accumulator-путь + `UpdatePresentation` 8 кадров);
- `--network-smoke` exit 0, `NETWORK_SMOKE_OK`, `simulationTick=90 snapshot.tick=90
  matchTime=1.5` (tick/time стабильны);
- `--server …` exit 0; `--connect …` exit 0; `--server --port -1` exit 5;
- `--automatch --runs 3 …` exit 0; finish-run seed 424242 → `winner=Yellow
  timeout=no duration=746.112` (как прежний baseline);
- регрессия seed 777 байт-в-байт `27663` (детерминизм сохранён);
  `git diff --check` чисто.

**Следующий этап:** render-interpolation на основе `renderAlpha_` (хранить
prev/next tick позиции игроков/сущностей и интерполировать в рендере).

---

## Phase 0.1P — wire protocol (serialization, без транспорта)

Появился **бинарный wire-протокол**: реальные сетевые пакеты (header + payload),
которые будущий UDP/ENet-транспорт будет слать как есть. Транспорта по-прежнему
нет — это только слой сериализации в памяти. **Внешних networking-библиотек не
подключали.**

**Новые файлы `src/Network/NetworkProtocol.{h,cpp}`** (raylib-free, зависят только
от value-типов snapshot/command):
- `MessageType { Invalid, PlayerCommand, MatchSnapshot }` (u8);
- `kProtocolVersion = 1`, `kProtocolMagic = 0x31424400` ("DB1\0"),
  `kPacketHeaderSize = 19`;
- `PacketHeader { magic, protocolVersion, type, sequence, tick, payloadSize }` —
  покрывает требуемые поля (message type / protocol version / sequence / tick /
  payload), плюс magic для отбраковки чужих байт;
- `DecodeStatus { Ok, TooShort, BadMagic, VersionMismatch, WrongType, BadPayload }`.

**Формат — простой explicit-size little-endian binary (выбор и обоснование, шаг 3):**
каждое целое фиксированной ширины (u8/u16/u32/i32) little-endian; float — как его
IEEE-754 битовый паттерн (u32, bit-exact, без потерь как в тексте); bool — один
байт; массивы — с u32-префиксом длины. **Не** полагаемся на memcpy struct /
padding → формат стабилен и портируем. Для первого этапа проще и компактнее, чем
JSON; varint/delta-сжатие можно добавить позже не трогая call sites.

**Сериализация (шаг 2):** `PlayerCommand` (все поля), `MatchSnapshot` header
(tick/matchTime/phase/winner) + length-prefixed `PlayerSnapshot[]` (включая
вложенные `InventorySnapshot`/`ItemStackSnapshot` и disguise-поля) +
`CoreSnapshot[]` + `BlockDelta[]` (с `GridPos`/`BlockType`/`BlockDeltaReason`).
Публичный API: `EncodePlayerCommand`/`EncodeMatchSnapshot` →
`std::vector<uint8_t>`; `DecodeHeader`/`DecodePlayerCommand`/`DecodeMatchSnapshot`
→ `DecodeStatus` + заполненный `PacketHeader`.

**Безопасность парсинга (критерий «не крашит»):** `ByteReader` полностью
**bounds-checked** — каждое чтение проверяет remaining, при нехватке ставит
`ok_=false` и возвращает 0 (никаких out-of-range/throw). Length-префиксы
ограничены `kMaxArrayLen = 1<<20` (corrupt count не аллоцирует/не зациклит).
`ReadAndValidateHeader`: сначала magic (иначе `BadMagic`), затем заполняет header
и проверяет версию (иначе `VersionMismatch` — header уже несёт wire-версию для
репорта), затем `payloadSize > remaining` → `TooShort`. `nullptr,0` безопасен.

**`--protocol-smoke` (шаги 4–5):** headless, без окна/Game (диспатчится в
`main.cpp` до конструирования `Game`), exit 0 при успехе. Прогон:
1. roundtrip `PlayerCommand` (encode→decode→равенство всех полей);
2. roundtrip `MatchSnapshot` (header + 2 `PlayerSnapshot` [один с inventory, один
   с disguise/stripped] + 2 `CoreSnapshot` + 1 `BlockDelta`, побайтовое равенство);
3. **truncation**: декод каждого префикса snapshot-байтов (0..size-1) + `nullptr,0`
   + 3-байтовый буфер — ни один не даёт `Ok`, без краша;
4. **version mismatch**: портим version-поле → `VersionMismatch` + wire-версия;
5. **bad magic**: случайные байты → `BadMagic`;
6. **wrong type**: декод command-пакета как snapshot → `WrongType`.

Наблюдаемо: `PlayerCommand bytes=69 status=Ok`, `MatchSnapshot bytes=287
players=2 cores=2 blockDeltas=1 status=Ok`, все roundtrip/truncation/version/
magic/type — `ok`, `PROTOCOL_SMOKE_OK`.

**Проверки Phase 0.1P (2026-06-25, Release `/W4`, чисто):**
- build без предупреждений; `--protocol-smoke` exit 0, `PROTOCOL_SMOKE_OK`;
- `--startup-smoke` exit 0; `--network-smoke` exit 0, `NETWORK_SMOKE_OK`;
- регрессия seed 777 байт-в-байт `27663` (протокол — отдельный слой, не в
  gameplay-пути); `git diff --check` чисто.

**Следующий этап:** реальный транспорт (UDP/ENet/Steam Sockets) поверх этих
пакетов; затем sequence-based reliability/ack, delta-compression payload'а.

---

## Phase 0.1Q — loopback "server + два клиента" в одном процессе

Появилась локальная модель **один сервер + несколько клиентов без сокетов**.
`LocalServerSession` (одно-канальный mock) остался нетронут (host/network-smoke
его используют); добавлен отдельный `LoopbackTransport` для multi-client.

**Новый `src/Network/LoopbackTransport.{h,cpp}`** (raylib-free): in-process
сервер с N клиентами, где каждый клиент привязан к своему игроку.
- **clientId ↔ playerId mapping (задача 2):** `Connect(clientId, playerId)`
  отклоняет дубликат clientId **и** уже занятого игрока (`ClientForPlayer`),
  поэтому два клиента не могут владеть одним игроком на уровне подключения.
  `PlayerForClient`/`ClientForPlayer`/`IsConnected`/`ClientCount`.
- **client → server:** `SubmitCommand(clientId, command)` **штампует**
  `controlledPlayerId` маппингом клиента (authoritative) — клиент физически не
  может адресовать чужого игрока, даже если соврёт в поле. Возврат `false` если
  клиент не подключён.
- **server:** `DrainCommands()` (как у `LocalServerSession`).
- **server → per-client snapshots:** `PublishSnapshot(clientId, snapshot)` +
  `LatestSnapshot(clientId)`/`HasSnapshot(clientId)` — у каждого клиента свой
  снапшот-канал.

**`Game::RunLoopbackTwoClientSmoke()`** (`GameNetwork.cpp`, CLI
`--loopback-two-client-smoke`), headless, exit 0 при успехе. Поток (задача 4):
1. `SetupMatch` (FourTeams) — реальный матч;
2. два mock-клиента: A→local player (id 1, team 0), B→игрок другой команды (id 2);
3. `Connect` обоих; проверка что дубликат игрока/клиента **отклонён**;
4. 90 тиков: A и B шлют **разные** movement-команды (свои `aimYaw`, `moveForward=1`)
   → `transport.DrainCommands` → `matchSimulation_.SubmitCommand`/`DrainCommands`
   → `ApplyPlayerCommand` к их игрокам; сервер `AdvanceTick`/`AdvanceClock`;
   публикует **per-client** `BuildNetworkSnapshotForClient(playerId)`;
5. **spoof-тест:** клиент A шлёт команду с `controlledPlayerId=playerB` —
   transport перештамповывает на playerA, игрок B **не двигается**.

**Проверки в smoke (критерий готовности):**
- **два клиента ≠ один игрок:** `playerA≠playerB`; Connect отклоняет занятого
  игрока; spoof перештампован (`controlledPlayerId→playerA`) и игрок B не сдвинут.
- **tick/snapshot consistent:** `serverTick == snapA.tick == snapB.tick == 90`.
- **authoritative positions:** оба клиента видят **обоих** игроков на их живых
  позициях (`posErr A.A/A.B/B.A/B.B == 0`).
- **visibility hook подключаем (уже подключён):** per-client снапшоты идут через
  `FilterSnapshotForClient` — каждый клиент видит **свой** inventory
  (`aSeesA.present && !aSeesB.present`, и зеркально для B).

Наблюдаемо: `clients=2`, `mapping=ok (dupPlayer=rejected,dupClient=rejected)`,
`serverTick=90 snapTickA=90 snapTickB=90 snapshotsPublished=180`,
`movedA=6.06694 movedB=6.06694`, `posErr …=0`, `visibility=ok`,
`spoof(stamped=yes,B_unchanged=yes)`, `LOOPBACK_SMOKE_OK`.

**Проверки Phase 0.1Q (2026-06-25, Release `/W4`, чисто):**
- build без предупреждений; `--loopback-two-client-smoke` exit 0, `LOOPBACK_SMOKE_OK`;
- `--startup-smoke`/`--network-smoke`/`--protocol-smoke` exit 0;
- регрессия seed 777 байт-в-байт `27663`; `git diff --check` чисто.

**Следующий этап (реальный транспорт — сделан ниже, 0.1R):** UDP-сокеты под
`LoopbackTransport`-форму (connect → submit → drain → publish → read), затем
per-client visibility/fog поверх `FilterSnapshotForClient` и reliability/ack.

---

## Phase 0.1R — настоящий UDP-транспорт (localhost / private server)

Появился **первый реальный transport**: байты реально летят по UDP-сокетам
(loopback), а не только в памяти. Минимально (не production netcode): connect /
disconnect / send command / receive snapshot / timeout. Без reliability/ordering.

**Выбор транспорта (задача 1+2 — без молчаливой зависимости):** **native UDP**
через системный socket API (Winsock2 / BSD sockets). ENet рассмотрен, но **не
взят**, чтобы первый транспорт не тащил fetched-зависимость — только системная
библиотека `ws2_32` (Windows), под **CMake-опцией** `DAIBED_ENABLE_NETWORK`
(default ON). При OFF — `DAIBED_HAVE_NETWORK=0`, транспорт компилируется в
безопасные **stub'ы** (всё собирается/линкуется), smoke печатает `…_SKIPPED`.
Проверено в обе стороны: ON → реальные сокеты; OFF → stub + `LOCALHOST_NET_SMOKE_SKIPPED`.

**Интерфейс (задача 3) — `src/Network/NetworkTransport.h` + `UdpTransport.cpp`:**
- `INetworkTransport` (базовый: `IsOpen`/`Close`/`LastError`);
- `ServerTransport` — bind UDP (non-blocking, headless **без окна**), `Poll`
  (приём connect/command/heartbeat/disconnect + timeout-sweep клиентов),
  `BroadcastSnapshot`, `DrainCommands` (с `clientId`), `BoundPort` (ephemeral
  при port 0), counters;
- `ClientTransport` — `Open`/`Connect(host,port,timeout)`, `Poll`, `SendCommand`,
  `Disconnect`, `IsConnected`/`TimedOut`/`AssignedPlayerId`/`LatestSnapshot`.

**Поддержано (задача 4):** connect (handshake `Connect`→`ConnectAck` с
assigned playerId), disconnect (`Disconnect` пакет), send `PlayerCommand`
(`EncodePlayerCommand` по проводу), receive `MatchSnapshot` (`EncodeMatchSnapshot`),
**timeout** (сервер дропает молчащих клиентов; клиент детектит потерю сервера).
Сериализация переиспользует `NetworkProtocol` (0.1P); добавлены control-типы
`Connect`/`ConnectAck`/`Disconnect`/`Heartbeat` + `EncodeControl`/`EncodeConnectAck`/
`DecodeConnectAck`. Все приёмы bounds-checked (плохой/чужой/version-mismatch пакет
игнорится). Winsock-специфика: non-blocking + `SIO_UDP_CONNRESET=false` (чтобы
send на мёртвый порт не ронял recvfrom).

**Authoritative mapping:** при приёме `PlayerCommand` сервер **штампует**
`controlledPlayerId` маппингом клиента (как в 0.1Q) — клиент не управляет чужим
игроком.

**CLI (задача 5):** `--server --port N` — headless (без окна, как и было);
`--connect host:port` — теперь реальный `ClientTransport` (bounded connect,
graceful на мёртвый сервер).

**`--localhost-net-smoke` (задача 6):** server + client в **одном процессе** на
**реальном loopback UDP** (single-thread non-blocking poll-loop). Сервер bind на
ephemeral порт `127.0.0.1:0` (без конфликтов), клиент коннектится на
`BoundPort()`; сервер отдаёт advancing-snapshot, клиент его принимает; клиент
шлёт команду, сервер её видит. Затем **bad-connect** тест на мёртвый порт (`:1`)
с коротким таймаутом — graceful, без краша.

**Критерий готовности — проверено:**
- localhost-клиент **реально получает snapshot** от сервера (наблюдаемо:
  `connected=yes assignedPlayer=1 gotSnapshot=yes snapTick=2 snapPlayers=2
  serverSawCommand=yes`);
- **сервер не открывает окно** (smoke диспатчится в `main.cpp` до конструирования
  `Game`; `--server` headless);
- **плохой connect не крашит** (`badConnect(port=1) connected=no timedOut=yes
  handled=ok`; `--connect 127.0.0.1:1` → "could not reach … connect timed out", exit 0).

**Проверки Phase 0.1R (2026-06-25, Release `/W4`, чисто):**
- build (ON) без предупреждений; `--localhost-net-smoke` exit 0, `LOCALHOST_NET_SMOKE_OK`
  (стабильно 3/3 прогона); `--connect 127.0.0.1:1` graceful exit 0;
- build (OFF, `DAIBED_ENABLE_NETWORK=OFF`) без предупреждений, stub + `…_SKIPPED` exit 0;
- `--startup-smoke`/`--network-smoke`/`--protocol-smoke`/`--loopback-two-client-smoke` exit 0;
- регрессия seed 777 байт-в-байт `27663`; `git diff --check` чисто.

**Следующий этап (сделан ниже, 0.1S):** интегрировать `ServerTransport`/
`ClientTransport` в живой `Game`-цикл — closed multiplayer.

---

## Phase 0.1S — первый закрытый мультиплеер (host/server + клиенты)

Транспорт интегрирован в **живой authoritative server-loop**: `--server` реально
серверит матч, клиенты подключаются по IP/port/password, сервер применяет их
команды и шлёт per-client snapshots, клиенты их рендерят. Проверено и in-process
(integration smoke), и **в двух процессах** (server + 2 `--connect`).

**Протокол (auth):** добавлен `MessageType::ConnectDenied` + string-сериализация
(`ByteWriter::Str`/`ByteReader::Str`, capped); `Connect` теперь несёт **token**
(`EncodeConnect(token)`), `EncodeConnectDenied(reason)`. Версия протокола без
изменений (additive).

**Транспорт (`ServerTransport`/`ClientTransport`):**
- **password/token check (задача 2):** сервер на `Connect` валидирует token через
  `ServerConfig::ValidatePassword` (пустой пароль принимает любой); при несовпадении
  шлёт `ConnectDenied`, клиента **не** регистрирует. Клиент: `WasDenied()`/`DenyReason()`.
- **game-driven assignment:** транспорт держит password+endpoints, но **player**
  назначает игра: `TakePendingClients()` → игра выбирает игрока → `AssignPlayer(
  clientId, playerId)` (шлёт `ConnectAck` с playerId). `TakeDisconnectedClients()`
  (clientId+playerId) — игра освобождает слот. `ConnectedClients()` +
  `SendSnapshotToClient()` — per-client снапшоты. Команды стампятся на player клиента
  (как 0.1Q).
- handshake надёжен к потере: клиент **ретраит** `Connect` каждые 150 мс до
  `ConnectAck`/`ConnectDenied`/timeout; сервер ре-шлёт `ConnectAck` повторному `Connect`.

**Server authoritative loop (`Game`):**
- `NetworkServerSetup(transport, config)` — `SetupMatch` (FourTeams + боты),
  headless **без окна**, local player retired (`Kill`), `transport.Start`.
- `NetworkServerTick(transport, dt)` — `Poll` → assign pending (клиент берёт под
  контроль свободного **бота**) → free disconnected → drain+`ApplyPlayerCommand`
  authoritatively → `UpdateMatchSimulation(dt)` → per-client
  `BuildNetworkSnapshotForClient` + `SendSnapshotToClient`.
- `RunNetworkServer(config, maxSeconds)` — real-time loop на tickRate, `SERVER_READY`/
  `SERVER_STOPPED`, `maxSeconds<=0` = до прерывания.
- Боты не водят сетевых игроков: `UpdateBots` пропускает `IsNetworkControlledPlayer(id)`
  (`networkControlledPlayerIds_`). Их движение — из `PlayerCommand` клиента.

**Клиент:** `--connect host:port --password X` — `ClientTransport.Connect` (token),
затем sustained loop: каждый тик auto-move команда + печать snapshot'а (CLI-render:
`*myId(x,z)` + все игроки). Denied/timeout — graceful.

**Integration smoke `--mp-loopback-smoke`** (`RunMultiplayerLoopbackSmoke`):
authoritative server + 2 реальных UDP-клиента в одном процессе, ephemeral loopback
порт. Наблюдаемо: `bothConnected=yes playerA=2 playerB=3 (different=yes)
badPassword=denied bothSeeBoth=yes aMovedSeenByB=8.28
afterDisconnect[aSlotFreed=yes bConnected=yes bReceiving=yes serverAlive=yes]`,
`MP_LOOPBACK_SMOKE_OK`.

**Двухпроцессный прогон (наблюдаемо):** `--server --port 7799 --password lan` +
два `--connect 127.0.0.1:7799 --password lan` → клиент A (playerId=2) видит, как
игрок 3 (клиент B) идёт `(-1,-36)→(5,-35)`, а клиент B видит игрока 2 (клиент A)
`(37,-2)→(44,0)` — **оба видят один матч (16 игроков) и движение друг друга**,
авторитетно на сервере без окна.

**Критерий готовности — проверено:** ✓ два клиента на localhost видят один матч;
✓ движение одного видно другому; ✓ disconnect не валит сервер (B продолжает
получать snapshots, слот A освобождён). Плюс: ✓ password проверяется (wrong →
denied); ✓ сервер без окна.

**Ограничения (как разрешено заданием):** без prediction (клиент рендерит
authoritative snapshot напрямую, есть лаг RTT); без lobby polish (assign = первый
свободный бот; full match → клиент просто не получает слот); GUI пока CLI-render
(текст); карта у клиента не реплицируется (smoke/CLI инспектят snapshot напрямую).

**Проверки Phase 0.1S (2026-06-25, Release `/W4`, чисто):**
- build (ON) без предупреждений; `--mp-loopback-smoke` exit 0 `MP_LOOPBACK_SMOKE_OK`
  (стабильно 3/3); двухпроцессный прогон выше; `--server … --server-seconds 1`
  `SERVER_READY`/`SERVER_STOPPED` exit 0; `--connect` к мёртвому серверу graceful exit 0;
- build (OFF) без предупреждений, stub, `MP_LOOPBACK_SMOKE_SKIPPED` exit 0;
- `--localhost-net-smoke`/`--protocol-smoke`/`--loopback-two-client-smoke`/
  `--network-smoke`/`--startup-smoke` exit 0;
- регрессия seed 777 байт-в-байт `27663` (`UpdateBots`-скип инертен без сетевых
  игроков); `git diff --check` чисто.

**Следующий этап:** client-side prediction + reconciliation, entity interpolation
(`renderAlpha_`), GUI-render клиента (карта по seed/config), reliability/ack,
delta-compression, real lobby/slot policy, fog/LOS.

---

## Phase 0.1T — GUI-рендер сетевого клиента

`--connect host:port` больше **не печатает текст** — клиент открывает окно raylib
и **визуально рендерит живой матч** (карта, игроки, ядра, HUD), реплицируя мир
локально и применяя snapshot'ы. Архитектура — **вариант B**: тонкий клиентский
режим на существующем `Game` (переиспользуем `world_`/`players_`/`teams_`/
`matchSimulation_`/`renderer_`/`cameraController_`), без дублирующего `ClientGame`.
Этап — **только spectator-render**: ввода/команд/prediction/interpolation нет (это
0.1U); камера следит за назначенным игроком, видимое движение дают AI-игроки.

**Карта реплицируется через конфиг, не «seed».** Арена детерминирована от
`arenaBiome_`+`arenaLayout_`+`matchMode` (в билдерах арены нет map-RNG — единственный
`GetRandomValue` в setup это выбор героя бота). Поэтому клиент строит **тот же** мир,
вызывая обычный `SetupMatch()` с конфигом сервера, а runtime-правки докатывает
`blockDeltas`. Конфиг едет в **существующем `LobbySnapshot`** (additive):
- `ServerConfig`/`LobbySnapshot` (`NetTypes.h`) получили `int worldBiome/worldLayout/
  matchMode` (raylib-free ints, дефолты = Arena/Classic/FourTeams);
- `WriteLobbySnapshot`/`ReadLobbySnapshot` (+ `MakeSampleLobbySnapshot`/`SnapshotEqual`)
  в `NetworkProtocol.cpp` сериализуют их (3 `I32`, симметрично);
- `ServerTransport::BuildLobbySnapshot` копирует их из конфига;
- `NetworkServerSetup` выставляет их из `arenaBiome_`/`arenaLayout_`/`selectedMode_`.

**Клиентский режим `Game` (`GameNetwork.cpp`, raylib доступен транзитивно через
`Game.h`→`World.h`):**
- `RunNetworkClient(host, port, password, maxSeconds, lobbyPrefs)` — connect
  (graceful на denied/мёртвый сервер: рендерит сообщение, exit 0) → шлёт
  `LobbyUpdate` → loop: `Poll` → on match-start строит мир из `LatestLobbySnapshot`
  и ставит `localPlayerId_ = AssignedPlayerId()` → `ApplyClientSnapshot` → камера →
  `Render()`; до старта — `RenderNetworkLobby`. Disconnect → overlay → exit 0.
  `maxSeconds<=0` = до закрытия окна (ESC/крестик).
- `ApplyClientSnapshot(snapshot)` — мост snapshot→мир: reconcile `players_` по
  `playerId` (create/update/drop; identity team/hero из snapshot; для своего игрока
  ещё и inventory→HUD), cores (`SetHealth` by team), pickups/droppedItems (rebuild),
  `blockDeltas`→`world_` (Air→`RemoveBlock`, иначе `PlaceBlock` replace),
  clock/phase/winner в `matchSimulation_` (новый `SetMatchTimeSeconds`).
- `UpdateClientCamera` — third-person follow назначенного игрока (fallback —
  первый живой), отслеживая его yaw кратчайшей дугой.
- `Player::ApplyReplicatedState(health,maxHealth,alive,eliminated)` — client-only
  setter (отражает snapshot без боевой логики).

**Полный матч у сетевого клиента (16 игроков).** Лобби-матч раньше состоял **только
из roster-игроков (0 ботов)** — один клиент = 1 неподвижный игрок. Чтобы spectator
видел живую арену, `SetupMatch` в **network-only** ветке (`!pendingNetworkRoster_`)
теперь дозаполняет каждую активную команду ботами до `selectedTeamSize_` (roster
держит id 1..N и привязку к клиентам; боты — поздние id и едут на AI, не в
`networkControlledPlayerIds_`). Single-player ветка **не тронута** → automatch/seed
детерминизм без изменений.

**`--client-gui-smoke`** (`RunClientGuiSmoke`): headless-сервер (второй `Game`,
`Initialize(true)`, без окна) + этот windowed-клиент в одном процессе, single-thread
interleave (как `mp-loopback-smoke` + рендер). Ephemeral порт, `minPlayersToStart=1`.
Рендерит 30 кадров и проверяет: client connected + snapshot пришёл, мир построен,
`world_` block count == серверного, `players_.size()` == snapshot roster. Печатает
`CLIENT_GUI_SMOKE_OK` (`…_SKIPPED` при `DAIBED_ENABLE_NETWORK=OFF`).

**main.cpp:** `--connect` больше **не headless** (открывает окно) и зовёт
`RunNetworkClient` (CLI-флаги `--player-name/--team/--hero/--ready/--start` едут в
`LobbyUpdate`); добавлен `--client-gui-smoke` (windowed, как `--startup-smoke`).

**Проверки Phase 0.1T (2026-06-25, Release `/W4`, чисто):**
- build без предупреждений; `--client-gui-smoke` exit 0, `CLIENT_GUI_SMOKE_OK`
  (`connected=yes assignedPlayer=1 gotSnapshot=yes snapshotsApplied=29
  renderedFrames=30 players[client/snapshot]=16/16 worldBlocks[client/server]=4836/4836`
  — карта детерминированно совпала, 16 игроков реплицированы);
- `--startup-smoke` exit 0; `--protocol-smoke` exit 0 (lobby snapshot round-trip с
  новыми полями); `--mp-loopback-smoke` exit 0 (lobby/сервер не сломаны: bothSeeBoth,
  aMovedSeenByB=8.34, aSlotFreed); `--network-smoke`/`--loopback-two-client-smoke`/
  `--localhost-net-smoke` exit 0;
- регрессия seed 777 байт-в-байт `27663` (`kills=15 coreDamage=278 timeout`) —
  GameSetup-правка вне automatch-пути.

**Следующий этап (0.1U):** реальный input/команды у GUI-клиента (не spectator),
client-side prediction/reconciliation (инфраструктура `predictionHistory_`/
`remoteSnapshotBuffer_` уже есть), entity interpolation (`renderAlpha_`),
reliability/ack + initial/static world baseline отдельно от `blockDeltas`.

---

## Phase 0.1U — ввод GUI-клиента (играбельный сетевой клиент)

`--connect host:port` больше **не spectator**: клиент читает локальный ввод, шлёт
`PlayerCommand` на сервер и управляет своим назначенным героем по сети. Снапшот
остаётся authoritative-источником позиций (клиент рисует то, что прислал сервер),
поэтому есть **видимый лаг = RTT** — это ожидаемо и принято; client-side
prediction/reconciliation и интерполяция — следующая фаза (0.1W), здесь их нет.

**Клиентское звено «ввод → команда → отправка» (`GameNetwork.cpp`):**
- Новый `Game::SendClientInputCommand(ClientTransport&)` — каждый кадр в матче:
  `input_.Poll()` → `currentInput_`, **mouse-look применяется локально per-frame**
  (`cameraController_.AddLook(yawDelta, pitchDelta)`, как в 0.1O / `HandleInput`),
  затем `BuildLocalPlayerCommand()` (aimYaw из локальной камеры,
  `tick = matchSimulation_.CurrentTick()`), `controlledPlayerId` штампуется
  `client.AssignedPlayerId()`, и `client.SendCommand(command)`. Позицию игрока
  локально **не** симулируем (никакого двойного шага) — её даёт `ApplyClientSnapshot`.
- `RunNetworkClient` loop: после `BuildClientWorld` → `DisableCursor()` (захват
  мыши для FPS-look); порядок в матче — `ApplyClientSnapshot` → `SendClientInputCommand`
  → `UpdateClientCamera` → `Render`.
- **Камера своего игрока больше не «догоняет» yaw из снапшота.** `UpdateClientCamera`
  раньше всегда подтягивал camera-yaw к `follow->GetYaw()` (лаговому snapshot-yaw),
  что дралось бы с локальным mouse-look. Теперь: если следим за **своим живым**
  игроком — yaw ведёт локальный ввод (только двигаем камеру к реплицированной
  позиции); если **спектируем** чужого (свой мёртв) — поведение прежнее
  (follow-yaw кратчайшей дугой). Одноразовая синхронизация camera-yaw к спавн-facing
  игрока на первом follow-кадре (`clientAimInitialized_`), чтобы не было рывка.

**Меню/пауза/выход (ESC) обнуляют команду (как в single-player):** новый
`clientPaused_`. ESC в матче → пауза (`EnableCursor`, рисуется кадр «Paused —
Esc: leave / Enter: resume`); второй ESC в паузе → выход из матча; Enter → resume
(`DisableCursor`). В паузе **или при потере фокуса окна** (`IsWindowFocused()`)
`SendClientInputCommand` шлёт **нейтральную** команду (обнулённый `currentInput_`),
так что персонаж не двигается. В лобби (до старта) ESC по-прежнему выходит.

**Что НЕ трогали (вне scope, как требовало задание):** prediction/reconciliation,
интерполяция чужих, reliability/ack, протокол, серверная логика, single-player
путь. Серверная сторона (`UdpTransport` приём команд, штамп `controlledPlayerId`,
authoritative apply, per-client снапшоты) уже была готова — добавлено только
клиентское звено. **Известный остаток:** selected hotbar slot у сетевого игрока на
сервере не реплицируется (server-player не `IsLocal()` → snapshot отдаёт slot `0`,
`ApplyClientSnapshot` затирает локальный выбор) — это server-side задача, отдельно.

**Smoke `--client-input-smoke` (`RunClientInputSmoke`, headless, exit 0/9):** по
образцу `--client-gui-smoke`/`--mp-loopback-smoke`. Второй headless-`Game` как
authoritative-сервер (`Initialize(true)`, ephemeral loopback-порт,
`minPlayersToStart=1`) + этот клиент в одном процессе, single-thread interleave.
Клиент коннектится, входит в матч, 150 тиков шлёт `PlayerCommand` с `moveForward=1`
и фиксированным `aimYaw` (= спавн-yaw + 0.5 рад, чтобы yaw был заведомо отличен от
спавнового и forward-движение уходило в открытую арену). Критерии успеха:
- **сервер реально принял команды:** server-side назначенный игрок сместился
  (`serverMoved > 0.5`) и его yaw == отправленному `aimYaw`, `packetsRx > 0`;
- **позиция в ПРИНЯТОМ клиентом снапшоте сместилась** (`clientMoved > 0.5`);
- **yaw в снапшоте == отправленному `aimYaw`** (`|clientYaw − aimYaw| < 0.01`);
- **bad-path не регрессировал:** `--connect` к мёртвому порту (`127.0.0.1:1`)
  возвращает graceful (false, без краша).
Под `DAIBED_ENABLE_NETWORK=OFF` печатает `CLIENT_INPUT_SMOKE_SKIPPED`, exit 0.
Флаг зарегистрирован в `main.cpp` (в `headlessRun`, без окна).

**Проверки Phase 0.1U (2026-06-26, Release `/W4`, чисто):**
- build (ON) без предупреждений; `--client-input-smoke` exit 0,
  `CLIENT_INPUT_SMOKE_OK` (`assignedPlayer=1 commandsSent=150 snapshotsApplied=149
  packetsRx=155 injectedAimYaw=2.0708 serverMoved=10.38 serverYaw=2.0708
  clientMoved=10.37 clientYaw=2.0708 deadServerConnected=no`);
- build (OFF, `DAIBED_ENABLE_NETWORK=OFF`) без предупреждений,
  `CLIENT_INPUT_SMOKE_SKIPPED` exit 0 (восстановлено в ON);
- `--startup-smoke`/`--network-smoke`/`--client-gui-smoke`/`--mp-loopback-smoke`/
  `--protocol-smoke`/`--loopback-two-client-smoke`/`--localhost-net-smoke` — все exit 0;
- `--connect 127.0.0.1:1` (мёртвый сервер) — graceful exit 0;
- регрессия seed 777 байт-в-байт `27663` (клиентский input-путь вне automatch/
  single-player); `git diff --check` чисто.

**Следующий этап (0.1W):** client-side prediction + reconciliation для своего
игрока (инфраструктура `predictionHistory_`/`StorePredictedLocalCommand`/
`ApplyAuthoritativeSnapshotForPrediction` уже есть), entity-интерполяция чужих
(`renderAlpha_`/`TryGetInterpolatedRemotePlayerPosition`), reliability/ack +
initial/static world baseline отдельно от `blockDeltas`; server-side репликация
hotbar-slot сетевого игрока.

---

## Phase 0.1W — играбельный host: server-side действия + сглаживание клиента

Сетевой (host) режим стал **играбельным и плавным**: на сервере у сетевого игрока
теперь работают способности, utility и выбор слота хотбара; на клиенте свой игрок
**предсказывается** и движется каждый кадр (нет «3 FPS»/рывков), а чужие
**интерполируются**. Примитивы сглаживания (`predictionHistory_`/
`StorePredictedLocalCommand`/`ApplyAuthoritativeSnapshotForPrediction`/
`TryGetInterpolatedRemotePlayer*`/`renderAlpha_`) уже существовали — этот проход
**подключил** их к живому клиенту и развязал клиентскую презентацию от частоты
снапшотов. Host-поток (GUI «играть с ботами») = отдельный процесс `--server`
(`NetworkServerTick`) + GUI-клиент `RunNetworkClient` — затронуты обе стороны.

### Тир 1 — корректность

**#2 способности + utility на сервере (`NetworkServerTick`, `GameNetwork.cpp`).**
После `ApplyPlayerCommand` для каждой дренированной клиентской команды теперь зовутся
`ApplyPlayerActionCommand(*target, received.command)` (hero abilities 1/2/ult) и
`UseUtilityInputs(*target, received.command)` (heal/teleport/dash/shoot/fireball/
molotov/alarm). Оба apply-метода гейтят `shopOpen_`/`inventoryOpen_` **только для
local player**, поэтому сетевого игрока (на сервере он не local) UI-состояние хоста
не глушит — так же, как для ботов в Phase 0.1K.
- `UseUtilityInputs(Player&)` стал **command-driven**: сигнатура
  `UseUtilityInputs(Player&, const PlayerCommand&)`, читает переданную команду, а не
  `BuildLocalPlayerCommand()` (которая на сервере вернула бы ввод хоста). Выбор слота
  утилиты пишется в нужный store (local → `selectedHotbarSlot_`, сетевой →
  `Player::SetSelectedSlot`). Dash берёт направление из **камеры** для local и из
  `Player::Forward()` (свой authoritative yaw) для сетевого игрока — у сервера нет
  осмысленной камеры для чужого игрока. `SpendUtilityItem` теперь списывает предмет
  из слота владельца и для сетевого игрока (раньше — только local), с фолбэком на
  `SpendUtility(type)`.

**#3 per-player hotbar slot.** Добавлено состояние слота в `Player`
(`selectedSlot_` + `GetSelectedSlot()`/`SetSelectedSlot()`). `ApplyPlayerCommand`
валидирует `command.selectedSlot` и пишет его **для всех** игроков: local — в
`selectedHotbarSlot_` (UI-зеркало, поведение single-player без изменений), сетевой —
в `Player::SetSelectedSlot`. `GetSelectedHotbarStack(player)` читает слот по
владельцу: local → `selectedHotbarSlot_`, **network-controlled** (`IsNetworkControlledPlayer`)
→ `player.GetSelectedSlot()`, **бот** → пусто (как раньше — у ботов нет held-item,
их боевой путь это не использует, automatch-детерминизм сохранён). `BuildNetworkSnapshot`
отдаёт настоящий slot и для network-controlled игрока (`entry.selectedSlot`).
- **Замыкание цепочки на клиенте.** Клиент **владеет своим слотом локально**
  (predicted): `SendClientInputCommand` обрабатывает цифры/колесо → `selectedHotbarSlot_`
  → `BuildLocalPlayerCommand` шлёт его в команде; сервер хранит per-player и
  реплицирует обратно. `ApplyClientSnapshot` **больше не затирает** `selectedHotbarSlot_`
  из (лагового) снапшота — иначе только что выбранный слот мигал бы назад на RTT.

**#1 камера от первого лица.** `BuildClientWorld` при входе в матч делает
`cameraController_.Reset(...)` (как `SetupMatch`; `Reset` выставляет
`ViewMode::FirstPerson`) вместо оставшегося `SetMode(ViewMode::ThirdPerson)`.
Ростер на этот момент уже очищен, поэтому точный yaw синхронизируется к спавн-facing
назначенного игрока на первом снапшоте (`clientAimInitialized_`), а позиция —
каждый кадр в `UpdateClientCamera`.

### Тир 2 — плавность

**Prediction локального игрока.** `SendClientInputCommand(client, dt)` теперь
**применяет свою команду локально немедленно** (`ApplyPlayerCommand(*localPlayer,
command, dt)` для живого игрока) перед `StorePredictedLocalCommand` — игрок и
следящая камера двигаются **каждый кадр**, а не замирают между снапшотами. Reconciliation
(`ApplyAuthoritativeSnapshotForPrediction`, зовётся до `ApplyClientSnapshot`) уже была
в цикле; теперь она реально владеет позицией своего игрока, потому что
`ApplyClientSnapshot` **перестал** делать прямой `SetPosition` для своего игрока.
Новое правило в `ApplyClientSnapshot`: для своего игрока authoritative-позиция
адаптируется **только** когда prediction не может ей владеть — игрок мёртв/респаунится
или дрейф `> kClientHardResyncDistance` (3 м: респаун/телепорт/нокбэк/пропавшая
история). Иначе позицию ведёт prediction, мягкие коррекции (>0.18 м) делает
reconciliation. yaw своего игрока ведёт локальный mouse-look (камера), снапшот его не
перебивает.

**Interpolation чужих.** `ApplyClientSnapshot` развёл ветки local/remote: чужие
игроки рендерятся из буфера интерполяции (`TryGetInterpolatedRemotePlayerPosition/Yaw`,
задержка `networkInterpolationDelaySeconds_` ≈100 мс, адаптивная под age/loss до
320 мс) — скользят между снапшотами, а не телепортируются на пакет. `PushRemoteSnapshot`
(заполнение буфера, ≤32) уже звался в цикле.

**Render ≠ snapshot rate.** Свой игрок — из prediction (двигается на каждый
`GetFrameTime`), чужие — из interpolation-буфера; ни те, ни другие не «заморожены»
между пакетами. `renderAlpha_`/fixed-accumulator (0.1O) остаются основой для
интерполяции тиков локальной симуляции.

### Что осознанно НЕ делалось (остаток)

- **attack/break/place** для сетевого игрока на сервере (мили/лук/бластер/постановка
  блоков) — `NetworkServerTick` пока применяет движение+aim+слот+способности+utility,
  но не combat/placement (они camera-raycast-центричны в `UpdateAttackOrBreak`/
  `HandlePlaceBlock`); следующий шаг.
- Single-player/automatch путь (`localPlayerServerDriven_=false`) не тронут:
  local-слот остался в `selectedHotbarSlot_`, бот-held-item остался пустым,
  dash локального игрока остался от камеры — seed 777 байт-в-байт.

### Прочее в этом проходе

- `--network-smoke`: ассерт block-delta ослаблен с «ровно 1 дельта» до «синтетическая
  `ReplicationTest`-дельта присутствует среди дельт + буфер очищен после publish» —
  authoritative-мир на финальном тике теперь легитимно эмитит **свои** block-дельты
  (боты ломают/ставят, огонь), поэтому жёсткое `size()==1` стало хрупким (давало
  `blockDeltas=3`, FAIL). Интент теста (round-trip синтетической дельты + очистка
  буфера) сохранён.

**Проверки Phase 0.1W (2026-06-26, Release `/W4`, чисто):**
- `cmake --build build-release --config Release --parallel 4` — без предупреждений;
- `--startup-smoke` exit 0;
- `--network-smoke` exit 0, `NETWORK_SMOKE_OK`: `slot 0->8 (snapshot=8)`,
  `movedDistance=6.17`, `posError=0`, `correction=ok`;
- `--client-gui-smoke` exit 0, `CLIENT_GUI_SMOKE_OK`
  (`players[client/snapshot]=16/16 worldBlocks=4836/4836 cores=4/4 generators=15/15`);
- `--client-input-smoke` exit 0, `CLIENT_INPUT_SMOKE_OK`
  (`serverMoved=2.39 clientMoved=2.32 clientYaw=2.0708`, `deadServerConnected=no`);
- `--loopback-two-client-smoke` exit 0, `LOOPBACK_SMOKE_OK`
  (`interpolation buffer=32`, `endError=0.144`, оба клиента видят обоих);
- `--mp-loopback-smoke` exit 0, `MP_LOOPBACK_SMOKE_OK`
  (`bothSeeBoth=yes aMovedSeenByB=8.28`, disconnect не валит сервер);
- `--automatch --runs 3 --speed 128 --minutes 3 --seed 20260624` exit 0;
- регрессия seed 777 — **байт-в-байт `29833`** до и после этой фазы
  (`kills=17 coreDamage=108 coreDestroyed=0 finalDeaths=0 timeouts=1`): изменения
  сетевые/презентационные, общие code-paths (`ApplyPlayerCommand`/`GetSelectedHotbarStack`/
  `UseUtilityInputs`) сохранили поведение для local/бота. **Отклонение от
  задокументированного 0.1U baseline `27663`** объясняется **предыдущей
  незакоммиченной работой** (snapshot delta-compression/reconnect/prediction-инфра,
  ~0.1V) в рабочем дереве **до** этого прохода — этот проход baseline не сдвинул;
- `git diff --check` чисто; `src/Simulation/` и `src/Network/` остались raylib-free.

**Ручная проверка хоста (рекомендуется интерактивно):** GUI «играть с ботами»
поднимает `DaiBed.exe --server` (там `NetworkServerTick` с новыми способностями/
utility/слотом) и `RunNetworkClient` (вид от 1-го лица, prediction, slot-input) — обе
стороны покрыты автосмоками выше (server-side apply: `--mp-loopback-smoke`/
`--client-input-smoke`; client world/camera/snapshot: `--client-gui-smoke`).

---

## Phase 0.1X — netcode-фиксы по итогам аудита (скорость движения + FPS)

Внешний аудит «просто поиграть хостом» вскрыл два бага, которые 0.1W-сглаживание
**обнажило**, а не создало. Эта фаза — их корневой фикс.

### Баг A — игрок по сети полз ~в 3 раза медленнее + спам команд + «резинка»

**Симптом:** зажимаешь «вперёд», а проходишь куда меньше, чем должен; на фоне
prediction'а — мелкие рывки/откаты; клиент засыпал сервер дублями (за ~10 c живого
матча клиент слал ~1751 пакет ≈ 175 команд/с, сервер тикает 60/с).

**Корень:** клиент брал `command.tick` из **последнего полученного снапшота**
(`latestSnapshot.tick + 1`), снапшоты идут rate-limited **~20 Гц**
(`kSnapshotSendIntervalSeconds = 1/20`), а сервер **отбрасывает как stale** любую
команду с `tick <= lastProcessedCommandTick` (`UdpTransport`). Итог: `command.tick`
обновлялся только с приходом нового снапшота → сервер применял ввод ~20 раз/с вместо
60 → движение ~1/3 скорости и ~85% команд в мусор. А локальное предсказание (каждый
кадр, полная скорость) убегало от authoritative (1/3 скорости) → reconciliation
постоянно дёргала назад → «резинка».

**Фикс (`GameNetwork.cpp`, клиент):** ввод **семплится каждый кадр** (отзывчивый
mouse-look + слот), а **предсказание и отправка идут фиксированным шагом sim-тикрейта**
(аккумулятор, `FixedDeltaSeconds`), с **монотонным** `command.tick = ++networkCommandTick_`
(не из снапшота). Теперь:
- сервер применяет ~одну команду на тик → **полная скорость движения**;
- предсказание интегрируется тем же `fixedDt`, что и сервер → сходится, нет «резинки»;
- одна команда на тик вместо одной на кадр → **нет stale-спама**.
- `SendClientInputCommand(client, dt)` разбит на `SampleClientInput()` (per-frame) +
  `StepClientPredictionAndSend(client, fixedDt)` (per fixed step), spiral-guard
  `kMaxClientStepsPerFrame=8`. One-shot edge-инпуты (способности/utility/attack)
  аккумулируются между кадрами (`MergeClientInput`/`ClearClientInputEdges`,
  `pendingClientInput_`), чтобы не теряться на кадрах с 0 шагов (FPS>60) и не
  дублироваться на кадрах с несколькими (FPS<60).

### Баг B — клиент переобрабатывал снапшот каждый кадр (падение FPS)

**Симптом:** лишняя нагрузка тем больше, чем выше FPS.

**Корень:** матч-цикл вызывал `PushRemoteSnapshot` + `ApplyAuthoritativeSnapshotForPrediction`
+ `ApplyClientSnapshot` **каждый кадр**, пока `client.HasSnapshot()` (всегда true), —
то есть тяжёлый fold (reconcile ростера, **переразбор block-delta в `world_`**,
ребилд pickups/dropped, reconciliation) гонялся ~175 раз/с на **неизменном** снапшоте,
хотя снапшоты идут ~20 Гц.

**Фикс:** `ShouldApplyClientSnapshot(snapshot)` гейтит тяжёлый fold на **новый tick
снапшота** (`lastFoldedSnapshotTick_`/`hasFoldedSnapshot_`). Плавность чужих при этом
не страдает — новый `UpdateRemoteInterpolation()` обновляет позиции/yaw чужих из
буфера интерполяции **каждый кадр** (дёшево). Свой игрок — prediction (тоже каждый
кадр). reconciliation теперь зовётся раз на новый снапшот с `FixedDeltaSeconds()`.

### Проверки Phase 0.1X (2026-06-27, Release `/W4`, чисто)

- build без предупреждений; все смоки exit 0
  (`NETWORK_SMOKE_OK`/`CLIENT_GUI_SMOKE_OK`/`CLIENT_INPUT_SMOKE_OK`/`LOOPBACK_SMOKE_OK`/
  `MP_LOOPBACK_SMOKE_OK`);
- **`--client-input-smoke` (обновлён под монотонный tick):** `serverMoved=10.38`
  (было `2.39` до фикса — **полная скорость восстановлена**), `clientMoved=10.30`,
  **`staleDropped=0`** (раньше дропалось большинство команд), `serverYaw=clientYaw=2.0708`;
- **живой 2-процессный прогон** (`--server` + GUI `--connect`, localhost):
  клиент шлёт **~63 команды/с** (tx=759 за 12 c) ≈ тикрейт, было ~175/с (tx=1751 за 10 c)
  → **−64% трафика клиент→сервер**; сервер принял все (`rx=759`), delta-компрессия
  жива (`snapshots full/delta=1/183`), дисконнект не валит сервер;
- `--automatch --runs 3 … --seed 20260624` exit 0; регрессия seed 777 — **байт-в-байт
  `29833`** (фиксы клиент-онли, automatch/single-player не затронуты);
- `git diff --check` чисто; `src/Simulation/` и `src/Network/` raylib-free.

### Остаётся (из аудита, вне scope этого фикса)

- **B1 аудита:** attack/break/place + доступ к шопу/инвентарю для сетевого игрока
  (сейчас по сети нельзя драться/строить/покупать — ядро лупа не играется);
- завис дочернего `--server` при аварийном выходе GUI — проверить `StopLocalServer`
  на всех путях; плашка спектатора после смерти; смягчить реконнект-штраф 7 c.

---

## Phase 0.1Y — B1: бой и строительство сетевого игрока на сервере

Закрыт главный геймплейный пробел из аудита: сетевой игрок (хост, играющий с
ботами) теперь **бьёт, ломает и строит** — `NetworkServerTick` применяет его
attack/break/place авторитетно. Локальный путь
(`UpdateAttackOrBreak`/`HandlePlaceBlock`/`UpdateFastPlacement` с camera-raycast и
single-instance полями `breakProgress_`/`fastPlaceTimer_`) **не тронут** — добавлен
изолированный серверный путь, как у ботов.

**Новое (`GameNetwork.cpp`):** `ApplyNetworkPlayerActions(Player&, const PlayerCommand&,
float dt)` — вызывается в `NetworkServerTick` после movement/abilities/utility. Полностью
command-driven (камеры у сервера нет):
- **aim** — `AimDirectionFromCommand(command)` строит направление из `aimYaw`+`aimPitch`
  (та же формула, что `CameraController::LookDirection`); raycast — новый
  `RaycastFromPlayerEye(player, aimDir, maxDist)` из глаз игрока (`pos.y+0.78`).
- **melee** (`attackPressed`, оружие выбрано) → `combat_.Attack(...)` + `RegisterCombatEvent`.
- **break** (`attackHeld`) → per-player прогресс копки (raycast в блок/вражеский Кор,
  `BreakSeconds`, при `fraction>=1` — `CompleteBreakProgress`).
- **place** (`placeHeld`, выбран блок) → позиция из aim-raycast (`hit->adjacent`) либо
  bridge-mode (под-вперёд по yaw), затем `TryPlaceBlockForPlayer`; rate-limit per-player.
- Placing исключает attack/break в этот тик (как в локальном вводе).

**Per-player серверное состояние:** `networkActionState_` (map по playerId) держит
`BreakProgress` + `placeCooldown` для каждого сетевого игрока — у локального игрока свои
single-instance поля, которые серверу не нужны. Так несколько живых клиентов не делят
прогресс копки/таймер постройки.

**Доработки общих примитивов (player-generic, ботам прозрачно):**
- `GetSelectedWeaponType`/`EffectiveToolLevel` — теперь учитывают выбранный предмет и
  для **network-controlled** игрока (а не только local); бот по-прежнему получает
  `Sword`/tool-level (поведение и automatch-детерминизм без изменений).
- `SelectPlacementBlockForPlayer` — network-игрок ставит **свой выбранный** блок (бот —
  priority auto-pick, как было).
- `CompleteBreakProgress(Player&)` → `CompleteBreakProgress(Player&, const BreakProgress&)`
  (берёт прогресс параметром; локальный call-site reset'ит сам). Чистый рефактор,
  поведение локального игрока идентично.

**Что осознанно НЕ вошло (остаток B1):**
- **bow/blaster** (заряжаемый ranged) — у них charge-state, отдельный шаг; пока сетевой
  ranged не работает (melee/break/place — да).
- **шоп/инвентарь UI на клиенте** — сетевой клиент пока не открывает магазин/инвентарь
  (нельзя докупать); игрок строит/бьёт стартовым лоадаутом. Отдельная клиентская задача.
- **pitch у aim** — `aimPitch` идёт в команде и используется для raycast/aim; но
  bridge-mode placement берёт yaw-flat forward (как локальный), вертикальный mining/aim
  работает через pitch.

**Проверки Phase 0.1Y (2026-06-27, Release `/W4`, чисто):**
- build без предупреждений; **новый `--network-actions-smoke`** exit 0,
  `NETWORK_ACTIONS_SMOKE_OK`: `meleeHp 100->82 melee=ok | blocks 4837->4838 place=ok
  break=ok` (сетевой игрок реально снял HP врагу, поставил и сломал блок на сервере);
- все прочие смоки exit 0 (`NETWORK_SMOKE_OK`/`CLIENT_GUI_SMOKE_OK`/`CLIENT_INPUT_SMOKE_OK`/
  `LOOPBACK_SMOKE_OK`/`MP_LOOPBACK_SMOKE_OK`);
- `--automatch --runs 3 … --seed 20260624` exit 0; регрессия seed 777 — **байт-в-байт
  `29833`** (изменения только для local/network путей; бот/automatch не затронуты:
  `GetSelectedWeaponType`/`EffectiveToolLevel`/`SelectPlacementBlockForPlayer` для бота
  без изменений, `CompleteBreakProgress`-рефактор value-preserving);
- `git diff --check` чисто; `src/Simulation/`, `src/Network/` raylib-free.

**Остаётся:** bow/blaster для сетевого игрока; шоп/инвентарь UI на клиенте; завис
дочернего `--server` при аварийном выходе GUI; плашка спектатора после смерти.

---

## Что сделано

**Файлы добавлены:**
- `src/Network/NetTypes.h`, `src/Network/PlayerCommand.h`,
  `src/Network/NetworkSnapshot.h`, `src/Network/BlockDelta.h`,
  `src/Network/LocalServerSession.{h,cpp}`,
- `src/Simulation/MatchSimulation.{h,cpp}` (Phase 0.1A/B/E/F/G — clock/fixed-step/queue/match-time/**generators/pickups/droppedItems/cores/winner/phase**),
- `src/Simulation/SimMath.h` (Phase 0.1C — raylib-free `Vec3`),
- `src/Simulation/MatchPhase.h` (Phase 0.1G — `MatchPhase` enum),
- `src/VecConvert.h` (Phase 0.1D — `Vec3`↔`Vector3` на Game/render-границе),
- `src/Network/SnapshotVisibility.{h,cpp}` (Phase 0.1N — per-client фильтр
  видимости `FilterSnapshotForClient` + предикат `IsVisibleToClient`),
- `src/GameNetwork.cpp` (network-glue как отдельный TU `Game`),
- `docs/NETWORK_PREP_PLAN.md` (этот файл).

**Файлы изменены:** `CMakeLists.txt` (+3 TU), `src/Game.h` (типы/члены/методы,
`matchSimulation_`, `ApplyPlayerCommand`, `localPlayerServerDriven_`,
`localAimPitch_`; поля `simulationTick_` и `matchTime_` удалены), `src/Game.cpp`
(`matchSimulation_.AdvanceTick()`/`AdvanceClock()`, `ApplyPlayerCommand`,
`UpdateLocalPlayer` через команду, session→sim очередь в `SendMockNetworkInput`),
`src/GameSetup.cpp` (`matchSimulation_.Reset()`), `src/main.cpp` (CLI + валидация
`--port`); чтения `matchTime_` → `matchSimulation_.MatchTimeSeconds()` в
`Game/Automatch/BotAI/Network/WorldTick/UI`. **Phase 0.1D:** `Resource.h`/
`Generator.{h,cpp}` — `Vector3`→`Vec3` (raylib-free); `Generator.cpp`,
`GameSetup`, `Renderer`, `GameBotAI`, `GameWorldTick`, `Hero/HeroAbilities`,
`Game.cpp` — конвертация через `VecConvert.h` на границе. **Phase 0.1E:** поле
`Game::generators_` удалено (живёт в `matchSimulation_`); `GameSetup`/`GameWorldTick`/
`GameBotAI`/`Game.cpp` ходят через `matchSimulation_.Generators()`/`UpdateGenerators`;
`NetworkSnapshot` получил `GeneratorSnapshot`. **Phase 0.1F:** поля
`Game::pickups_`/`droppedItems_` удалены (живут в `matchSimulation_`); `DroppedItem`
→ `Vec3` и перенесён `Feedback.h`→`Inventory.h`; `GameWorldTick`/`GameBotAI`/
`GamePlayerActions`/`Hero/HeroAbilities`/`Game.cpp` ходят через
`matchSimulation_.Pickups()`/`DroppedItems()`; `NetworkSnapshot` получил
`PickupSnapshot`/`DroppedItemSnapshot`. **Phase 0.1G (4A):** поля
`Game::cores_`/`winnerTeamId_` удалены (живут в `matchSimulation_`); `MatchPhase`
вынесен в `Simulation/MatchPhase.h`; win-condition пишет `matchSimulation_.SetWinner(...)`;
`Game`/`GameSetup`/`GameWorldTick`/`GameBotAI`/`GamePlacement`/`GameUI`/`GameAutomatch`/
`Hero/HeroAbilities` ходят через `matchSimulation_.Cores()`/`HasWinner()`/`Winner()`;
snapshot cores/winner/phase — authoritative из `matchSimulation_`. **Phase 0.1H:**
`Player.{h,cpp}` — pos/vel/spawn `Vector3`→`Vec3` (storage); добавлены
`GetPositionVec3()`/`GetVelocityVec3()`/`SetPosition(Vec3)`; `GetPosition()`/
`GetVelocity()` стали `Vector3`-адаптерами; `BuildNetworkSnapshot` берёт
`Vec3`-геттеры напрямую. **Phase 0.1I (6A):** `MatchSimulation` получил
`SetPlayers`/`Players()`/`GetPlayer(id)` (header forward-declare'ит `Player`,
raylib-free); `Game::Initialize` инжектит `&players_`; `SetupMatch` создаёт
игроков и `GetLocalPlayer`/`BuildNetworkSnapshot` ходят через `matchSimulation_`
(storage `players_` пока в `Game`). **Phase 0.1J:** `PlayerCommand` расширен
action-полями; `BuildLocalPlayerCommand` заполняет их; `ApplyPlayerCommand`
(модификаторы), новый `ApplyPlayerActionCommand` (abilities), `UseUtilityInputs`/
`UpdateAttackOrBreak`/`UpdateFastPlacement`/`BuildPlacementPreview`/
`IsSniperScopeRequested` — читают команду, не `currentInput_`; smoke применяет
action-команду. **Phase 0.1K:** `GameBotAI.cpp` строит `PlayerCommand` для bot
movement/aim (`BuildBotMovementCommand`) и применяет его через
`ApplyPlayerCommand`; прямые `bot.Move`/`bot.SetYaw` убраны. `Game.cpp` сохраняет
bot autostep в `ApplyPlayerCommand` и поддерживает aim-only command (`dt <= 0`);
`HeroAbilities.cpp` возвращает `bool` из `ApplyPlayerActionCommand`, а bot hero
casts идут через action-command. **Phase 0.1L:** `BlockDelta`/`BlockDeltaReason`
добавлены в `Network/`; `MatchSimulation` хранит bounded block-delta buffer;
`Game` пишет deltas через `PlaceWorldBlock`/`BreakWorldBlock`/`RemoveWorldBlock`;
`BuildNetworkSnapshot` копирует `blockDeltas`, а publishers очищают buffer после
publish.

**CLI-режимы:** `--server`, `--host`, `--connect <addr>`, `--port` (валидируется
`1..65535`, иначе ошибка в stderr и exit 5 — без silent wrap через `uint16_t`),
`--server-name`, `--password`/`--token`, `--private`, `--network-smoke`,
`--client-gui-smoke`, `--client-input-smoke` (Phase 0.1U).

**Phase 0.1U — GUI-клиент управляет героем по сети:** `--connect` больше не
spectator. `Game::SendClientInputCommand(ClientTransport&)` (`GameNetwork.cpp`)
читает `input_.Poll()`, применяет mouse-look локально per-frame
(`cameraController_.AddLook`), строит `BuildLocalPlayerCommand()` (aimYaw из
камеры, `tick` из `matchSimulation_`), штампует `controlledPlayerId =
client.AssignedPlayerId()` и шлёт `client.SendCommand`. `RunNetworkClient`-loop:
`DisableCursor` на старте матча, `ApplyClientSnapshot → SendClientInputCommand →
UpdateClientCamera → Render`. `UpdateClientCamera` больше **не** подтягивает
camera-yaw к snapshot-yaw для своего живого игрока (локальный mouse-look ведёт),
для спектируемого чужого — прежний follow-yaw. ESC = пауза (нейтральная команда,
overlay), второй ESC = выход, Enter = resume; пауза/потеря фокуса обнуляют команду.
Позиция игрока локально не симулируется (authoritative из снапшота, лаг = RTT).

**Через `PlayerCommand` реально применяется** (Phase 0.1J — **все** действия
локального игрока; Phase 0.1K — bot movement/aim): movement/aim/slot +
aim-slow/sprint-modifiers
(`ApplyPlayerCommand`); hero abilities (`ApplyPlayerActionCommand`); utility items
(`UseUtilityInputs`); attack/break/ranged (`UpdateAttackOrBreak`); block place
(`UpdateFastPlacement`/`BuildPlacementPreview`); scope. `currentInput_` остался
только в input/UI/camera/menu (`HandleInput`/`HandleInventoryInput`) и в
`BuildLocalPlayerCommand`. Боты: movement/aim command-driven, hero ability casts
через `ApplyPlayerActionCommand`; utility/combat/place/core assault пока напрямую.
Проверено в `--network-smoke`: команда двигает controlled player +
action-команда (ability) даёт controlled effect (кулдаун).

**`MatchSnapshot` покрывает:** tick, matchTime, phase, winner; per-player
public state + **owner-private inventory** + disguise-hint (slot — настоящий
только для local/controlled); per-core state; generators/pickups/droppedItems;
block deltas (diff/event stream, не весь World); **dynamic entities**
(projectiles, explosives, hazard zones, hero devices, status effects — Phase
0.1M, public state + owner/team + `visibility` tag). **Phase 0.1N:** есть
per-client фильтр `FilterSnapshotForClient`/`BuildNetworkSnapshotForClient` —
public/team/owner-private/hidden-enemy split (см. раздел 0.1N); обычный
`BuildNetworkSnapshot` остаётся full server/debug-снапшотом. Осталось: fog/LOS
(+ Svidetel contours) и stable spawn-id под этот слой.

**До настоящего multiplayer остаётся:** реальный транспорт; initial/static world
baseline/chunks отдельно от block deltas; оставшиеся bot-actions на
`PlayerCommand` (+ action target/direction и slot-state, если нужен); перенос
ownership мира в `MatchSimulation` (clock/queue уже там); fixed accumulator +
render-interpolation; prediction/reconciliation; visibility filtering;
delta-compression; lobby/auth.

**Проверки (2026-06-24, Release `/W4`, чисто):**
- `cmake --build build-release --config Release --parallel 4` — без предупреждений;
- `--startup-smoke` → exit 0;
- `--network-smoke` → exit 0, `NETWORK_SMOKE_OK`; controlled `id=1`
  `movedDistance≈6.07`, `yaw=1.5708` (=aimYaw), `slot 0→8` (snapshot=8);
- `--server --port 7888 --server-name LocalTest --private --password q` → exit 0,
  `private=yes password=set`, `NETWORK_SMOKE_OK`;
- `--connect 127.0.0.1:7888` → exit 0 (client transport STUB);
- `--automatch --runs 3 --speed 128 --minutes 3 --seed 20260624` → exit 0, 3/3;
- `--server --port -1` (и `70000`, `abc`) → exit 5, понятная ошибка, без wrap;
- Phase 0.1K: build без предупреждений; `--startup-smoke` exit 0;
  `--network-smoke` exit 0, `NETWORK_SMOKE_OK`; `--automatch --runs 3
  --speed 128 --minutes 3 --seed 20260624` exit 0; финиш-ран seed 424242 →
  `winner=Yellow`, `timeout=no`; регрессия seed 777 принята как новый baseline
  (`kills=15`, `coreDamage=278`, `27663` байта).
- Phase 0.1L: build без предупреждений; `--startup-smoke` exit 0;
  `--network-smoke` exit 0, `NETWORK_SMOKE_OK`, `blockDeltas=1`,
  `deltaBufferAfterPublish=0`; `--automatch --runs 3 --speed 128 --minutes 3
  --seed 20260624` exit 0.

---

## Phase A — сетевая экономика: command-driven покупка в магазине

Первый шаг «parity roadmap» (см. чат-обсуждение): расширить **словарь команды**
дискретными экономическими действиями, чтобы удалённый человек мог то же, что
бот. Раньше `PlayerCommand` нёс только move/aim/combat/place/ability/utility — у
сетевого человека не было способа купить предмет; этот шаг закрывает **покупку**.

**Инвариант фазы:** «всё, что умеет бот, выразимо через `PlayerCommand`».
Покупка теперь выразима; боты пока остаются на прямом `TryShopPurchase` (тот же
валидируемый путь) — перевод ботов на эмиссию команды отложен, чтобы не трогать
детерминизм automatch в этом шаге.

**Дизайн (осознанные решения):**
- Действие встроено в существующий `PlayerCommand` (не отдельный канал/MessageType):
  `actionSeq`/`actionType`/`actionParamA`/`actionParamB` (+16 байт/тик). Дешевле,
  чем новый transport-путь; редкость действий компенсируется дедупом.
- **Exactly-once на сервере:** `economyActionSeq_` (playerId → последний
  применённый `actionSeq`). Команда с уже виденным seq игнорируется, поэтому
  одно и то же действие можно (пере)слать на нескольких тиках / при UDP-дубле —
  покупка не сработает дважды. Seq «съедается» даже при denied (запрос обработан).
- **Server-authoritative валидация:** жив + нет победителя + рядом с магазином
  (`shop_.IsPlayerInShop`, зеркалит локальный гейт «shop UI открывается только в
  зоне») + хватает ресурсов (`TryShopPurchase`). Результат (ресурсы/предметы/
  team-upgrades) реплицируется через owner-private inventory в снапшоте.
- Канал generic (`BuyItem`/`DropItem`/`MoveInventory`/`ChestTransfer`), реализован
  **BuyItem**; drop/inventory/chest — следующий инкремент по тому же пути.

**Файлы:**
- `src/Network/PlayerCommand.h` — `enum class PlayerActionType` + поля действия.
- `src/Network/NetworkProtocol.{h,cpp}` — (de)serialize новых полей в
  Write/ReadPlayerCommand; `CommandEqual`/`MakeSampleCommand` расширены;
  `kProtocolVersion` 7 → **8**.
- `src/Game.h` / `src/GameNetwork.cpp` — `ApplyPlayerEconomyCommand` (server
  handler + дедуп, вызван в `NetworkServerTick`), `QueueEconomyAction` (клиентская
  очередь), copy в `BuildLocalPlayerCommand`, clear-after-send в
  `StepClientPredictionAndSend`. `src/GameSetup.cpp` — сброс экономического
  состояния в `SetupMatch`.
- `src/main.cpp` — флаг `--network-purchase-smoke`.

**`--network-purchase-smoke` (`RunNetworkPurchaseSmoke`)**, headless, exit 0:
ставит controlled-игрока на магазин и через `ApplyPlayerEconomyCommand`
проверяет 4 случая — (1) валидная покупка списывает ресурс и выдаёт предмет,
(2) повтор той же команды (тот же seq) **не** покупает второй раз (дедуп),
(3) нехватка ресурсов → denied без изменений, (4) вне зоны магазина → denied
даже при наличии средств.

**Отложено (честно):** репликация per-client feedback покупки (сейчас denied/ok
видно только host-side через `AddEventMessage`; клиент видит лишь результат через
снапшот); client-side shop UI, открывающее покупку (это UI-слой, Phase F);
reliability-resend команды при потере пакета (Phase D); боты-через-команду.

**Проверки Phase A (2026-06-27, Release, чисто):**
- build без предупреждений; `--startup-smoke` exit 0;
- `--network-purchase-smoke` exit 0: `buy=ok (iron 5->0, wood 24->56)
  dedupe=ok deniedFunds=ok deniedRange=ok`, `PURCHASE_SMOKE_OK`;
- `--protocol-smoke` exit 0: `version=8`, `PlayerCommand … roundtrip=ok`
  (включает новые поля), version-mismatch handled, `PROTOCOL_SMOKE_OK`;
- `--network-smoke` / `--mp-loopback-smoke` / `--loopback-two-client-smoke`
  exit 0 (реальный UDP-путь переживает bump протокола);
- `--automatch --runs 1 --seed 777` детерминирован между прогонами
  (`kills=49 coreDamage=352`), `--runs 3` exit 0. Экономический путь зовётся
  только в `NetworkServerTick`, поэтому automatch-симуляция не затронута
  (новые поля `PlayerCommand` инертны вне сетевого пути).

---

## Следующие задачи

1. **`MatchSimulation` ownership** — tick/fixed-step/command-queue (0.1A),
   match clock (0.1B), `Vec3` (0.1C), `Generator`/`ResourcePickup` на `Vec3` (0.1D),
   `generators_` (0.1E), `pickups_`/`droppedItems_` (0.1F), `cores_`/winner/phase
   (0.1G/4A), `Player` storage → `Vec3` (0.1H) и **players access API (6A)** (0.1I)
   уже есть; далее **6B**: raylib-free `Player` (signatures + `World`/`Hero`) →
   перенос storage `players_` в `MatchSimulation`; `teams_` (`Vec3` в `Team`);
   опц. `MatchSimulation::DamageCore`-API.
2. **Fixed accumulator + render-interpolation** — accumulator-шаг **сделан**
   (0.1O: `Game::Update` тикает 60 Гц через accumulator, render/презентация
   per-frame, spiral-of-death clamp, input-edge буфер); осталось **render-
   interpolation** на основе `renderAlpha_` (prev/next tick позиции + интерполяция).
3. **Довести ботов до полного `PlayerCommand`** — movement/aim уже
   command-driven (0.1K), hero ability casts тоже через action-command; осталось:
   utility/combat/place/core assault, bot action direction/target и slot-state
   (если нужен) + перенос place-клик-триггера из `HandleInput` в command-path.
4. **World baseline/chunks** — static initial world надо доставлять отдельно
   (map seed/chunks/baseline), а затем применять `BlockDelta` stream; текущий
   snapshot уже несёт diffs, но не initial world.
5. **Real transport** — **готов** (0.1R: native UDP transport; **0.1S:
   интегрирован в живой `Game`-цикл — closed multiplayer**: `--server`
   authoritative-loop, `--connect` клиенты по IP/port/password, два процесса
   видят один матч). Осталось: sequence-ack reliability + delta-compression
   payload'а; ENet/Steam Sockets — опционально позже.
6. **GUI-render клиента** — **готов** (0.1T: карта по seed/config из `LobbySnapshot`,
   spectator-рендер живого матча). **Ввод/команды** — **готов** (0.1U: клиент
   управляет своим героем по сети — `SendClientInputCommand` шлёт `PlayerCommand`
   каждый кадр, mouse-look локально, пауза/ESC обнуляют команду; позиция
   authoritative из снапшота, лаг = RTT). **Играбельный host** — **готов**
   (0.1W): server-side способности/utility/hotbar-slot для сетевого игрока в
   `NetworkServerTick`; client-side **prediction/reconciliation** своего игрока
   (предсказывается каждый кадр, `ApplyClientSnapshot` не затирает позицию) +
   **интерполяция чужих** (`TryGetInterpolatedRemotePlayer*`, ~100 мс), убирающие
   «3 FPS»/рывки; камера от 1-го лица при входе в матч (`BuildClientWorld`).
   Осталось: attack/break/place сетевого игрока на сервере; render-интерполяция
   тиков локальной симуляции через `renderAlpha_`.
7. **Visibility filtering** per client — базовый слой есть (0.1N:
   `FilterSnapshotForClient`, public/team/owner-private/hidden-enemy); осталось
   **fog/LOS** (+ Svidetel-contour reveal) и stable spawn-id под него.
8. **Snapshot delta-compression** (бэйзлайн + дельты, а не полный мир).
9. **Lobby / private server auth** — базовая password-проверка на connect **есть**
   (0.1S: `Connect` token → `ValidatePassword` → `ConnectDenied`); осталось:
   хешированный токен/challenge вместо plain, real slot/team policy (сейчас assign
   = первый свободный бот), join/leave UI, max-players enforcement.

---

## Phase 6 slice — integrated singleplayer server (skeleton + экономика)

Первый шаг «singleplayer = integrated server» из
`docs/MULTIPLAYER_REFACTOR_PLAN.md` (Фаза 6, пункты 9–10). **Hybrid**: скелет
живёт рядом с прямым SP-путём; direct gameplay mutations из `Game::Update`
**не** удалялись — мигрирована только экономика.

**Скелет (`StartIntegratedServer`):**
- Обычный singleplayer (не automatch) в конце `SetupMatch` поднимает
  in-process сервер поверх существующего `LoopbackTransport` и подключает
  локального человека как loopback-клиента (`kIntegratedServerClientId` ↔
  `localPlayerId_`). Automatch остаётся direct — там нет human-клиента.
- `IntegratedServerTick()` (вызывается per-tick из `UpdateMatchSimulation`,
  рядом с `SendMockNetworkInput`): client шлёт `BuildLocalPlayerCommand()` →
  server `DrainCommands` → применяет **мигрированные** системы → публикует
  per-client visibility-filtered снапшот (`BuildNetworkSnapshotForClient`).
  Movement и остальной gameplay пока применяет прямой SP-путь (drain этих
  полей инертен — ничего не применяется дважды).

**Мигрированная система — shop/economy:**
- `ApplyPendingLocalPlayerAction` теперь при активном integrated server
  отправляет команду через `LoopbackTransport` и синхронно дренирует её на
  «серверной» стороне (`ApplyIntegratedServerCommand` →
  `ApplyPlayerEconomyCommand` — тот же валидированный/dedup метод, что у
  multiplayer в `NetworkServerTick`). Фоллбэк на прямой вызов остаётся, если
  транспорт не активен. Фидбэк — `PresentPlayerActionResult` прямо на
  серверной стороне (integrated клиент делит презентацию хоста, пока
  action-result snapshots не станут источником и для SP).
- Мёртвая прямая ветка `TryShopPurchase` в shop-click хэндлере (`Game.cpp`,
  недостижимая после command-path) удалена; у локального человека в SP больше
  **нет** прямых вызовов `TryShopPurchase` (остались боты — осознанно, и сам
  server-side `ApplyPlayerEconomyCommand`).

**Wire-формат не менялся** — протокол не бампался (loopback, без сокетов).

**`--integrated-server-smoke`** (headless, `INTEGRATED_SERVER_SMOKE_OK`):
mapping client↔player после `SetupMatch`; 30 тиков канала (commands drained,
snapshots published, snapshot tick/позиция = живые); покупка через транспорт
(iron −5 / wood +32, результат-сообщение показан); resend того же `actionSeq`
по проводу не покупает дважды; покупка вне shop-zone denied той же серверной
валидацией.

**Проверки (2026-07-01, Release):** build чисто; все smoke зелёные
(protocol/network/actions/ranged/purchase/client-input/loopback-two-client/
mp-loopback/client-dynamic-apply/movement-parity/integrated-server/startup);
`--automatch --runs 2 --seed 777` идентичен базлайну до изменений
(run1 kills=23 coreDamage=278, run2 kills=11 coreDamage=168).

**Честный остаток:** integrated клиент не потребляет снапшоты/ActionResult для
презентации (SP-рендер читает живое состояние); movement/combat/place/break/
inventory-UI drag в SP всё ещё direct; удаление direct-путей — следующие срезы
Фазы 6.
