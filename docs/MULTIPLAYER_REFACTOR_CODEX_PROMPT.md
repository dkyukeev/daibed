# Prompt for Codex — DaiBed multiplayer architecture refactor

> Copy this prompt into a fresh Codex session when starting work on the
> multiplayer refactor. Pick exactly one task from the "First task" section unless
> the user explicitly asks for a different phase.

---

You are working in the DaiBed repository, a C++17/raylib voxel BedWars PvP game.
The project goal for this work is not merely "multiplayer works". The target is
fast, smooth, competitive multiplayer feel: responsive local input, stable
server authority, smooth remote players/entities, strong hit/pickup/action
feedback, and one gameplay pipeline shared by singleplayer and multiplayer.

Before coding, read these documents in this order:

1. `docs/MULTIPLAYER_REFACTOR_INDEX.md`
2. `docs/MULTIPLAYER_QUALITY_TARGET.md`
3. `docs/MULTIPLAYER_TARGET_ARCHITECTURE.md`
4. `docs/MULTIPLAYER_REFACTOR_PLAN.md`
5. `docs/NETWORK_PREP_PLAN.md`
6. `docs/VISION.md`

## North star

The architecture must move toward:

```text
Singleplayer = ClientFrontend -> LocalTransport -> LogicalServer
Multiplayer = ClientFrontend -> UdpTransport   -> LogicalServer
Loopback     = ClientFrontend -> LoopbackTransport -> LogicalServer
```

Singleplayer and multiplayer should differ by transport and latency, not by
separate gameplay code paths.

## Hard rules

- Do not do a big-bang rewrite.
- Keep the game buildable and playable after each step.
- Prefer small vertical slices with smoke tests.
- Do not move renderer/UI/audio into server-side code.
- Do not let client presentation mutate authoritative gameplay.
- Do not use `Player::IsLocal()` for gameplay physics or rules.
- Server receives intent, validates it, mutates truth, and replicates state/events.
- Client may predict and show feedback, but server owns damage, blocks, economy,
  inventory, cooldowns, death, respawn, pickup and victory.
- If a player-visible action exists, it needs either an explicit event/result or
  a reliable reconstruction path from snapshots.
- If a replicated entity lives longer than one tick, it needs a stable id before
  it is used for interpolation.

## Current context to verify

The code already has:

- `src/Network/PlayerCommand.h`
- `src/Network/NetworkSnapshot.h`
- UDP transport and loopback tests
- authoritative server path
- client prediction/reconciliation pieces
- remote interpolation pieces
- dynamic entity snapshot replication
- smoke tests such as:
  - `--protocol-smoke`
  - `--network-smoke`
  - `--network-actions-smoke`
  - `--network-ranged-smoke`
  - `--client-dynamic-apply-smoke`
  - `--client-input-smoke`
  - `--loopback-two-client-smoke`
  - `--mp-loopback-smoke`

Do not assume these are perfect; inspect the current code before changing it.

## First task

Start with Phase 1 from `docs/MULTIPLAYER_REFACTOR_PLAN.md`:

**Introduce explicit player control roles and movement parity.**

Goal:

- separate "has local camera", "human controlled", "bot controlled",
  "locally predicted" and "render replica";
- remove gameplay meaning from `Player::IsLocal()`;
- make human movement profile identical for:
  - singleplayer local human;
  - multiplayer predicted own player;
  - authoritative remote human on server;
- keep bot-only movement assistance explicitly bot-only.

Recommended implementation shape:

```cpp
enum class PlayerControlKind
{
    LocalHumanPredicted,
    RemoteHumanAuthoritative,
    BotAuthoritative,
    Replica,
    Spectator
};

bool IsHumanControlled(PlayerControlKind kind);
bool IsBotControlled(PlayerControlKind kind);
bool IsLocallyPredicted(PlayerControlKind kind);
bool HasLocalCamera(PlayerControlKind kind);
```

Do not blindly add this enum everywhere. First inspect existing ownership and
network player mapping:

- `src/Player.h`
- `src/Player.cpp`
- `src/Game.h`
- `src/Game.cpp`
- `src/GameNetwork.cpp`
- `src/GamePlayerActions.cpp`
- `src/GameWorldTick.cpp`
- `src/GameBotAI.cpp`

Find every gameplay-significant use of `IsLocal()` and categorize it:

- camera/UI/presentation: may remain local/camera-specific;
- human-vs-bot movement/rules: must move to explicit control kind/helper;
- prediction: must use locally-predicted helper;
- server authority: must not depend on local camera/UI.

## Acceptance criteria for the first task

Implement a narrow, testable slice:

1. Human movement no longer differs because a human is remote on the server.
2. Autostep and terrain/ice movement rules are explicit human/bot decisions.
3. Add or strengthen a parity smoke that applies the same `PlayerCommand` to a
   local human and a network/server human and checks movement stays equivalent.
4. Existing network smoke tests still pass.

Run at minimum:

```text
cmake --build build-release --config Release --parallel 4
build-release\Release\DaiBed.exe --protocol-smoke
build-release\Release\DaiBed.exe --network-smoke
build-release\Release\DaiBed.exe --network-actions-smoke
build-release\Release\DaiBed.exe --network-ranged-smoke
build-release\Release\DaiBed.exe --client-input-smoke
build-release\Release\DaiBed.exe --loopback-two-client-smoke
build-release\Release\DaiBed.exe --mp-loopback-smoke
```

If you change dynamic replication or client presentation, also run:

```text
build-release\Release\DaiBed.exe --client-dynamic-apply-smoke
```

## Working style

- Read code first; do not assume the docs are perfectly up to date.
- Keep edits scoped.
- Add tests proportional to risk.
- Do not revert unrelated user changes.
- If `src/GameNetwork.cpp` is untracked or dirty, work with it; do not reset it.
- Use existing patterns before creating new abstractions.
- Prefer a compatibility bridge over deleting old paths in the first pass.

## Deliverable

At the end, report:

- files changed;
- what behavior changed for players;
- what architecture boundary was improved;
- which tests were run and their result;
- what remains for the next session.

