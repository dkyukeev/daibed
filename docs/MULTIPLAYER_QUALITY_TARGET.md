# DaiBed — multiplayer quality target

> Цель документа: зафиксировать, к какому ощущению и техническому уровню должен
> прийти мультиплеер. Это не список текущих багов, а планка качества, против
> которой проверяются архитектурные решения.

## Цель

Мультиплеер DaiBed должен ощущаться как быстрый соревновательный PvP:

- локальное управление отзывчивое, как в хороших arena/team shooter серверах;
- свой игрок не "резинится" от обычного ping/jitter;
- чужие игроки, боты, снаряды и эффекты движутся плавно на render-rate, а не на
  частоте сетевых снапшотов;
- удары, кнокбэк, попадания, смерть, pickup, покупка, ломание и постановка
  блока дают немедленный и понятный feedback;
- singleplayer и multiplayer используют один gameplay pipeline, чтобы работа
  над одним режимом улучшала другой.

Эталон ощущения: не конкретная копия Marvel Rivals, TF2 или Quake, а их общие
качества: предсказуемость, малая задержка ввода, стабильный серверный authority,
плавная презентация удаленных сущностей и честная коррекция ошибок.

## Неформальные принципы

1. **Игрок всегда чувствует себя локально.**
   Команда движения, камера, слот, charge/aim и базовый feedback должны
   реагировать немедленно. Сервер может исправить состояние, но не должен
   превращать управление в ожидание снапшота.

2. **Сервер всегда решает истину.**
   Урон, смерть, экономика, блоки, pickup, cooldown, победа, respawn и inventory
   мутируются только на серверной стороне. Клиент может предсказывать и
   показывать feedback, но не становится authority.

3. **Singleplayer — это integrated server, а не отдельная игра.**
   Разница между singleplayer и multiplayer должна быть в транспорте и задержке,
   а не в gameplay-коде.

4. **Presentation не решает gameplay.**
   ClientPresentation может двигать визуальные снаряды, показывать hitmarker,
   звук, floating text и camera shake. Он не наносит урон, не покупает предметы
   и не ставит настоящие блоки.

5. **Сеть не должна просачиваться в game design.**
   Если механика хорошо ощущается в singleplayer, она должна иметь явный
   prediction/feedback/event путь в multiplayer, а не отдельную урезанную ветку.

## Минимальная техническая планка

| Область | Цель |
|---|---|
| Server tick | 60 Hz authoritative simulation для movement/combat/block/economy. |
| Client render | Presentation обновляется каждый кадр независимо от snapshot rate. |
| Own player | Client-side prediction + reconciliation по per-client command ack. |
| Remote players | Interpolation по render clock с адаптивным interpolation delay. |
| Projectiles/devices | Stable spawn ids + client visual interpolation/extrapolation. |
| Combat feedback | Event stream или надежно восстановимые события: hit, hurt, kill, core hit, pickup, buy result. |
| Snapshot | State replication достаточен для resync, но не заменяет event feedback. |
| Packet loss | Игра остается читаемой при 2-5% loss и умеренном jitter. |
| Reconnect | Слот игрока сохраняется, клиент получает full baseline и продолжает матч. |
| Security | Клиент отправляет намерения, а не результат: no client-side damage/economy/block authority. |

## Что должно быть заметно игроку

- При 60 FPS или выше удаленные игроки и боты не выглядят как 20 Hz объекты.
- Удар дает кнокбэк, звук, вспышку/текст/marker и читаемую реакцию тела.
- Быстрые снаряды не прыгают между позициями снапшота.
- Покупка, отказ покупки, pickup, drop, chest transfer и смена слота дают
  быстрый feedback, даже если подтверждение приходит снапшотом позже.
- Ломание/постановка блока не "исчезает из рук": есть локальный preview,
  серверное подтверждение и понятный rollback.
- В singleplayer и multiplayer один и тот же input дает один и тот же movement
  profile для human player.

## Что считается провалом

- `IsLocal()` влияет на физику human player.
- Remote human на сервере получает bot-only movement правила.
- Клиентский GUI вызывает gameplay mutation напрямую в одном режиме, а через
  команду в другом.
- Snapshot используется как единственный источник всех ощущений: звука, hitmarker,
  pickup text, buy result, death feed.
- Dynamic entity id равен индексу в vector и используется для interpolation.
- Singleplayer получает новую механику, а multiplayer требует отдельного ручного
  порта той же логики.

## Метрики готовности

Перед тем как считать архитектурный этап завершенным:

- `--client-input-smoke`, `--loopback-two-client-smoke`, `--mp-loopback-smoke`,
  `--network-actions-smoke`, `--network-ranged-smoke`,
  `--client-dynamic-apply-smoke`, `--protocol-smoke` проходят.
- Есть parity smoke: local human и network human получают одинаковую физику
  движения на одном наборе `PlayerCommand`.
- Есть симуляция плохой сети: jitter/loss/reorder не ломают movement, snapshot
  folding и event feedback.
- Automatch не деградирует по crash/stall/winner validity после переноса систем.
- Ручная проверка: 2 клиента на loopback и на искусственном ping чувствуют
  движение, удары, снаряды и покупки без грубой "сухости" сетевого режима.

