# Аудит способностей героев DaiBed

Дата снимка: 22 июня 2026 года. Аудит выполнен по текущему рабочему дереву, без исправления найденных дефектов.

> Обновление после дизайн-ревью: нижеследующие оценки сохранены как исходный снимок до исправлений. После ответов владельца игры реализован первый полный corrective pass: общие правила feedback/уникальности героев, утвержденные механики Радона, Орбиты и Брома, завершенный набор Конвоя, стойкий надрез и выбираемая маскировка Лихо, разрушаемые Эхо/фазовый участок/контуры Свидетеля. Альтернативный режим притяжения Радона намеренно оставлен за рамками до решения о новой способности.

## Краткий вердикт

В подсчёте ниже — 18 активных способностей (по две активки и одна ульта у каждого из шести героев). Пассивки и источники заряда ульты проверены отдельно.

- **Полностью работает и соответствует опубликованному описанию: 1/18** — «Робот-пылесос» Брома.
- **Работает частично: 14/18.** У способности есть полезный игровой результат, но часть обещанной механики отсутствует, отличается или плохо читается.
- **Сломано: 3/18** — все три способности Конвоя. Они создают объекты/визуалы, но их основное воздействие на игровой процесс отсутствует; ульта вдобавок недостижима штатным зарядом.
- **Не соответствует описанию: 17/18.** Единственное исключение — «Робот-пылесос»; у него остаются риски навигации и feedback, но заявленные правила реализованы.

Три наиболее критичные проблемы:

1. **P0 — Молотов Радона нарушает матч:** взрыв мгновенно удаляет все breakable-блоки, включая обсидиан и энергостекло, хотя описание обещает постепенное повреждение и иммунитет этих двух материалов. Попадание в игрока также складывает прямой урон с уроном взрыва и немедленным первым тиком огня ([GamePlayerActions.cpp:771–816](../src/GamePlayerActions.cpp#L771-L816), [Block.cpp:136–149](../src/Block.cpp#L136-L149), [GameWorldTick.cpp:577–613](../src/GameWorldTick.cpp#L577-L613)).
2. **P0 — защита Core ультой Радона обходится ботами и снарядами:** перехват вызывается только из локального завершения добычи Core; bot core assault и projectile damage уничтожают Core по другим путям ([GamePlacement.cpp:420–447](../src/GamePlacement.cpp#L420-L447), [GameBotAI.cpp:2782–2814](../src/GameBotAI.cpp#L2782-L2814), [GameWorldTick.cpp:561–569](../src/GameWorldTick.cpp#L561-L569)).
3. **P0 — Конвой функционально не завершён:** капкан не замедляет и не ослабляет прыжок, наручники не удерживают, купол не запирает/не замедляет/не имеет HP; источников заряда ульты Конвоя в коде нет ([GameWorldTick.cpp:1238–1332](../src/GameWorldTick.cpp#L1238-L1332), [Hero/HeroAbilities.cpp:1500–1572](../src/Hero/HeroAbilities.cpp#L1500-L1572)).

Три наиболее удачные способности в текущем виде:

1. **«Робот-пылесос» Брома** — ясное экономическое решение, реальная доставка в командный сундук, весовая вместимость и понятный риск потери темпа.
2. **«Видимый телепорт» Орбиты** — сильная идентичность «мобильность за здоровье», хорошие проверки геометрии и выразительный момент; ему не хватает безопасной опоры и настоящего preview-before-commit.
3. **«Силовой толчок» Радона** — простая, но пространственно выразительная контригра у края; основной push и LOS работают, хотя альтернативный pull и AI требуют исправлений.

## Методика и границы доказательств

Подтверждено прогоном:

- Release-сборка текущего дерева завершилась успешно.
- `--startup-smoke` завершился с exit code 0.
- Два headless automatch-прогона с seed `424242`, `--runs 2 --minutes 16`, при `--speed 1` и `--speed 256` дали одинаковые итоги: 0 timeout, средняя длительность 766,532 с, 41 убийство, 422 урона Core, 8 уничтоженных Core. Значит, при фиксированном шаге 1/60 результат не зависит от batching; automatch действительно вызывает `Update(1/60)` и меняет только число симуляционных тиков за frame ([GameAutomatch.cpp:110–140](../src/GameAutomatch.cpp#L110-L140), [Game.cpp:621–667](../src/Game.cpp#L621-L667)).
- Этот automatch **не является тестом реальных 10/30/60/144 FPS** и не ведёт счётчики кастов. В выбранных двух run Конвой не выпал в случайную выборку героев, поэтому его AI подтверждён статическим путём вызова, а не телеметрией этого запуска ([GameAutomatch.cpp:197–216](../src/GameAutomatch.cpp#L197-L216)).

Подтверждено статически:

- Все способности игрока и AI проходят через `UseHeroAbility`; мёртвый/устранённый игрок отклоняется до диспетчеризации ([Hero/HeroAbilities.cpp:149–209](../src/Hero/HeroAbilities.cpp#L149-L209)).
- Cooldown/duration уменьшаются через `dt`; смерть и respawn очищают active-флаги игрока, а новый матч очищает все глобальные hero-объекты ([Player.cpp:310–359](../src/Player.cpp#L310-L359), [Player.cpp:620–684](../src/Player.cpp#L620-L684), [GameSetup.cpp:329–374](../src/GameSetup.cpp#L329-L374)).
- При реальном FPS ниже 20 `dt` обрезается до 0,05 с, поэтому вся симуляция замедляется относительно реального времени; это не ability-specific, но нарушает требование одинакового real-time поведения на очень низком FPS ([Game.cpp:621–624](../src/Game.cpp#L621-L624)).
- `GAME_QUALITY_AUDIT.md` использован как список известных рисков, а не как источник истины: он уже отмечал отсутствующую пассивку Конвоя и называл Лихо/Свидетеля каркасными, но текущая реализация этих двух героев заметно продвинулась и была перепроверена по коду ([GAME_QUALITY_AUDIT.md:214–242](GAME_QUALITY_AUDIT.md#L214-L242)).

Не подтверждено интерактивно: субъективная читаемость на реальном экране в first-person/third-person, окклюзия VFX на конкретных картах и reduced-flashes при ручном управлении. Для них ниже приведён вывод из render/settings-кода, а не утверждение о playtest.

## Сводная таблица

| Герой | Способность | Технический статус | Соответствие дизайну | Fun 1–5 | Главная проблема |
|---|---|---:|---:|---:|---|
| Радон | Силовой толчок | Частично | Частично | 4 | Pull не усиливается от огня; ввод AI зависит от физического Alt/RMB |
| Радон | Коктейль Молотова | Частично / P0 | Нет | 3 | Мгновенно ломает обсидиан/стекло и даёт составной double-hit |
| Радон | Принять разрушение | Частично / P0 | Частично | 5 | Перехват Core работает только для одного пути уничтожения |
| Орбита | Дэш вперёд | Частично | Нет | 3 | Нет зарядки, overcharge-срыва и длинного штрафного cooldown |
| Орбита | Фантомные блоки | Частично | Частично | 4 | Это обычное энергостекло до мгновенного исчезновения |
| Орбита | Видимый телепорт | Частично | Частично | 4 | Preview показывается после commit; опора под destination не проверяется |
| Бром | Робот-пылесос | Полностью | Да | 4 | Только риск застревания/неразличимые команды, не подтверждённый отказ механики |
| Бром | Дрон-турель | Частично | Частично | 3 | Не имеет HP и не может быть уничтожен |
| Бром | Сборка обороны | Частично / P0 | Частично | 3 | При отказе уже всасывает ресурсы, не расходуя ульту |
| Конвой | Капкан | Сломано | Нет | 1 | Срабатывает без slow/jump penalty и не ломается врагом |
| Конвой | Наручники | Сломано | Нет | 1 | Линия рисуется, но дистанция не ограничивается |
| Конвой | Купол содержания | Сломано | Нет | 1 | Только визуальная сфера; штатно нельзя зарядить |
| Лихо | Тихие шаги | Частично | Частично | 3 | Шагов в audio pipeline нет, промах не снимает бонус |
| Лихо | Кровотечение | Частично | Частично | 3 | Нет «надреза» блока как состояния и нет финального burst |
| Лихо | Искажённая маскировка | Частично | Частично | 3 | Меняется в основном team tint; ranged/DoT damage не раскрывает |
| Свидетель | Усиленное Эхо | Частично | Частично | 3 | Не имеет HP, стреляет и обнаруживает через стены |
| Свидетель | Фазовый участок | Частично | Частично | 4 | Нет slow/eject/suffocation; блок может вернуться внутрь игрока |
| Свидетель | Контуры Войда | Частично | Частично | 2 | Контуры depth-tested, не включают pickups и не искажают Лихо |

## Общие системные находки

### Активация и feedback AI

`BotCastHeroAbility` правильно использует тот же gameplay entry point, но на время каста включает `suppressLocalFeedback_` и mute ([GameBotAI.cpp:3084–3116](../src/GameBotAI.cpp#L3084-L3116)). Подавление применяется не только к HUD, но и к `AddWorldEffect`/floating text ([Game.cpp:2011–2075](../src/Game.cpp#L2011-L2075)). Поэтому противник не видит стартовый push/cone, dash trail, teleport rings и другие cast-VFX бота. Это **P1: скрытое преимущество AI**, даже если последующий projectile/device становится видимым.

Способности используют общие generic SFX (`Pickup`, `Build`, `BreakBlock`, `CoreDestroyed`); отдельных сигнатур героев нет ([AudioSystem.cpp:63–74](../src/AudioSystem.cpp#L63-L74), [AudioSystem.cpp:103–119](../src/AudioSystem.cpp#L103-L119)). Reduced camera shake проходит через единый множитель, но reduced flashes влияет на post-process damage flash, а не на частые hero world effects ([GameSettings.cpp:168–171](../src/GameSettings.cpp#L168-L171), [PostProcessor.cpp:79–118](../src/PostProcessor.cpp#L79-L118)).

Hero devices всегда рисуются фиксированным цветом героя, хотя `teamId` передаётся в visual; team relation в `DrawHeroDeviceVisual` не используется ([Feedback.h:75–89](../src/Feedback.h#L75-L89), [Renderer.cpp:878–999](../src/Renderer.cpp#L878-L999)). Союзные и вражеские турели, капканы, купола и Эхо визуально не различаются.

### Заряд ульт

| Герой | Факт |
|---|---|
| Радон | Заряд равен фактически полученному после mitigation урону; соответствует основному обещанию ([Player.cpp:367–386](../src/Player.cpp#L367-L386)). |
| Орбита | Заряд идёт от расчётной скорости, risk-air и специальных событий, а не от фактического displacement. Бег в стену при ненулевой velocity потенциально продолжает заряд ([GameWorldTick.cpp:724–756](../src/GameWorldTick.cpp#L724-L756), [CombatSystem.cpp:611–622](../src/CombatSystem.cpp#L611-L622)). |
| Бром | Pickup, покупки, создание устройств, трофеи и доставка дают заряд; описание в целом соблюдено ([GameWorldTick.cpp:284–294](../src/GameWorldTick.cpp#L284-L294), [GamePlayerActions.cpp:675–677](../src/GamePlayerActions.cpp#L675-L677), [Hero/HeroAbilities.cpp:1264–1312](../src/Hero/HeroAbilities.cpp#L1264-L1312)). |
| Конвой | В проекте нет ни одного `AddHeroUltimateCharge` для Конвоя. Ульта недостижима без debug/state mutation. |
| Лихо | Есть заряд за backstab, bleed-hit и выход с базы; нет обещанного накопления просто за нахождение на базе и скрытное перемещение ([Game.cpp:2137–2175](../src/Game.cpp#L2137-L2175), [GameWorldTick.cpp:760–795](../src/GameWorldTick.cpp#L760-L795)). |
| Свидетель | Заряд дают смерть/убийство и обнаружение врага Эхом; ресурсы, Core, скрытые цели и уничтожение Эха не реализованы ([Game.cpp:1454–1459](../src/Game.cpp#L1454-L1459), [Game.cpp:2191–2195](../src/Game.cpp#L2191-L2195), [GameWorldTick.cpp:1390–1426](../src/GameWorldTick.cpp#L1390-L1426)). |

## Радон

### Силовой толчок

- **Заявлено:** push с малым уроном; после потери Core — pull-конус с LOS, меньшим воздействием на crouch и большим на горящие цели ([HeroSystem.cpp:16–20](../src/HeroSystem.cpp#L16-L20)).
- **Фактически:** instant cone до 5,6 блока, horizontal dot threshold 0,48, LOS raycast, 2 damage и knockback. Pull включается после потери Core только при удержании physical Alt/RMB ([Hero/HeroAbilities.cpp:459–465](../src/Hero/HeroAbilities.cpp#L459-L465), [Hero/HeroAbilities.cpp:473–545](../src/Hero/HeroAbilities.cpp#L473-L545)).
- **Работает:** фильтр союзников/мёртвых, стена, crouch-множитель 0,65, push/pull направление, miss message и 12-секундный cooldown.
- **Дефекты:** состояние «горит» нигде не хранится, поэтому обещанное усиление pull невозможно. Вертикальная дистанция исключена из range-test: цель далеко выше/ниже проходит по horizontal radius, если ray не встретил блок. Урон не вызывает `NoteDamageCredit`, поэтому последующее падение в void может не засчитаться Радону ([Hero/HeroAbilities.cpp:500–538](../src/Hero/HeroAbilities.cpp#L500-L538)).
- **Расхождение:** механика реализована частично; описание сильнее факта.
- **Спорные взаимодействия:** способность штатно расходуется при пустом конусе — это допустимый skill shot, но область не preview-ится. После смерти active state очищается, отдельных lingering-объектов нет.
- **AI:** разумно использует push у базы/края, но не умеет выбрать pull: bot cast читает тот же глобальный `IsKeyDown`, так что режим бота случайно зависит от клавиш локального пользователя ([GameBotAI.cpp:3135–3165](../src/GameBotAI.cpp#L3135-L3165)).
- **Feedback:** есть cone/pull, hit burst и miss text у игрока; у бота cast/hit VFX подавлены общей feedback-защёлкой.
- **Баланс и fun:** полезность **4**, надёжность **3**, читаемость **3**, уникальность **4**, мастерство **4**, контригра **4**, эмоция **4**, образ **5**, удовольствие **4**. Решение — сохранить push для края или pull для setup; риск — промах и длинный cooldown; ответ — стена, дистанция, crouch. Это не бесплатный урон, а хорошая пространственная кнопка.
- **Рекомендации:** **P1** передавать alt-mode через ability input/AI intent; учесть vertical range и burning; записывать damage credit. **P2** дать краткий предкастовый cone и team-readable цвет.

### Коктейль Молотова

- **Заявлено:** projectile создаёт огненную область, поджигает врагов, постепенно портит блоки; скорость продлевает горение, stop/crouch тушит; обсидиан и стекло иммунны; blue fire x2, сильнее наказывает скорость, хуже тушится и живёт меньше ([HeroSystem.cpp:21–25](../src/HeroSystem.cpp#L21-L25)).
- **Фактически:** projectile 12/24 damage, explosion radius 1,6; `DetonateAt` мгновенно ломает каждый breakable block в сфере, наносит второй AoE hit и создаёт radius 2,4 zone на 5/4 с с тиком 8/16 каждые 0,55 с ([Hero/HeroAbilities.cpp:563–592](../src/Hero/HeroAbilities.cpp#L563-L592), [GamePlayerActions.cpp:771–816](../src/GamePlayerActions.cpp#L771-L816), [GameWorldTick.cpp:656–693](../src/GameWorldTick.cpp#L656-L693)).
- **Работает:** projectile collision substeps уменьшают tunneling; союзники не получают player damage; blue fire действительно наносит x2 и живёт на секунду меньше.
- **Дефекты:** direct hit получает projectile damage, explosion damage и немедленный fire tick; persistent AoE (2,4) больше initial visual/explosion (1,6). Обсидиан/энергостекло входят в `IsBreakableByPlayers` и удаляются мгновенно. Взрыв ломает и союзные блоки. Нет burn-status, скорости, crouch или постепенного block damage ([GameWorldTick.cpp:577–613](../src/GameWorldTick.cpp#L577-L613), [Block.cpp:136–149](../src/Block.cpp#L136-L149)).
- **Расхождение:** реализация существенно сильнее и принципиально иная; это не устаревшая формулировка, а опасный gameplay defect.
- **Спорные взаимодействия:** projectile напрямую уменьшает HP вражеского Core, но не запускает обычный destroy pipeline, не вызывает жертву Радона и не синхронизирует `Team::coreAlive`/world block ([GameWorldTick.cpp:561–569](../src/GameWorldTick.cpp#L561-L569)).
- **AI:** выбирает текущую цель на 4,5–12 блоках, но не прогнозирует дугу, стену, союзные постройки или край ([GameBotAI.cpp:3167–3175](../src/GameBotAI.cpp#L3167-L3175)).
- **Feedback:** projectile trail и повторяющийся fire-zone заметны; нет предиктора приземления, а initial radius вводит в заблуждение. Bot projectile виден после создания, но начальный cast VFX/SFX скрыт.
- **Баланс и fun:** полезность **5**, надёжность **4**, читаемость **2**, уникальность **3**, мастерство **3**, контригра **1**, эмоция **4**, образ **4**, удовольствие **3**. Сейчас это кнопка бесплатного удаления защиты и огромного burst; сильный момент зрелищен, но контригра блока/материала отменена.
- **Рекомендации:** **P0** разделить direct hit/explosion/fire tick, запретить мгновенное уничтожение иммунных материалов и провести Core damage через единый pipeline. **P1** реализовать заявленный burn/block model. **P2** синхронизировать telegraph и фактический radius.

### Принять разрушение

- **Заявлено:** при смертельном ударе Core остаётся на 20 HP, Радон умирает на 7 с, идёт мощная волна; после разрушения Core — AoE damage/knockback с шансом blue fire; cooldown 20/40 ([HeroSystem.cpp:26–31](../src/HeroSystem.cpp#L26-L31)).
- **Фактически:** при живом Core нажатие лишь toggles primed без расхода; локальный melee-break path при смертельном уроне вызывает sacrifice, ставит 20 HP, убивает Радона на 7 с и даёт knockback-wave. При мёртвом Core сразу тратится заряд, запускается 40-секундный cooldown и AoE 28 damage/knockback ([Hero/HeroAbilities.cpp:423–456](../src/Hero/HeroAbilities.cpp#L423-L456), [Hero/HeroAbilities.cpp:600–671](../src/Hero/HeroAbilities.cpp#L600-L671)).
- **Работает:** human mining destruction path, 20 HP, 7 s respawn, 20 s cooldown, enemy-only wave, live/destroyed branches и strong feedback.
- **Дефекты:** bot assault и projectile Core damage обходят `TryRadonCoreSacrifice`; destroyed-Core AoE никогда не создаёт blue fire. Волна не проверяет стены. Primed не очищается `ClearHeroActiveEffects`, то есть переживает обычную смерть/respawn ([Player.cpp:620–629](../src/Player.cpp#L620-L629)).
- **Расхождение:** центральная механика работает только для одного attacker path; post-destruction fire отсутствует.
- **Спорные взаимодействия:** sudden death намеренно уничтожает Core без hero-intercept — это корректное исключение, явно зафиксированное кодом ([Game.cpp:961–983](../src/Game.cpp#L961-L983)). При двух Радонах выигрывает первый по порядку `players_` ([Hero/HeroAbilities.cpp:609–639](../src/Hero/HeroAbilities.cpp#L609-L639)).
- **AI:** хорошо праймит при low HP/critical defense и использует post-Core AoE рядом с врагом, но его же bot core assault не даёт вражескому Радону сработать ([GameBotAI.cpp:3137–3155](../src/GameBotAI.cpp#L3137-L3155)).
- **Feedback:** primed Core и Радон имеют persistent visual; sacrifice даёт ring/camera shake/event. Bot priming cast-ring скрыт, но persistent Core outline остаётся ([Renderer.cpp:1562–1589](../src/Renderer.cpp#L1562-L1589)).
- **Баланс и fun:** полезность **5**, надёжность **2**, читаемость **4**, уникальность **5**, мастерство **4**, контригра **4**, эмоция **5**, образ **5**, удовольствие **5**. Это лучший dramatic promise проекта: решение праймить заранее, цена собственной жизни и окно добивания 20 HP создают память; техническая ненадёжность сейчас разрушает доверие.
- **Рекомендации:** **P0** единая точка fatal Core transition для human/bot/projectile с осознанным исключением sudden death. **P1** blue-fire шанс/правило и LOS для wave. **P2** явно показать, кто из нескольких Радонов является активным перехватчиком.

## Орбита

### Дэш вперёд

- **Заявлено:** charge-to-distance до 5/2 блоков; overcharge срывает dash и удлиняет cooldown; повторный air dash запрещён до касания земли ([HeroSystem.cpp:38–42](../src/HeroSystem.cpp#L38-L42)).
- **Фактически:** одно нажатие мгновенно даёт фиксированный impulse 9,4 по горизонтали и lift 2,55, ставит 12 s cooldown, air-lock, momentum strike и +7 ult charge ([Hero/HeroAbilities.cpp:675–720](../src/Hero/HeroAbilities.cpp#L675-L720), [Hero/HeroAbilities.cpp:752–779](../src/Hero/HeroAbilities.cpp#L752-L779)).
- **Работает:** мобильность, направление камеры/AI, cooldown, first-air-use/lock state, momentum combo.
- **Дефекты:** нет hold input, шкалы charge, overcharge, cancel/failure и penalty cooldown. Фиксированный dash может отправить игрока в void без destination/risk preview.
- **Расхождение:** механика существует, но решение игрока полностью другое: timing button вместо управления силой.
- **Спорные взаимодействия:** landing сбрасывает air-lock; momentum также включается от обычной высокой скорости и держится до успешного удара ([GameWorldTick.cpp:724–745](../src/GameWorldTick.cpp#L724-L745)).
- **AI:** хорошо использует для gap-close/retreat и не кастует на земле без цели; edge safety перед dash не проверяет ([GameBotAI.cpp:3193–3213](../src/GameBotAI.cpp#L3193-L3213)).
- **Feedback:** trail/ring, floating text, body/first-person pulse есть у игрока; bot cast trail подавлен. Нет charge telegraph, потому что charge отсутствует ([Renderer.cpp:1680–1693](../src/Renderer.cpp#L1680-L1693), [Renderer.cpp:2051–2070](../src/Renderer.cpp#L2051-L2070)).
- **Баланс и fun:** полезность **4**, надёжность **4**, читаемость **3**, уникальность **2**, мастерство **2**, контригра **3**, эмоция **3**, образ **4**, удовольствие **3**. Сейчас это обычный mobility item; обещанная игра «додержать/передержать» дала бы уникальное мастерство и риск.
- **Рекомендации:** **P1** реализовать hold/overcharge contract либо отдельно переутвердить дизайн. **P2** edge-risk cue и bot safety. **P3** speed lines/charge audio.

### Фантомные блоки

- **Заявлено:** до 8 блоков на 3 с, постепенно разрушаются, слабее обычных, не ставятся у enemy Core и не могут полностью запереть игрока ([HeroSystem.cpp:43–47](../src/HeroSystem.cpp#L43-L47)).
- **Фактически:** линия до 8 обычных breakable `EnergyGlassBlock` на уровне under-feet; блоки удаляются одновременно по таймеру. Есть world bounds, overlap и enemy-Core restriction ([Hero/HeroAbilities.cpp:782–848](../src/Hero/HeroAbilities.cpp#L782-L848), [GameWorldTick.cpp:816–848](../src/GameWorldTick.cpp#L816-L848)).
- **Работает:** частичная постановка, invalid-all rejection без расхода, 3 s lifetime, до 8, overlap игрока и Core restriction.
- **Дефекты:** обычная прочность энергостекла 1,65 s, нет постепенного decay и anti-entrapment-проверки ([Block.cpp:167–172](../src/Block.cpp#L167-L172)). Линия не адаптируется по высоте/опоре и может образовать неудобную стену вместо моста.
- **Расхождение:** реализовано частично; исчезновение мгновенное, защита сильнее заявленной.
- **Спорные взаимодействия:** если блок сломан/заменён раньше, таймер безопасно перестаёт владеть позицией. Одинаковые Орбиты могут ставить независимые линии; teamId/type недостаточно различает владельца, но замена предотвращает удаление не-фантомного блока.
- **AI:** использует только при assault, ≤4 обычных блоков и void впереди; не проверяет, хватит ли 3 секунд пройти мост ([GameBotAI.cpp:3215–3228](../src/GameBotAI.cpp#L3215-L3228)).
- **Feedback:** блоки видны как энергостекло, но не имеют lifetime indicator/fade; bot placement sparks подавлены.
- **Баланс и fun:** полезность **4**, надёжность **3**, читаемость **3**, уникальность **4**, мастерство **4**, контригра **3**, эмоция **4**, образ **5**, удовольствие **4**. Хорошее решение «мост или временная стенка», но full-strength стекло даёт слишком бесплатную защиту.
- **Рекомендации:** **P1** отдельная phantom durability/anti-lock rule. **P2** fade за 0,8 s и таймер поверхности. **P3** распад по блокам волной вместо одновременного исчезновения.

### Видимый телепорт

- **Заявлено:** teleport в выбранную видимую доступную точку ограниченной дальности с self-damage по дистанции; нельзя через закрытые блоки/в чужую защиту ([HeroSystem.cpp:48–53](../src/HeroSystem.cpp#L48-L53)).
- **Фактически:** raycast до 20, destination перед первой стеной/на поверхности, AABB/map/height/enemy-Core checks, cost 4–28 по ray distance; затем immediate teleport и self-damage ([Hero/HeroAbilities.cpp:851–943](../src/Hero/HeroAbilities.cpp#L851-L943), [Hero/HeroAbilities.cpp:946–1018](../src/Hero/HeroAbilities.cpp#L946-L1018)).
- **Работает:** invalid target не расходует ульту, wall/core/AABB checks, distance cost, bot/player общий path, health feedback.
- **Дефекты:** safety не проверяет ground/support и void; no-hit ray разрешает destination на 20 блоках при той же высоте, даже над пустотой. Self-damage может убить — это допустимый риск, но confirmation нет.
- **Расхождение:** «доступная» точка реализована только как незанятый AABB, не как безопасная опора.
- **Спорные взаимодействия:** preview строится в тот же keypress непосредственно перед `UseHeroAbility`; commit уже совершён до первого render кадра preview. Полоска стоимости поэтому объясняет результат постфактум, а не помогает выбрать ([Hero/HeroAbilities.cpp:164–175](../src/Hero/HeroAbilities.cpp#L164-L175), [Renderer.cpp:2637–2662](../src/Renderer.cpp#L2637-L2662)).
- **AI:** использует как retreat при 18<HP<40, но face-to-home ray может упереться в ближайшую стену или выбрать unsupported point; retry безопасно не тратит charge ([GameBotAI.cpp:3180–3191](../src/GameBotAI.cpp#L3180-L3191)).
- **Feedback:** луч, valid/invalid цвет, destination marker и HP cost качественны, но приходят слишком поздно. Старт/end rings и звук сильные; bot rings скрыты.
- **Баланс и fun:** полезность **5**, надёжность **3**, читаемость **3**, уникальность **5**, мастерство **4**, контригра **4**, эмоция **5**, образ **5**, удовольствие **4**. Цена HP делает teleport не бесплатным; противник может давить predicted exit. Главный missing decision — preview/confirm.
- **Рекомендации:** **P1** support/void safety и двухфазный aim-confirm. **P2** оставить возможность рискованного void teleport только как явно красный override. **P3** enemy-visible wind-up/exit signature.

## Бром

### Робот-пылесос

- **Заявлено:** 48 iron, максимум два, медленно собирает и несёт в team chest, capacity 24 с весом gold/crystal ([HeroSystem.cpp:60–64](../src/HeroSystem.cpp#L60-L64)).
- **Фактически:** все перечисленные правила реализованы: limit/resource validation до spawn, weighted cargo 1/2/3, поиск pickup, grounded step/drop navigation, return/delivery и charge owner ([Hero/HeroAbilities.cpp:1233–1274](../src/Hero/HeroAbilities.cpp#L1233-L1274), [GameWorldTick.cpp:851–1110](../src/GameWorldTick.cpp#L851-L1110)).
- **Работает:** invalid resource/limit не расходует; permanent active не исчезает по cooldown; союзники не конкурируют за owner-limit; доставка идёт в team chest.
- **Дефекты:** подтверждённого функционального дефекта относительно active-description нет. Риск: spawn offset не проверяется на стену/опору, а локальный greedy mover не строит path и может застрять; automatch не предоставляет device-delivery counter для подтверждения частоты.
- **Расхождение:** нет подтверждённого расхождения active-description.
- **Спорные взаимодействия:** два Брома имеют по два робота; pickup выигрывает тот device, который обновился первым. Порядок игроков ротируется, но порядок device vector нет.
- **AI:** создаёт только при iron surplus ≥60, у базы и без врага — осмысленная экономическая эвристика ([GameBotAI.cpp:3249–3256](../src/GameBotAI.cpp#L3249-L3256)).
- **Feedback:** модель, cargo gauge, returning line и delivery message хороши; team relation цветом не показан, lifetime для permanent ожидаемо отсутствует.
- **Баланс и fun:** полезность **4**, надёжность **4**, читаемость **4**, уникальность **5**, мастерство **3**, контригра **2**, эмоция **3**, образ **5**, удовольствие **4**. Решение — инвестировать 48 iron в экономику вместо моста/защиты; риск — медленная окупаемость. Однако враг не может взаимодействовать с роботом, поэтому контригра только через кражу pickup/давление на владельца.
- **Рекомендации:** **P2** path-stall recovery и team tint. **P3** доставка/окупаемость в статистике. Если дизайн позже потребует уничтожаемость active-device — добавить HP как отдельное изменение, не считать текущий текст нарушенным.

### Дрон-турель

- **Заявлено:** 12 gold, максимум один, небольшой damage/средний knockback, LOS, не Core, быстро уничтожается ranged weapon ([HeroSystem.cpp:65–69](../src/HeroSystem.cpp#L65-L69)).
- **Фактически:** limit/cost, range 16, LOS, nearest target, 6 damage, knockback и 1,15 s cadence работают; Core не рассматривается. Struct не содержит health, а combat не target-ит devices ([Hero/HeroAbilities.cpp:1277–1319](../src/Hero/HeroAbilities.cpp#L1277-L1319), [Game.h:327–338](../src/Game.h#L327-L338), [GameWorldTick.cpp:1112–1220](../src/GameWorldTick.cpp#L1112-L1220)).
- **Работает:** invalid limit/resource не расходует, allies filtered, wall blocks shot, kill/damage credit.
- **Дефекты:** неуничтожаема и permanent. Нет placement safety. Turret damage по замаскированному Лихо не сбрасывает disguise, потому что идёт прямым `Damage`, а reveal выполняется только в combat-event path ([GameWorldTick.cpp:1180–1208](../src/GameWorldTick.cpp#L1180-L1208), [Game.cpp:2184–2189](../src/Game.cpp#L2184-L2189)).
- **Расхождение:** заявленная главная контригра отсутствует; «турели раскрывают Лихо» фактически лишь означает, что они игнорируют tint и стреляют по реальной команде.
- **Спорные взаимодействия:** limit per-player, не per-team: четыре Брома дают четыре permanent turrets. Target choice не учитывает disguise/угрозу владельцу.
- **AI:** ставит при defense или близком враге, сохраняет 8 gold вне defense; хорошая эвристика ([GameBotAI.cpp:3241–3247](../src/GameBotAI.cpp#L3241-L3247)).
- **Feedback:** модель, muzzle line, hit text есть; shots используют глобальный не spatial `PlayHit`, ally/enemy цвет одинаков.
- **Баланс и fun:** полезность **4**, надёжность **5**, читаемость **4**, уникальность **3**, мастерство **2**, контригра **1**, эмоция **3**, образ **5**, удовольствие **3**. Сейчас это бесплатная вечная зона контроля после оплаты; позиционное решение есть, но враг не может реализовать обещанный ответ.
- **Рекомендации:** **P1** HP/hitbox/ranged damage/removal и корректный reveal Лихо. **P2** team cap или diminishing overlap, team tint, spatial fire SFX.

### Сборка обороны

- **Заявлено:** всасывает nearby resources, использует inventory, число units зависит от ресурсов; units временные и уничтожаемые ([HeroSystem.cpp:70–75](../src/HeroSystem.cpp#L70-L75)).
- **Фактически:** radius 7 pickup transfer, затем до 2 vacuum и 3 turret по полным active-cost; lifetime 90 s и 70 s cooldown. Devices не имеют health ([Hero/HeroAbilities.cpp:1322–1377](../src/Hero/HeroAbilities.cpp#L1322-L1377)).
- **Работает:** количество масштабируется ресурсами, inventory cost, temporary cleanup, no-use при нуле spawned, эффект переживает смерть владельца и очищается на новом матче.
- **Дефекты:** pickup помечаются collected и добавляются в inventory **до** проверки, можно ли создать хоть один unit. При недостатке после всасывания метод возвращает false; charge/cooldown не тратятся, но ресурсы остаются — бесплатный повторяемый vacuum ([Hero/HeroAbilities.cpp:1324–1345](../src/Hero/HeroAbilities.cpp#L1324-L1345)). Units нельзя уничтожить.
- **Расхождение:** временность соблюдена, уничтожаемость нет; invalid cast мутирует мир.
- **Спорные взаимодействия:** до пяти units на игрока, без team cap; несколько Бромов дают непрозрачный turret swarm. Владелец может умереть, а defense работает дальше — текст этого не запрещает, но balance-risk высок.
- **AI:** разумно ждёт defense/critical и near-home, однако не проверяет, хватит ли ресурсов; может пользоваться тем же free-vacuum side effect ([GameBotAI.cpp:3234–3239](../src/GameBotAI.cpp#L3234-L3239)).
- **Feedback:** большой ring/message и distinct temporary outline; bot activation скрыта, units появляются без enemy-readable wind-up.
- **Баланс и fun:** полезность **5**, надёжность **4**, читаемость **3**, уникальность **4**, мастерство **2**, контригра **1**, эмоция **4**, образ **5**, удовольствие **3**. Решение ресурсов интересное, но indestructible swarm и fail-side-effect превращают ульту в слишком надёжную ценность.
- **Рекомендации:** **P0** transactional validation/rollback pickup absorption. **P1** destructible units. **P2** cap/telegraph/team relation. **P3** сборочная анимация вместо мгновенного появления.

## Конвой

### Капкан

- **Заявлено:** до двух ловушек на поверхности на 45 s; враг сильно slow и хуже прыгает; trap заметен, ломается и обходится ([HeroSystem.cpp:82–86](../src/HeroSystem.cpp#L82-L86)).
- **Фактически:** surface/limit validation и lifetime работают. При enemy overlap trap показывает effect/message и сразу удаляется, но не вызывает ни одного метода target ([Hero/HeroAbilities.cpp:1444–1507](../src/Hero/HeroAbilities.cpp#L1444-L1507), [GameWorldTick.cpp:1238–1287](../src/GameWorldTick.cpp#L1238-L1287)).
- **Работает:** постановка, no-surface rejection без cooldown, enemy-only trigger, lifetime/limit.
- **Дефекты:** нет slow, jump penalty, damage, stun или status; нет health/hitbox и enemy break. Сам message признаёт, что ломание — «следующий этап» ([Hero/HeroAbilities.cpp:1500–1506](../src/Hero/HeroAbilities.cpp#L1500-L1506)).
- **Расхождение:** основная механика отсутствует; визуальный trigger не даёт gameplay value.
- **Спорные взаимодействия:** несколько trap могут стоять рядом, но первый сработавший удаляется; allies безопасны. Смерть владельца trap не очищает.
- **AI:** Defender ставит у базы в тишине — логика разумна, но тратит способность впустую ([GameBotAI.cpp:3279–3286](../src/GameBotAI.cpp#L3279-L3286)).
- **Feedback:** trap видим всем и flash срабатывания читаем, но визуал ложно обещает effect; team relation отсутствует.
- **Баланс и fun:** полезность **1**, надёжность **1**, читаемость **2**, уникальность **2**, мастерство **1**, контригра **1**, эмоция **1**, образ **4**, удовольствие **1**. Решение поставить на route есть, но награды за чтение маршрута нет.
- **Рекомендации:** **P0** применить slow/jump debuff и destructible state. **P2** status icon/duration, trap team tint и hit feedback.

### Наручники

- **Заявлено:** target в радиусе 6 не может сильно увеличить дистанцию; tether рвётся от damage Конвою, большого падения или fireball ([HeroSystem.cpp:87–91](../src/HeroSystem.cpp#L87-L91)).
- **Фактически:** автоматически выбирается nearby enemy, проверяется LOS, создаётся 12 s visual tether. При distance >6 только flash; позиция/velocity не изменяются, tether не рвётся ([Hero/HeroAbilities.cpp:1510–1554](../src/Hero/HeroAbilities.cpp#L1510-L1554), [GameWorldTick.cpp:1289–1321](../src/GameWorldTick.cpp#L1289-L1321)).
- **Работает:** no-target/wall rejection без расхода, enemy filtering, lifetime, cleanup при смерти owner/target.
- **Дефекты:** нет удержания и всех трёх break conditions. Player не выбирает crosshair target — helper выбирает цель по score в радиусе, затем лишь проверяет стену ([GameBotAI.cpp:5560–5593](../src/GameBotAI.cpp#L5560-L5593)).
- **Расхождение:** механика отсутствует; сообщение прямо говорит, что полная механика будет добавлена позже ([Hero/HeroAbilities.cpp:1549–1553](../src/Hero/HeroAbilities.cpp#L1549-L1553)).
- **Спорные взаимодействия:** несколько Конвоев могут tether одну цель; один Конвой может создать несколько tether после cooldown, cleanup независим. Стена после каста не разрывает связь.
- **AI:** корректно ждёт fighting target <5,5 и face-ит его, но получает нулевую gameplay value ([GameBotAI.cpp:3271–3277](../src/GameBotAI.cpp#L3271-L3277)).
- **Feedback:** линия и endpoints читаемы, duration fraction вычисляется, но HUD duration только владельцу; ally/enemy tint нет.
- **Баланс и fun:** полезность **1**, надёжность **1**, читаемость **3**, уникальность **3**, мастерство **1**, контригра **1**, эмоция **1**, образ **5**, удовольствие **1**. Визуально обещает duel, фактически не меняет решение ни одного игрока.
- **Рекомендации:** **P0** constraint + explicit break events. **P1** aim/cone target selection. **P2** tension meter и причины разрыва.

### Купол содержания

- **Заявлено:** destructible 30 s prison; враги не выходят, slow и медленнее атакуют ([HeroSystem.cpp:92–97](../src/HeroSystem.cpp#L92-L97)).
- **Фактически:** создаётся 30 s visual sphere/ring radius 6. Tick только обновляет lifetime и рисует pulse; HP/collision/status отсутствуют ([Hero/HeroAbilities.cpp:1557–1573](../src/Hero/HeroAbilities.cpp#L1557-L1573), [GameWorldTick.cpp:1323–1342](../src/GameWorldTick.cpp#L1323-L1342)).
- **Работает:** только lifetime, визуал и cooldown/charge consumption — если charge искусственно установлен.
- **Дефекты:** нет выхода/стены, slow, attack slow, HP, destruction. Более того, штатных источников charge Конвоя нет, поэтому игрок и AI никогда не получат `ultimateReady` ([Player.cpp:568–588](../src/Player.cpp#L568-L588)).
- **Расхождение:** вся gameplay-механика отсутствует; event message называет это «каркасом зоны» ([Hero/HeroAbilities.cpp:1569–1572](../src/Hero/HeroAbilities.cpp#L1569-L1572)).
- **Спорные взаимодействия:** dome не различает allies/enemies, потому что никого не обрабатывает; несколько dome лишь добавляют визуальный шум. Смерть владельца не удаляет.
- **AI:** имеет хорошую эвристику commit-kill/defense, но readiness никогда не наступает ([GameBotAI.cpp:3261–3269](../src/GameBotAI.cpp#L3261-L3269)).
- **Feedback:** сфера читается и пульсирует, но создаёт ложную границу; нет HP/status/inside warning. Bot cast suppressed.
- **Баланс и fun:** полезность **1**, надёжность **1**, читаемость **2**, уникальность **3**, мастерство **1**, контригра **1**, эмоция **2**, образ **5**, удовольствие **1**. Потенциально сильная арена для duel, сейчас — декоративная кнопка, которую нельзя зарядить.
- **Рекомендации:** **P0** реализовать charge source и minimum viable prison/status/HP. **P1** collision/ejection rules у стены/void. **P2** team tint, HP ring, enemy warning. До этого не балансировать числовые cooldown/duration.

## Лихо

### Тихие шаги

- **Заявлено:** несколько секунд тишины; первый backstab даёт bonus; miss/non-backstab снимает bonus ([HeroSystem.cpp:104–108](../src/HeroSystem.cpp#L104-L108)).
- **Фактически:** 6 s active; первый зарегистрированный combat hit вычисляет backstab и всегда снимает active, bonus = max(3, half event damage). В audio system вообще нет footstep cue ([Hero/HeroAbilities.cpp:260–305](../src/Hero/HeroAbilities.cpp#L260-L305), [Game.cpp:2137–2157](../src/Game.cpp#L2137-L2157), [AudioSystem.cpp:63–74](../src/AudioSystem.cpp#L63-L74)).
- **Работает:** backstab orientation, bonus damage, charge +14, duration/cooldown, non-backstab hit снимает bonus.
- **Дефекты:** attack miss не создаёт combat event и не снимает bonus; block/core/ranged projectile action также не проходит этот branch. «Тишина» не меняет слышимость, потому что footsteps отсутствуют для всех.
- **Расхождение:** damage-бонус частичный; stealth-часть неотличима от базового состояния.
- **Спорные взаимодействия:** смерть очищает active; две Лихо независимы. Hit в спину по уже смертельно раненой цели может начислить charge до death handling, но дополнительного kill event нет.
- **AI:** кастует перед siege/chase, но face-ит enemy и не планирует обход за спину — backstab возникает случайно ([GameBotAI.cpp:3291–3298](../src/GameBotAI.cpp#L3291-L3298)).
- **Feedback:** trail/floating text при касте и backstab text; жертва не получает явного pre-hit cue (что уместно для stealth). Bot cast VFX скрыт.
- **Баланс и fun:** полезность **3**, надёжность **3**, читаемость **3**, уникальность **3**, мастерство **4**, контригра **3**, эмоция **4**, образ **5**, удовольствие **3**. Решение маршрута и угла хорошее, но бесплатное сохранение после miss снижает риск.
- **Рекомендации:** **P1** consume на attack attempt и определить реальную stealth-информацию (footstep/minimap). **P2** AI flank intent; тонкий victim cue только при раскрытии.

### Кровотечение

- **Заявлено:** attacks дают player DoT; blocks получают «надрез» и усиление последующих атак; после нескольких hits по одной цели — сильный bonus damage ([HeroSystem.cpp:109–113](../src/HeroSystem.cpp#L109-L113)).
- **Фактически:** 6 s buff. Player hits создают/обновляют per-owner bleed до 4 stacks, нанося 2+min(3, stacks) раз в секунду 6 s. На block пока active лишь умножает текущий break speed на 0,72; persistent cut и burst отсутствуют ([Game.cpp:2158–2175](../src/Game.cpp#L2158-L2175), [GameWorldTick.cpp:1345–1371](../src/GameWorldTick.cpp#L1345-L1371), [GamePlacement.cpp:371–389](../src/GamePlacement.cpp#L371-L389)).
- **Работает:** enemy DoT, stacking per Лихо/target, damage credit, active duration/cooldown, temporary block-breaking bonus.
- **Дефекты:** нет promised finishing burst; block modifier не является состоянием target и действует на любой блок только пока buff активен. Bleed object не удаляется при смерти target/owner; target respawn через 3 s может снова получить тики того же bleed после 1,65 s invulnerability ([Player.cpp:632–656](../src/Player.cpp#L632-L656)).
- **Расхождение:** player half частичен, block half работает иначе, capstone отсутствует.
- **Спорные взаимодействия:** одинаковые Лихо накладывают независимые DoT; один owner обновляет lifetime/stacks. Несколько simultaneous bleeds суммируются без global cap.
- **AI:** включает в melee <3,5, что надёжно, но не планирует удержание stacks/finish ([GameBotAI.cpp:3299–3305](../src/GameBotAI.cpp#L3299-L3305)).
- **Feedback:** floating damage на каждом tick, но нет stack/status/duration icon, block cut decal или burst telegraph.
- **Баланс и fun:** полезность **4**, надёжность **4**, читаемость **2**, уникальность **3**, мастерство **3**, контригра **2**, эмоция **3**, образ **5**, удовольствие **3**. Сейчас это в основном бесплатный DoT steroid; promised focus-target payoff дал бы decision и контригру disengage/cleanse.
- **Рекомендации:** **P0** очистить/инвалидировать bleed на death/respawn. **P1** persistent block cut и stack burst. **P2** stack/duration UI и distinct bleed hit SFX.

### Искажённая маскировка

- **Заявлено:** модель чужой команды с заметными glitches; снимается атакой, damage и близостью к Core; scanner/Эхо/turret/defense раскрывают ([HeroSystem.cpp:114–119](../src/HeroSystem.cpp#L114-L119)).
- **Фактически:** выбирается первый active enemy team, на 20 s меняется team color и добавляется зелёный glitch tint; actual Likho model/hero silhouette сохраняется. В радиусе 7 от enemy Core снимается. Combat-event attack/hit снимает, direct projectile/DoT/device damage — нет ([Hero/HeroAbilities.cpp:286–300](../src/Hero/HeroAbilities.cpp#L286-L300), [Renderer.cpp:1619–1626](../src/Renderer.cpp#L1619-L1626), [Renderer.cpp:1695–1698](../src/Renderer.cpp#L1695-L1698), [GameWorldTick.cpp:760–782](../src/GameWorldTick.cpp#L760-L782)).
- **Работает:** duration/cooldown/charge, enemy tint, visible glitch, melee combat reveal, Core reveal, death cleanup.
- **Дефекты:** bow/blaster/Molotov attack и block/Core attack не снимают disguise; fire/bleed/turret/echo damage не раскрывает. Эхо/turret не имеют explicit reveal — они только используют real team for targeting. Нет выбора/копирования enemy model.
- **Расхождение:** визуал и reveal реализованы частично; описание сильнее факта.
- **Спорные взаимодействия:** для союзника Лихо цвет тоже становится enemy color, team relation не подписана; Свидетель рисует одинаковую sphere-wire вокруг всех enemies, не distorted silhouette.
- **AI:** использует только при assault и <30 блоков до Core, что тематично; не отменяет disguise перед невыгодной атакой и не понимает scanners ([GameBotAI.cpp:3306–3313](../src/GameBotAI.cpp#L3306-L3313)).
- **Feedback:** third-person tint/glitch читаем, first-person почти не показывает состояние кроме HUD timer; reveal от Core message только local owner. Противник может распознать неизменившийся силуэт Лихо слишком легко.
- **Баланс и fun:** полезность **3**, надёжность **2**, читаемость **3**, уникальность **5**, мастерство **4**, контригра **3**, эмоция **4**, образ **5**, удовольствие **3**. Отличный social-deception promise, но team tint без model/behavior disguise не создаёт достаточно правдоподобных моментов.
- **Рекомендации:** **P1** единый reveal-on-offensive-action/damage pipeline и explicit scanner reveal. **P2** copy enemy-team silhouette/gear с намеренным glitch, ally marker, first-person status. **P3** brief reveal stinger.

## Свидетель

### Усиленное Эхо

- **Заявлено:** low-HP destructible echo, ranged small damage, отвлечение и информация ([HeroSystem.cpp:126–130](../src/HeroSystem.cpp#L126-L130)).
- **Фактически:** stationary 24 s echo; каждые 1,35 s ищет ближайшего enemy в радиусе 10, без LOS, мгновенно наносит 5 damage и +3 charge владельцу. Struct не содержит HP ([Hero/HeroAbilities.cpp:374–382](../src/Hero/HeroAbilities.cpp#L374-L382), [Game.h:372–381](../src/Game.h#L372-L381), [GameWorldTick.cpp:1390–1435](../src/GameWorldTick.cpp#L1390-L1435)).
- **Работает:** ranged damage, enemy filtering, lifetime, target beam, damage credit, info-through-flash.
- **Дефекты:** нельзя уничтожить/отвлечь как combat target; обнаруживает и стреляет через стены; attack instant без projectile. Нет cap active echoes, кроме 24 s/cooldown.
- **Расхождение:** damage/info есть, обещанная хрупкость и контригра отсутствуют.
- **Спорные взаимодействия:** переживает смерть владельца; одинаковые Свидетели стакают independent fire. Disguise Лихо игнорируется real-team targeter и не снимается от echo damage.
- **AI:** ставит при entering fight <8; хорошая эвристика, но получает wallhack/damage без риска ([GameBotAI.cpp:3318–3325](../src/GameBotAI.cpp#L3318-L3325)).
- **Feedback:** видимая echo-модель и beam на shot; no HP bar/team tint/spatial shot sound. Bot spawn VFX suppressed, но device появляется.
- **Баланс и fun:** полезность **5**, надёжность **5**, читаемость **3**, уникальность **4**, мастерство **2**, контригра **1**, эмоция **3**, образ **5**, удовольствие **3**. Сейчас это бесплатная indestructible turret с wallhack; placement decision есть, но ответ противника отсутствует.
- **Рекомендации:** **P1** HP/hitbox/LOS/projectile и destruction reward. **P2** team tint, detection cone/beam, HP/lifetime feedback; решить team/global cap.

### Фазовый участок

- **Заявлено:** выбранные blocks становятся passable; внутри slow; при завершении eject, иначе suffocation; Core-touching/near-Core запрещены ([HeroSystem.cpp:131–135](../src/HeroSystem.cpp#L131-L135)).
- **Фактически:** raycast 5,5, до 3×2 breakable blocks в плоскости X/Y удаляются на 4 s и потом force-placed обратно; radius-near-Core check <12 sq работает ([Hero/HeroAbilities.cpp:322–371](../src/Hero/HeroAbilities.cpp#L322-L371), [GameWorldTick.cpp:1374–1388](../src/GameWorldTick.cpp#L1374-L1388)).
- **Работает:** invalid target/near-Core rejection без cooldown, temporary passage, original block restore, match cleanup.
- **Дефекты:** нет slow/eject/suffocation/occupancy test. Restore с `force=true` может вернуть solid block в player. Если позицию занял другой block, запись с expired timer остаётся до первого момента air и затем неожиданно восстанавливает старый block.
- **Расхождение:** core phase работает, safety contract отсутствует.
- **Спорные взаимодействия:** selection orientation не учитывает hit normal (всегда x/y при fixed z), поэтому на стенах другой ориентации area выглядит непоследовательно. Удаляются и friendly, и enemy breakable blocks.
- **AI:** пробует только после stuck >1,4, но не проверяет ray target/near-Core и может retry каждые 0,6 s; при успехе pathfinding не знает 4 s deadline ([GameBotAI.cpp:3326–3330](../src/GameBotAI.cpp#L3326-L3330)).
- **Feedback:** исчезновение/возврат видно, но нет translucent phase volume, timer, inside warning или eject cue; bot cast sparks suppressed.
- **Баланс и fun:** полезность **4**, надёжность **2**, читаемость **2**, уникальность **5**, мастерство **4**, контригра **3**, эмоция **4**, образ **5**, удовольствие **4**. Проход через оборону создаёт сильные истории, но unsafe restore превращает mastery в непредсказуемость.
- **Рекомендации:** **P0** occupancy-safe restore/eject/suffocation. **P1** orient area by surface normal и добавить slow volume. **P2** countdown/translucent ghost blocks.

### Контуры Войда

- **Заявлено:** 15 s contours enemies/resources/Cores; enemies pulses; disguise/hidden — distorted silhouettes ([HeroSystem.cpp:136–140](../src/HeroSystem.cpp#L136-L140)).
- **Фактически:** active flag заставляет renderer рисовать wire spheres вокруг generators, живых Cores и enemies. Pickups/dropped resources не обводятся; Лихо не отличается; pulse для enemies отсутствует ([Renderer.cpp:1438–1440](../src/Renderer.cpp#L1438-L1440), [Renderer.cpp:1542–1549](../src/Renderer.cpp#L1542-L1549), [Renderer.cpp:1581–1584](../src/Renderer.cpp#L1581-L1584), [Renderer.cpp:1699–1702](../src/Renderer.cpp#L1699-L1702)).
- **Работает:** duration/cooldown/charge consumption; генераторы/Core/enemies получают видимый wire overlay в обоих camera modes.
- **Дефекты:** wires рисуются в обычном 3D depth pass, не отдельным through-wall/outline pass; за solid geometry они скрываются. Нет pickups, enemy pulse, distorted disguise, distance policy.
- **Расхождение:** реализована слабая видимая подсветка уже видимых объектов, а не обещанная разведывательная ульта.
- **Спорные взаимодействия:** уничтоженные Core пропускаются; это логично, но описание не уточняет. First-person и third-person используют один local-player active flag, поэтому правило одинаково.
- **AI:** кастует для chase/после 150 s, но AI perception/path selection вообще не читает этот active flag — бот получает нулевую информационную пользу, только рендер для человека-наблюдателя ([GameBotAI.cpp:3331–3336](../src/GameBotAI.cpp#L3331-L3336)).
- **Feedback:** owner видит overlay, враг не получает telegraph глобальной разведки; нет audio signature. Reduced flashes не меняет wires.
- **Баланс и fun:** полезность **2**, надёжность **2**, читаемость **3**, уникальность **3**, мастерство **1**, контригра **2**, эмоция **2**, образ **5**, удовольствие **2**. Сейчас это пассивный visual buff без решения после нажатия; promised pulse/read скрытых целей дал бы hunt window и counterplay через укрытие/ложные силуэты.
- **Рекомендации:** **P1** real outline/occlusion policy, pickups и distorted disguise; AI perception должен получать ту же информацию, не больше. **P2** pulsing cadence, enemy global cue и accessibility-safe shape coding.

## Пассивки и lifecycle, не вошедшие в счёт 18

- **Радон — Перегрузка Core:** 0,8 incoming в радиусе sqrt(105) от живого Core; после потери 1,2 incoming/1,2 outgoing по карте. Соответствует описанию ([GameWorldTick.cpp:705–722](../src/GameWorldTick.cpp#L705-L722)).
- **Орбита — Разгонный импульс:** включается от high horizontal speed/risky air, следующий melee hit получает усиленный knockback и +11 charge. В целом работает; состояние не тухнет после остановки, поэтому «после разгона» может храниться сколь угодно долго ([GameWorldTick.cpp:731–745](../src/GameWorldTick.cpp#L731-L745), [CombatSystem.cpp:611–622](../src/CombatSystem.cpp#L611-L622)).
- **Бром — Разбор трофеев:** enemy-team block даёт probabilistic iron/gold, crystal отсутствует. Проверки team есть; отдельного placerId у Block нет, но собственноручно поставленный нормальным путём block и так имеет team Брома и исключается ([Hero/HeroAbilities.cpp:1576–1609](../src/Hero/HeroAbilities.cpp#L1576-L1609)).
- **Конвой — Нарушитель:** полностью отсутствует; нет mark state, зон, damage/ability multiplier или charge source.
- **Лихо — Одинокий разрез:** 25% быстрее ломает non-Core block без ally в радиусе 8, после выхода из enemy-base получает speed 4 s; работает. Ultimate-charge description при этом реализован лишь частично ([GamePlacement.cpp:371–389](../src/GamePlacement.cpp#L371-L389), [GameWorldTick.cpp:760–795](../src/GameWorldTick.cpp#L760-L795)).
- **Свидетель — Эхо цикла:** death/kill echoes создаются на 12 s и обнаруживают enemies, но не имеют max count, HP, LOS или destruction path; поэтому пассивка частична ([Game.cpp:1454–1459](../src/Game.cpp#L1454-L1459), [Game.cpp:2191–2195](../src/Game.cpp#L2191-L2195), [GameWorldTick.cpp:1390–1438](../src/GameWorldTick.cpp#L1390-L1438)).

## Матрица обязательных граничных проверок

| Сценарий | Результат аудита |
|---|---|
| Без подходящей цели | Handcuffs/phase/phantom/device cost корректно reject до cooldown; Brom ultimate некорректно всасывает pickup до reject. Untargeted AoE/skillshots штатно тратятся на miss. |
| Вплотную к стене / через стену | Radon pulse, handcuffs, turret и teleport имеют LOS/collision. Echo игнорирует стены. Phantom/phase могут изменять сами стены по своим правилам. |
| Над пустотой / край острова | Phantom строит мост; trap требует поверхность; teleport не требует опору; dash и device spawn не имеют edge safety. |
| Союзники и враги в одной области | Player damage обычно enemy-only. Molotov ломает breakable blocks без team filter; phase удаляет blocks без team filter. |
| Живой / уничтоженный Core | Radon, Orbita restriction и Likho reveal ветвятся по `EnergyCore::IsAlive`. Projectile Core-destruction не проходит общий lifecycle. Sudden death намеренно обходит Радона. |
| Смерть во время эффекта | Player active flags очищаются. Global devices/zones/temporary blocks/echo/bleed продолжаются; tether удаляется при смерти участника. |
| Respawn до окончания | Likho bleed может снова бить тот же playerId после respawn; disguise/active buffs очищены; world objects сохраняются. |
| Повтор сразу после cooldown | Общий state допускает при zero; invalid targets обычно не стартуют cooldown. Brom actives zero-cooldown ограничены ресурсом/count. |
| Несколько simultaneous effects | Hazard/bleeds/devices стакаются; per-owner bleed объединяется, разные owners независимы; team cap у устройств нет. |
| Одинаковые герои | Radon sacrifice выигрывает первый в `players_`; Brom limits per-owner; Konvoy/Svidetel effects свободно стакаются. |
| Применение ботом | Все шесть имеют decision branch и общий cast path; activation VFX/SFX бота подавлены. Конвой-ульта readiness недостижима. |
| Завершение/перезапуск матча | `SetupMatch` очищает все hero vectors/state через пересоздание players. |
| Низкий/высокий FPS | Speed 1/256 fixed-step automatch детерминирован. Реальный FPS <20 замедляет симуляцию из-за cap dt; визуальная читаемость вручную не подтверждена. |
| First-person / third-person | Gameplay aim одинаков через camera direction. Только Радон/Орбита имеют специальные first-person hand effects; остальные зависят от world effect/HUD. Свидетель contours используют local state в обоих режимах. |

## Рекомендации по приоритетам

### P0 — ломает способность или матч

1. Сделать Molotov transactional и material-aware: убрать instant break обсидиана/стекла, direct/explosion/fire double-hit, friendly block deletion.
2. Провести все fatal Core transitions через единую проверку Radon sacrifice, кроме явно исключённого sudden death.
3. Исправить Brom ultimate reject-after-mutation.
4. Реализовать minimum viable gameplay трёх способностей Конвоя и источник charge; до этого не считать героя готовым.
5. Не допускать bleed через death/respawn и solid restore фазовых блоков внутрь игрока.

### P1 — заметно не так, как обещано

1. Orbita dash charge/overcharge и teleport aim-confirm/support safety.
2. Destructible HP/LOS/counterplay для Brom turret и обоих типов Echo.
3. Likho: attack-attempt consumption, persistent block cut/burst, единый disguise reveal pipeline.
4. Svidetel contours: настоящая политика occlusion, pickups, distorted hidden targets и одинаковая польза для AI/player.
5. Разделить suppression bot HUD от world VFX/SFX, чтобы касты AI были реактивно читаемы.

### P2 — читаемость и баланс

1. Team tint/shape для всех hero devices; HP/lifetime/status bars.
2. Stack/duration icons для bleed, trap, tether, dome и disguise; явные причины reject/break.
3. Telegraphed radii и landing previews для Molotov/teleport/dome; fade phantom blocks.
4. Team caps или diminishing returns для turret/echo/device stacking одинаковых героев.
5. Реальный low-FPS test harness с несколькими `dt`, отдельно от fixed-step automatch.

### P3 — polish и «вау»

1. Уникальные spatial SFX по героям вместо generic pickup/build/core-destroyed.
2. Сборочная анимация Брома, распад phantom wave, tether tension, dome cracking.
3. Sacrifice camera/audio event уровня матча и distinct blue-fire language.
4. First-person cast language для Брома, Конвоя, Лихо и Свидетеля.

## План следующих работ

1. **Минимальные исправления без изменения дизайна:** закрыть пять P0; добавить regression-сценарии fatal Core paths, Molotov materials/direct hit, respawn bleed, phase occupancy, Brom failed ult.
2. **Feedback:** отделить bot HUD suppression от world feedback; team tint, status duration/reject reasons, pre-commit teleport и accurate AoE telegraphs; проверить reduced flashes и обе камеры вручную.
3. **Баланс:** после функциональных исправлений запустить не менее 100 automatch с ability-use/hit/miss/device-value counters; только затем менять damage/cooldown/caps.
4. **Более смелые изменения скучных способностей:** восстановить charge-risk dash Орбиты; превратить dome в разрушаемую duel-arena; дать contours pulse-information loop; сделать disguise копированием правдоподобного силуэта, а не только tint.
5. **Кандидаты для будущего `IHeroKit/AbilityContext` (отдельная задача):** единый fatal-Core hook Радона; explicit `AbilityInputs.altModifier/aim`; общий destructible-device registry; status-effect ownership/lifecycle; team-aware feedback emitter. Текущий аудит не начинает этот рефактор.
