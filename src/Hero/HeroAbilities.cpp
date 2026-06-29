#include "Game.h"
#include "HeroSystem.h"
#include "RangedCombat.h"
#include "VecConvert.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace
{
constexpr int kBuildMinY = -2;
constexpr int kBuildMaxY = 64;
constexpr int kBuildMapRadius = 72;
constexpr float kRadonSacrificeRespawnSeconds = 7.0f;
constexpr float kOrbitaDashDistance = 9.4f;
constexpr float kOrbitaDashLift = 0.72f;
constexpr float kOrbitaTeleportMaxDistance = 20.0f;
constexpr float kOrbitaEnemyCoreRestrictionSq = 16.0f;
constexpr float kOrbitaTeleportDamagePerBlock = 1.65f;
constexpr int kBromVacuumIronCost = 48;
constexpr int kBromTurretGoldCost = 12;
constexpr int kBromVacuumCapacity = 24;
constexpr float kBromUltimateCooldownSeconds = 70.0f;
constexpr float kBromUltimateDeviceLifetime = 90.0f;
constexpr int kKonvoyMaxTraps = 2;
constexpr float kKonvoyHandcuffRadius = 6.0f;
constexpr float kKonvoyDomeVisualRadius = 6.0f;
constexpr Vector3 kPlayerCollisionHalfExtents { 0.36f, 0.95f, 0.36f };

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float Length2D(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.z * value.z);
}

float Dot2D(Vector3 a, Vector3 b)
{
    return a.x * b.x + a.z * b.z;
}

Vector3 Normalize2D(Vector3 value)
{
    const float length = Length2D(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, 0.0f, value.z / length };
}

float Length(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vector3 Normalize(Vector3 value)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, value.y / length, value.z / length };
}

int ResourceIndex(ResourceType type)
{
    return static_cast<int>(type);
}

int BromCargoWeight(ResourceType type)
{
    switch (type)
    {
    case ResourceType::Iron:
        return 1;
    case ResourceType::Gold:
        return 2;
    case ResourceType::Crystal:
        return 3;
    }
    return 1;
}

int BromCargoUnits(const std::array<int, 3>& cargo)
{
    return cargo[ResourceIndex(ResourceType::Iron)] * BromCargoWeight(ResourceType::Iron)
        + cargo[ResourceIndex(ResourceType::Gold)] * BromCargoWeight(ResourceType::Gold)
        + cargo[ResourceIndex(ResourceType::Crystal)] * BromCargoWeight(ResourceType::Crystal);
}

float BromUltimateChargeForResource(ResourceType type, int amount)
{
    const float value = type == ResourceType::Iron ? 0.9f : (type == ResourceType::Gold ? 2.2f : 4.0f);
    return value * static_cast<float>(std::max(0, amount));
}

Vector3 OffsetAround(Vector3 center, int index, float radius)
{
    const float angle = static_cast<float>(index) * 2.3999632f;
    return Vector3 {
        center.x + std::cos(angle) * radius,
        center.y,
        center.z + std::sin(angle) * radius
    };
}

Color HeroAccentColor(HeroId id)
{
    switch (id)
    {
    case HeroId::Radon:
        return Color { 92, 164, 255, 255 };
    case HeroId::Orbita:
        return Color { 255, 96, 82, 255 };
    case HeroId::Brom:
        return Color { 96, 202, 118, 255 };
    case HeroId::Konvoy:
        return Color { 92, 210, 255, 255 };
    case HeroId::Likho:
        return Color { 104, 238, 92, 255 };
    case HeroId::Svidetel:
        return Color { 180, 104, 255, 255 };
    }
    return WHITE;
}

std::string FormatTenths(float value)
{
    const int tenths = static_cast<int>(value * 10.0f + 0.5f);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}
}

void Game::UseHeroAbilityInputs(Player& player)
{
    const PlayerCommand command = BuildLocalPlayerCommand();
    const PlayerControlKind controlKind = ControlKindForPlayer(player);
    if (HasLocalCamera(controlKind) && (shopOpen_ || inventoryOpen_))
    {
        return;
    }

    if (command.useAbility1)
    {
        UseHeroAbility(player, HeroAbilitySlot::Active1);
    }
    if (command.useAbility2)
    {
        UseHeroAbility(player, HeroAbilitySlot::Active2);
    }
    if (command.useUltimate)
    {
        if (HasLocalCamera(controlKind) && player.GetHeroId() == HeroId::Orbita)
        {
            orbitaTeleportPreview_ = BuildOrbitaTeleportPreview(player);
            orbitaTeleportPreviewTimer_ = orbitaTeleportPreview_.visible ? 0.28f : 0.0f;
        }
        else if (HasLocalCamera(controlKind))
        {
            orbitaTeleportPreviewTimer_ = 0.0f;
        }
        UseHeroAbility(player, HeroAbilitySlot::Ultimate);
    }
}

// Applies the per-tick action intents of a command (hero abilities) to a
// player. The single entry point the network smoke and the local input path
// share. Other actions (attack/break/place/utility) still live in their own
// command-driven methods — see docs/NETWORK_PREP_PLAN.md.
bool Game::ApplyPlayerActionCommand(Player& player, const PlayerCommand& command)
{
    const PlayerControlKind controlKind = ControlKindForPlayer(player);
    if (HasLocalCamera(controlKind) && (shopOpen_ || inventoryOpen_))
    {
        return false;
    }

    const auto applySlot = [this, &player](HeroAbilitySlot slot)
    {
        const HeroAbilityActionResult result = ApplyHeroAbilityAction(player, slot);
        if (result.handled)
        {
            return result.success;
        }

        ScopedLocalFeedbackSuppression suppressLegacyFeedback(*this, true);
        return UseHeroAbilityLegacy(player, slot);
    };

    bool used = false;
    if (command.useAbility1)
    {
        used = applySlot(HeroAbilitySlot::Active1) || used;
    }
    if (command.useAbility2)
    {
        used = applySlot(HeroAbilitySlot::Active2) || used;
    }
    if (command.useUltimate)
    {
        used = applySlot(HeroAbilitySlot::Ultimate) || used;
    }
    return used;
}

Game::HeroAbilityActionResult Game::ApplyHeroAbilityAction(Player& player, HeroAbilitySlot slot)
{
    HeroAbilityActionResult result {};
    result.hero = player.GetHeroId();
    result.slot = slot;

    if (!player.IsAlive() || player.IsEliminated())
    {
        result.handled = true;
        return result;
    }

    if (player.GetHeroId() == HeroId::Radon && slot != HeroAbilitySlot::Ultimate)
    {
        result.handled = true;
        const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Radon);
        const HeroAbilityDefinition& ability = slot == HeroAbilitySlot::Active1 ? hero.active1 : hero.active2;
        const HeroAbilityState& state = slot == HeroAbilitySlot::Active1
            ? player.GetHeroState().active1
            : player.GetHeroState().active2;
        const Color accent = HeroAccentColor(HeroId::Radon);
        result.color = accent;
        result.position = player.GetPosition();
        result.direction = player.Forward();

        if (state.cooldownRemaining > 0.0f)
        {
            result.message = hero.name + ": " + ability.name + " на кулдауне еще "
                + FormatTenths(state.cooldownRemaining) + " с.";
            result.messageSeconds = 1.7f;
            result.playDeniedSound = true;
            return result;
        }

        EnergyCore* core = FindCoreByTeam(player.GetTeamId());
        const bool coreAlive = core != nullptr && core->IsAlive();
        const bool hasLocalCamera = HasLocalCamera(ControlKindForPlayer(player));
        player.StartHeroAbilityCooldown(slot, ability.cooldownSeconds, ability.durationSeconds);

        if (slot == HeroAbilitySlot::Active1)
        {
            const bool pull = !coreAlive
                && hasLocalCamera
                && (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
            SetHeroAnimation(player, HeroAnimationState::WindUp, pull ? 0.44f : 0.40f);
            const Vector3 origin {
                player.GetPosition().x,
                player.GetPosition().y + 0.72f,
                player.GetPosition().z
            };
            Vector3 forward = hasLocalCamera ? cameraController_.GetFlatForward() : player.Forward();
            forward = Normalize2D(forward);
            if (Length2D(forward) <= 0.0001f)
            {
                forward = player.Forward();
            }

            int affected = 0;
            const Color pulseColor = pull ? Color { 92, 164, 255, 255 } : accent;
            result.worldEffects.push_back(HeroWorldEffectResult {
                origin,
                forward,
                pulseColor,
                pull ? 5.2f : 5.6f,
                0.34f,
                pull ? WorldEffectKind::Pull : WorldEffectKind::Cone,
                true });
            for (Player& target : players_)
            {
                if (target.GetId() == player.GetId()
                    || target.GetTeamId() == player.GetTeamId()
                    || !target.IsAlive()
                    || target.IsEliminated())
                {
                    continue;
                }

                const Vector3 toTarget {
                    target.GetPosition().x - player.GetPosition().x,
                    0.0f,
                    target.GetPosition().z - player.GetPosition().z
                };
                const float distance = Length2D(toTarget);
                if (distance <= 0.1f || distance > 5.6f)
                {
                    continue;
                }
                const Vector3 direction = Normalize2D(toTarget);
                if (Dot2D(forward, direction) < 0.48f)
                {
                    continue;
                }

                const Vector3 targetEye {
                    target.GetPosition().x,
                    target.GetPosition().y + 0.72f,
                    target.GetPosition().z
                };
                const Vector3 ray {
                    targetEye.x - origin.x,
                    targetEye.y - origin.y,
                    targetEye.z - origin.z
                };
                const float rayDistance = Length(ray);
                const std::optional<RaycastHit> wall = world_.Raycast(origin, ray, rayDistance);
                if (wall.has_value() && wall->distance < rayDistance - 0.45f)
                {
                    continue;
                }

                const float sneakMultiplier = pull && target.IsSneaking() ? 0.65f : 1.0f;
                const float force = (pull ? 7.6f : 15.0f) * sneakMultiplier * BiomeKnockbackMultiplier();
                const Vector3 impulseDirection = pull ? Vector3 { -direction.x, 0.0f, -direction.z } : direction;
                target.Damage(2);
                target.ApplyKnockback(
                    Vector3 { impulseDirection.x * force, pull ? 1.1f : 3.8f, impulseDirection.z * force },
                    pull ? 0.35f : 0.58f);
                result.worldEffects.push_back(HeroWorldEffectResult {
                    target.GetPosition(), impulseDirection, pulseColor, 0.34f, 0.30f, WorldEffectKind::Burst, true });
                result.floatingTexts.push_back(HeroFloatingTextResult {
                    pull ? "притяжение" : "толчок", target.GetPosition(), accent });
                ++affected;
            }

            result.worldEffects.push_back(HeroWorldEffectResult {
                player.GetPosition(), forward, pulseColor, 0.62f, 0.42f, WorldEffectKind::Ring, true });
            result.message = pull
                ? "Радон притянул цели перед собой."
                : "Радон выпустил силовой толчок.";
            result.messageSeconds = 2.0f;
            result.eventMessages.push_back(HeroEventMessageResult {
                affected == 0
                    ? "Импульс Радона не задел врагов."
                    : "Импульс Радона задел целей: " + std::to_string(affected) + ".",
                affected == 0 ? Fade(WHITE, 0.76f) : accent,
                affected == 0 ? 1.8f : 2.2f });
        }
        else
        {
            SetHeroAnimation(player, HeroAnimationState::Ability2, 0.46f);
            const bool blueFire = !coreAlive;
            Vector3 direction = hasLocalCamera ? cameraController_.GetAimDirection() : player.Forward();
            const float directionLength = Length(direction);
            if (directionLength <= 0.0001f)
            {
                direction = player.Forward();
            }
            else
            {
                direction = Vector3 { direction.x / directionLength, direction.y / directionLength, direction.z / directionLength };
            }

            EnergyProjectile projectile {};
            projectile.position = Vector3 {
                player.GetPosition().x + direction.x * 0.75f,
                player.GetPosition().y + 0.82f + direction.y * 0.75f,
                player.GetPosition().z + direction.z * 0.75f
            };
            projectile.ownerId = player.GetId();
            projectile.ownerTeamId = player.GetTeamId();
            projectile.velocity = Vector3 { direction.x * 10.0f, direction.y * 10.0f + 2.0f, direction.z * 10.0f };
            projectile.damage = blueFire ? 24 : 12;
            projectile.radius = 0.28f;
            projectile.explosionRadius = 1.6f;
            projectile.fireZone = true;
            projectile.blueFire = blueFire;
            projectiles_.push_back(projectile);

            const Color fireColor = blueFire ? Color { 92, 164, 255, 255 } : Color { 255, 118, 70, 255 };
            result.worldEffects.push_back(HeroWorldEffectResult {
                projectile.position, direction, fireColor, 0.36f, 0.32f, WorldEffectKind::Trail, true });
            result.message = blueFire
                ? "Радон бросил синий Молотов."
                : "Радон бросил коктейль Молотова.";
            result.messageSeconds = 2.0f;
            result.eventMessages.push_back(HeroEventMessageResult {
                blueFire
                    ? "Синий огонь Радона горит в 2 раза горячее."
                    : "Огненная область Радона создана.",
                fireColor,
                2.4f });
        }

        result.success = true;
        result.playBreakBlockSound = true;
        return result;
    }

    if (player.GetHeroId() == HeroId::Brom && slot != HeroAbilitySlot::Ultimate)
    {
        result.handled = true;
        const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Brom);
        const HeroAbilityDefinition& ability = slot == HeroAbilitySlot::Active1 ? hero.active1 : hero.active2;
        const HeroAbilityState& state = slot == HeroAbilitySlot::Active1
            ? player.GetHeroState().active1
            : player.GetHeroState().active2;
        const Color accent = HeroAccentColor(HeroId::Brom);
        result.color = accent;
        result.position = player.GetPosition();
        result.direction = player.Forward();

        if (state.cooldownRemaining > 0.0f)
        {
            result.message = hero.name + ": " + ability.name + " на кулдауне еще "
                + FormatTenths(state.cooldownRemaining) + " с.";
            result.messageSeconds = 1.7f;
            result.playDeniedSound = true;
            return result;
        }

        const int spawnIndex = static_cast<int>(bromVacuumBots_.size() + bromTurretDrones_.size());
        if (slot == HeroAbilitySlot::Active1)
        {
            const int activeCount = static_cast<int>(std::count_if(
                bromVacuumBots_.begin(),
                bromVacuumBots_.end(),
                [&player](const BromVacuumBot& bot)
                {
                    return !bot.temporary && bot.ownerPlayerId == player.GetId();
                }));
            if (activeCount >= 2)
            {
                result.message = "Бром: одновременно могут работать только два робота-пылесоса.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }
            if (!player.GetInventory().SpendResource(ResourceType::Iron, kBromVacuumIronCost))
            {
                result.message = "Бром: для робота-пылесоса нужно 48 железа.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }

            BromVacuumBot bot {};
            bot.position = OffsetAround(player.GetPosition(), spawnIndex, 1.35f);
            bot.ownerPlayerId = player.GetId();
            bot.ownerTeamId = player.GetTeamId();
            bot.temporary = false;
            bot.lifetime = 0.0f;
            bot.pulseTimer = 0.4f;
            bot.invulnerabilityTimer = 0.0f;
            bromVacuumBots_.push_back(bot);
            player.AddHeroUltimateCharge(8.0f);
            SetHeroAnimation(player, HeroAnimationState::Ability1, 0.34f);
            result.worldEffects.push_back(HeroWorldEffectResult {
                bot.position, Vector3 { 0.0f, 0.0f, 1.0f }, accent, 0.26f, 0.34f, WorldEffectKind::Burst, false });
            result.floatingTexts.push_back(HeroFloatingTextResult { "робот-пылесос", bot.position, accent });
            result.message = "Бром собрал робота-пылесоса за 48 железа.";
            result.messageSeconds = 2.2f;
            result.eventMessages.push_back(HeroEventMessageResult {
                "Робот-пылесос Брома ищет ресурсы и несет их в командный сундук.", accent, 2.8f });
        }
        else
        {
            const int activeCount = static_cast<int>(std::count_if(
                bromTurretDrones_.begin(),
                bromTurretDrones_.end(),
                [&player](const BromTurretDrone& drone)
                {
                    return !drone.temporary && drone.ownerPlayerId == player.GetId();
                }));
            if (activeCount >= 1)
            {
                result.message = "Бром: одновременно может работать только один дрон-турель.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }
            if (!player.GetInventory().SpendResource(ResourceType::Gold, kBromTurretGoldCost))
            {
                result.message = "Бром: для дрона-турели нужно 12 золота.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }

            BromTurretDrone drone {};
            drone.position = OffsetAround(Vector3 { player.GetPosition().x, player.GetPosition().y + 1.05f, player.GetPosition().z }, spawnIndex, 1.65f);
            drone.ownerPlayerId = player.GetId();
            drone.ownerTeamId = player.GetTeamId();
            drone.temporary = false;
            drone.lifetime = 0.0f;
            drone.fireCooldown = 0.4f;
            drone.pulseTimer = 0.4f;
            drone.invulnerabilityTimer = 0.0f;
            bromTurretDrones_.push_back(drone);
            player.AddHeroUltimateCharge(10.0f);
            SetHeroAnimation(player, HeroAnimationState::Ability2, 0.34f);
            result.worldEffects.push_back(HeroWorldEffectResult {
                drone.position, Vector3 { 0.0f, 0.0f, 1.0f }, accent, 0.30f, 0.34f, WorldEffectKind::Burst, false });
            result.floatingTexts.push_back(HeroFloatingTextResult { "дрон-турель", drone.position, accent });
            result.message = "Бром собрал дрона-турель за 12 золота.";
            result.messageSeconds = 2.2f;
            result.eventMessages.push_back(HeroEventMessageResult {
                "Дрон-турель Брома стреляет только по игрокам и не атакует Кор.", accent, 2.8f });
        }

        player.StartHeroAbilityCooldown(slot, ability.cooldownSeconds, ability.durationSeconds);
        result.success = true;
        result.playBuildSound = true;
        return result;
    }

    if (player.GetHeroId() == HeroId::Konvoy)
    {
        result.handled = true;
        const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Konvoy);
        const HeroAbilityDefinition* ability = nullptr;
        const HeroAbilityState* state = nullptr;
        switch (slot)
        {
        case HeroAbilitySlot::Active1:
            ability = &hero.active1;
            state = &player.GetHeroState().active1;
            break;
        case HeroAbilitySlot::Active2:
            ability = &hero.active2;
            state = &player.GetHeroState().active2;
            break;
        case HeroAbilitySlot::Ultimate:
            ability = &hero.ultimate;
            state = &player.GetHeroState().ultimate;
            break;
        }

        if (ability == nullptr || state == nullptr)
        {
            return result;
        }
        const Color accent = HeroAccentColor(HeroId::Konvoy);
        result.color = accent;
        result.position = player.GetPosition();
        result.direction = player.Forward();
        if (state->cooldownRemaining > 0.0f)
        {
            result.message = hero.name + ": " + ability->name + " на кулдауне еще "
                + FormatTenths(state->cooldownRemaining) + " с.";
            result.messageSeconds = 1.7f;
            result.playDeniedSound = true;
            return result;
        }
        if (slot == HeroAbilitySlot::Ultimate && !player.GetHeroState().ultimateReady)
        {
            result.message = "Конвой: ульта не готова, заряд "
                + std::to_string(static_cast<int>(player.GetHeroState().ultimateCharge)) + "%.";
            result.messageSeconds = 1.8f;
            result.playDeniedSound = true;
            return result;
        }

        if (slot == HeroAbilitySlot::Active1)
        {
            const int activeCount = static_cast<int>(std::count_if(
                konvoyTraps_.begin(),
                konvoyTraps_.end(),
                [&player](const KonvoyTrap& trap)
                {
                    return trap.ownerPlayerId == player.GetId();
                }));
            if (activeCount >= kKonvoyMaxTraps)
            {
                result.message = "Конвой: одновременно может быть до 2 капканов.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }

            Vector3 forward = HasLocalCamera(ControlKindForPlayer(player)) ? cameraController_.GetFlatForward() : player.Forward();
            forward = Normalize2D(forward);
            if (Length2D(forward) <= 0.0001f)
            {
                forward = player.Forward();
            }
            const Vector3 desired {
                player.GetPosition().x + forward.x * 1.15f,
                player.GetPosition().y,
                player.GetPosition().z + forward.z * 1.15f
            };
            const GridPos column = world_.WorldToGrid(desired);
            std::optional<Vector3> trapPosition;
            const int startY = world_.WorldToGrid(player.GetPosition()).y + 1;
            for (int y = startY; y >= startY - 3; --y)
            {
                const GridPos ground { column.x, y, column.z };
                const GridPos above { column.x, y + 1, column.z };
                if (world_.IsSolid(ground) && world_.IsAir(above))
                {
                    const Vector3 groundCenter = world_.GridToWorld(ground);
                    trapPosition = Vector3 { desired.x, groundCenter.y + 0.57f, desired.z };
                    break;
                }
            }
            if (!trapPosition.has_value())
            {
                result.message = "Конвой: капкану нужна свободная поверхность рядом.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }

            KonvoyTrap trap {};
            trap.position = *trapPosition;
            trap.ownerPlayerId = player.GetId();
            trap.ownerTeamId = player.GetTeamId();
            trap.lifetime = std::max(0.1f, ability->durationSeconds);
            trap.flashTimer = 0.45f;
            konvoyTraps_.push_back(trap);
            SetHeroAnimation(player, HeroAnimationState::Ability1, 0.30f);
            result.worldEffects.push_back(HeroWorldEffectResult {
                trap.position, forward, accent, 0.52f, 0.38f, WorldEffectKind::Ring, true });
            result.floatingTexts.push_back(HeroFloatingTextResult { "капкан", trap.position, accent });
            result.message = "Конвой поставил капкан. Время существования: 45 секунд.";
            result.messageSeconds = 2.5f;
            result.eventMessages.push_back(HeroEventMessageResult {
                "Капкан Конвоя заметен: его можно обойти или сломать инструментом.", accent, 2.8f });
            result.playBuildSound = true;
        }
        else if (slot == HeroAbilitySlot::Active2)
        {
            Player* target = FindNearbyEnemyPlayer(player, kKonvoyHandcuffRadius);
            if (target == nullptr)
            {
                result.message = "Конвой: для наручников нужен противник в радиусе 6 блоков.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }
            const Vector3 origin { player.GetPosition().x, player.GetPosition().y + 0.78f, player.GetPosition().z };
            const Vector3 targetPoint { target->GetPosition().x, target->GetPosition().y + 0.72f, target->GetPosition().z };
            const Vector3 ray {
                targetPoint.x - origin.x,
                targetPoint.y - origin.y,
                targetPoint.z - origin.z
            };
            const float distance = Length(ray);
            const std::optional<RaycastHit> wall = world_.Raycast(origin, ray, distance);
            if (wall.has_value() && wall->distance < distance - 0.35f)
            {
                result.message = "Конвой: наручники не проходят через блоки.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }

            KonvoyTether tether {};
            tether.ownerPlayerId = player.GetId();
            tether.targetPlayerId = target->GetId();
            tether.ownerTeamId = player.GetTeamId();
            tether.lifetime = std::max(0.1f, ability->durationSeconds);
            tether.flashTimer = 0.42f;
            tether.ownerLastHealth = player.GetHealth();
            konvoyTethers_.push_back(tether);
            player.AddHeroUltimateCharge(10.0f);
            SetHeroAnimation(player, HeroAnimationState::Ability2, 0.34f);
            const Vector3 tetherDirection = Normalize2D(Vector3 {
                target->GetPosition().x - player.GetPosition().x,
                0.0f,
                target->GetPosition().z - player.GetPosition().z
            });
            result.worldEffects.push_back(HeroWorldEffectResult {
                player.GetPosition(), tetherDirection, accent, kKonvoyHandcuffRadius, 0.42f, WorldEffectKind::Trail, true });
            result.floatingTexts.push_back(HeroFloatingTextResult { "наручники", target->GetPosition(), accent });
            result.message = "Конвой связал цель наручниками на 12 секунд. Радиус цепи: 6 блоков.";
            result.messageSeconds = 2.6f;
            result.eventMessages.push_back(HeroEventMessageResult {
                "Наручники Конвоя притягивают цель и рвутся после накопленных 30+ урона по Конвою.", accent, 3.0f });
            result.playPickupSound = true;
        }
        else
        {
            KonvoyDome dome {};
            dome.position = player.GetPosition();
            dome.ownerPlayerId = player.GetId();
            dome.ownerTeamId = player.GetTeamId();
            dome.lifetime = std::max(0.1f, ability->durationSeconds);
            dome.flashTimer = 0.60f;
            for (const Player& target : players_)
            {
                if (target.IsAlive() && !target.IsEliminated()
                    && target.GetTeamId() != player.GetTeamId()
                    && DistanceSquared(target.GetPosition(), dome.position) < kKonvoyDomeVisualRadius * kKonvoyDomeVisualRadius)
                {
                    dome.initiallyInsideEnemyIds.push_back(target.GetId());
                }
            }
            konvoyDomes_.push_back(dome);
            SetHeroAnimation(player, HeroAnimationState::Ultimate, 0.52f);
            result.worldEffects.push_back(HeroWorldEffectResult {
                player.GetPosition(), Vector3 { 0.0f, 0.0f, 1.0f }, accent, kKonvoyDomeVisualRadius, 0.80f, WorldEffectKind::CorePulse, true });
            result.message = "Конвой развернул Купол содержания на 30 секунд.";
            result.messageSeconds = 2.8f;
            result.eventMessages.push_back(HeroEventMessageResult {
                "Купол содержания имеет 180 HP: враги не проходят сквозь оболочку, союзники проходят свободно.", accent, 3.2f });
            result.playPurchaseSound = true;
        }

        player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
        result.success = true;
        return result;
    }

    if (player.GetHeroId() == HeroId::Svidetel)
    {
        result.handled = true;
        const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Svidetel);
        const HeroAbilityDefinition& ability = slot == HeroAbilitySlot::Active1
            ? hero.active1
            : (slot == HeroAbilitySlot::Active2 ? hero.active2 : hero.ultimate);
        const Color accent = HeroAccentColor(HeroId::Svidetel);
        result.color = accent;
        result.position = player.GetPosition();
        result.direction = player.Forward();

        if (!player.IsHeroAbilityReady(slot))
        {
            result.message = "Свидетель: способность не готова.";
            result.messageSeconds = 1.6f;
            result.playDeniedSound = true;
            return result;
        }

        if (slot == HeroAbilitySlot::Active2)
        {
            const Vector3 origin { player.GetPosition().x, player.GetPosition().y + 0.72f, player.GetPosition().z };
            const Vector3 direction = HasLocalCamera(ControlKindForPlayer(player)) ? cameraController_.GetAimDirection() : player.Forward();
            const std::optional<RaycastHit> hit = world_.Raycast(origin, direction, 5.5f);
            if (!hit.has_value() || hit->blockData.type == BlockType::EnergyCoreBlock)
            {
                result.message = "Свидетель: наведитесь на обычные блоки вдали от Кора.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }

            int phased = 0;
            for (int x = -1; x <= 1; ++x)
            {
                for (int y = 0; y <= 1; ++y)
                {
                    GridPos pos = hit->block;
                    if (std::fabs(hit->normal.x) > 0.5f)
                    {
                        pos.y += y;
                        pos.z += x;
                    }
                    else if (std::fabs(hit->normal.z) > 0.5f)
                    {
                        pos.x += x;
                        pos.y += y;
                    }
                    else
                    {
                        pos.x += x;
                        pos.z += y;
                    }
                    const Block* block = world_.GetBlock(pos);
                    if (block == nullptr || block->type == BlockType::EnergyCoreBlock || !block->breakable)
                    {
                        continue;
                    }
                    bool nearCore = false;
                    const Vector3 center = world_.GridToWorld(pos);
                    for (const EnergyCore& core : matchSimulation_.Cores())
                    {
                        if (DistanceSquared(center, world_.GridToWorld(core.GetBlockPosition())) < 12.0f)
                        {
                            nearCore = true;
                            break;
                        }
                    }
                    if (nearCore)
                    {
                        continue;
                    }
                    svidetelPhaseBlocks_.push_back(SvidetelPhaseBlock { pos, *block, ability.durationSeconds });
                    RemoveWorldBlock(pos, BlockDeltaReason::PhaseRemove, player.GetId());
                    result.worldEffects.push_back(HeroWorldEffectResult {
                        center, Vector3 { 0.0f, 0.0f, 1.0f }, accent, 0.30f, 0.42f, WorldEffectKind::Burst, false });
                    ++phased;
                }
            }
            if (phased == 0)
            {
                result.message = "Свидетель: участок защищен или слишком близко к Кору.";
                result.messageSeconds = 2.0f;
                result.playDeniedSound = true;
                return result;
            }
        }

        player.StartHeroAbilityCooldown(slot, ability.cooldownSeconds, ability.durationSeconds);
        SetHeroAnimation(player, slot == HeroAbilitySlot::Active1
            ? HeroAnimationState::Ability1
            : (slot == HeroAbilitySlot::Active2 ? HeroAnimationState::Ability2 : HeroAnimationState::Ultimate), 0.38f);

        result.success = true;
        result.playPickupSound = true;
        result.floatingTexts.push_back(HeroFloatingTextResult {
            ability.name,
            Vector3 { player.GetPosition().x, player.GetPosition().y + 1.35f, player.GetPosition().z },
            accent });
        result.worldEffects.push_back(HeroWorldEffectResult {
            player.GetPosition(), player.Forward(), accent, 0.95f, 0.65f, WorldEffectKind::Ring, true });

        if (slot == HeroAbilitySlot::Active1)
        {
            svidetelEchoes_.push_back(SvidetelEcho {
                player.GetPosition(), player.GetId(), player.GetTeamId(), ability.durationSeconds, 0.2f, true, {}, 0.0f });
            result.message = "Свидетель создал вооруженное Эхо.";
            result.messageSeconds = 2.2f;
        }
        else if (slot == HeroAbilitySlot::Active2)
        {
            result.message = "Свидетель сделал блоки фазовыми на 4 секунды.";
            result.messageSeconds = 2.4f;
        }
        else
        {
            result.message = "Свидетель видит контуры врагов, ресурсов и Коров.";
            result.messageSeconds = 2.6f;
        }
        return result;
    }

    if (player.GetHeroId() != HeroId::Likho)
    {
        return result;
    }

    result.handled = true;
    const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Likho);
    const HeroAbilityDefinition& ability = slot == HeroAbilitySlot::Active1
        ? hero.active1
        : (slot == HeroAbilitySlot::Active2 ? hero.active2 : hero.ultimate);
    result.color = HeroAccentColor(HeroId::Likho);
    result.position = player.GetPosition();
    result.direction = player.Forward();

    if (!player.IsHeroAbilityReady(slot))
    {
        result.message = "Лихо: способность не готова.";
        result.messageSeconds = 1.6f;
        result.playDeniedSound = true;
        return result;
    }

    Player* disguiseTarget = nullptr;
    if (slot == HeroAbilitySlot::Ultimate)
    {
        const Vector3 origin { player.GetPosition().x, player.GetPosition().y + 0.72f, player.GetPosition().z };
        const Vector3 aim = Normalize(HasLocalCamera(ControlKindForPlayer(player)) ? cameraController_.GetAimDirection() : player.Forward());
        float bestScore = -1.0f;
        for (Player& candidate : players_)
        {
            if (!candidate.IsAlive() || candidate.IsEliminated() || candidate.GetTeamId() == player.GetTeamId())
            {
                continue;
            }
            const Vector3 targetPoint { candidate.GetPosition().x, candidate.GetPosition().y + 0.65f, candidate.GetPosition().z };
            const Vector3 delta { targetPoint.x - origin.x, targetPoint.y - origin.y, targetPoint.z - origin.z };
            const float distance = Length(delta);
            if (distance > 24.0f || distance <= 0.001f)
            {
                continue;
            }
            const Vector3 normalizedDelta = Normalize(delta);
            const float alignment = aim.x * normalizedDelta.x + aim.y * normalizedDelta.y + aim.z * normalizedDelta.z;
            if (alignment < 0.72f)
            {
                continue;
            }
            const std::optional<RaycastHit> wall = world_.Raycast(origin, delta, distance);
            if (wall.has_value() && wall->distance < distance - 0.35f)
            {
                continue;
            }
            const float score = alignment * 2.0f - distance / 24.0f;
            if (score > bestScore)
            {
                bestScore = score;
                disguiseTarget = &candidate;
            }
        }
        if (disguiseTarget == nullptr)
        {
            result.message = "Лихо: наведитесь на видимого врага, чтобы скопировать облик.";
            result.messageSeconds = 2.0f;
            result.playDeniedSound = true;
            return result;
        }
    }

    player.StartHeroAbilityCooldown(slot, ability.cooldownSeconds, ability.durationSeconds);
    HeroRuntimeState& state = player.MutableHeroState();
    SetHeroAnimation(player, slot == HeroAbilitySlot::Active1
        ? HeroAnimationState::Ability1
        : (slot == HeroAbilitySlot::Active2 ? HeroAnimationState::Ability2 : HeroAnimationState::Ultimate), 0.34f);

    result.success = true;
    result.hasWorldEffect = true;
    result.floatingText = ability.name;
    result.floatingTextPosition = Vector3 { player.GetPosition().x, player.GetPosition().y + 1.35f, player.GetPosition().z };
    result.hasFloatingText = true;
    result.playPickupSound = true;
    result.messageSeconds = 2.4f;

    if (slot == HeroAbilitySlot::Active1)
    {
        result.message = "Лихо заглушило шаги. Первый удар проверит атаку в спину.";
        result.radius = 0.75f;
        result.seconds = 0.55f;
        result.effectKind = WorldEffectKind::Trail;
        result.directedWorldEffect = true;
    }
    else if (slot == HeroAbilitySlot::Active2)
    {
        result.message = "Лихо: следующие удары накладывают кровотечение.";
        result.radius = 0.70f;
        result.seconds = 0.45f;
    }
    else
    {
        state.likhoDisguiseTeamId = disguiseTarget->GetTeamId();
        state.likhoDisguisePlayerId = disguiseTarget->GetId();
        state.likhoDisguiseHeroId = disguiseTarget->GetHeroId();
        result.message = "Лихо приняло искаженный облик чужой команды. Атака или урон раскроют маскировку.";
        result.messageSeconds = 3.0f;
        result.radius = 1.1f;
        result.seconds = 0.85f;
        result.effectKind = WorldEffectKind::Ring;
        result.directedWorldEffect = true;
    }
    return result;
}

void Game::PresentHeroAbilityResult(const HeroAbilityActionResult& result)
{
    if (!result.handled || suppressLocalFeedback_)
    {
        return;
    }
    if (!result.message.empty())
    {
        SetMessage(result.message, result.messageSeconds);
    }
    if (result.hasWorldEffect)
    {
        if (result.directedWorldEffect)
        {
            AddWorldEffect(result.position, result.direction, result.color, result.radius, result.seconds, result.effectKind);
        }
        else
        {
            AddWorldEffect(result.position, result.color, result.radius, result.seconds);
        }
    }
    if (result.hasFloatingText)
    {
        AddFloatingText(result.floatingText, result.floatingTextPosition, result.color);
    }
    for (const HeroWorldEffectResult& effect : result.worldEffects)
    {
        if (effect.directed)
        {
            AddWorldEffect(effect.position, effect.direction, effect.color, effect.radius, effect.seconds, effect.kind);
        }
        else
        {
            AddWorldEffect(effect.position, effect.color, effect.radius, effect.seconds);
        }
    }
    for (const HeroFloatingTextResult& text : result.floatingTexts)
    {
        AddFloatingText(text.text, text.position, text.color);
    }
    for (const HeroEventMessageResult& event : result.eventMessages)
    {
        AddEventMessage(event.message, event.color, event.seconds);
    }
    if (result.playPickupSound)
    {
        audio_.PlayPickup();
    }
    if (result.playBuildSound)
    {
        audio_.PlayBuild();
    }
    if (result.playPurchaseSound)
    {
        audio_.PlayPurchase();
    }
    if (result.playBreakBlockSound)
    {
        audio_.PlayBreakBlock();
    }
    if (result.playDeniedSound)
    {
        audio_.PlayDenied();
    }
}

bool Game::UseHeroAbility(Player& player, HeroAbilitySlot slot)
{
    const HeroAbilityActionResult result = ApplyHeroAbilityAction(player, slot);
    if (result.handled)
    {
        PresentHeroAbilityResult(result);
        return result.success;
    }
    return UseHeroAbilityLegacy(player, slot);
}

bool Game::UseHeroAbilityLegacy(Player& player, HeroAbilitySlot slot)
{
    if (!player.IsAlive() || player.IsEliminated())
    {
        return false;
    }

    if (player.GetHeroId() == HeroId::Radon)
    {
        return UseRadonAbility(player, slot);
    }
    if (player.GetHeroId() == HeroId::Orbita)
    {
        return UseOrbitaAbility(player, slot);
    }
    if (player.GetHeroId() == HeroId::Brom)
    {
        return UseBromAbility(player, slot);
    }
    if (player.GetHeroId() == HeroId::Konvoy)
    {
        return UseKonvoyAbility(player, slot);
    }
    if (player.GetHeroId() == HeroId::Likho)
    {
        return UseLikhoAbility(player, slot);
    }
    if (player.GetHeroId() == HeroId::Svidetel)
    {
        return UseSvidetelAbility(player, slot);
    }

    const HeroDefinition& hero = HeroSystem::GetDefinition(player.GetHeroId());
    const HeroAbilityDefinition* ability = nullptr;
    const HeroAbilityState* state = nullptr;
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        ability = &hero.active1;
        state = &player.GetHeroState().active1;
        break;
    case HeroAbilitySlot::Active2:
        ability = &hero.active2;
        state = &player.GetHeroState().active2;
        break;
    case HeroAbilitySlot::Ultimate:
        ability = &hero.ultimate;
        state = &player.GetHeroState().ultimate;
        break;
    }

    if (ability == nullptr || state == nullptr)
    {
        return false;
    }
    if (state->cooldownRemaining > 0.0f)
    {
        SetMessage(hero.name + ": " + ability->name + " на кулдауне еще "
            + FormatTenths(state->cooldownRemaining) + " с.", 1.7f);
        audio_.PlayDenied();
        return false;
    }
    if (slot == HeroAbilitySlot::Ultimate && !player.GetHeroState().ultimateReady)
    {
        SetMessage(hero.name + ": ульта не готова, заряд "
            + std::to_string(static_cast<int>(player.GetHeroState().ultimateCharge)) + "%.", 1.8f);
        audio_.PlayDenied();
        return false;
    }

    player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
    const Color accent = HeroAccentColor(hero.id);
    AddWorldEffect(player.GetPosition(), accent, slot == HeroAbilitySlot::Ultimate ? 0.62f : 0.38f, 0.45f);
    AddFloatingText(HeroAbilitySlotName(slot), Vector3 { player.GetPosition().x, player.GetPosition().y + 1.4f, player.GetPosition().z }, accent);
    const std::string message = hero.name + ": " + ability->name + " - базовый каркас активирован.";
    SetMessage(message, 2.2f);
    AddEventMessage(message, accent, 2.8f);
    audio_.PlayPickup();
    return true;
}

bool Game::UseLikhoAbility(Player& player, HeroAbilitySlot slot)
{
    const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Likho);
    const HeroAbilityDefinition& ability = slot == HeroAbilitySlot::Active1
        ? hero.active1
        : (slot == HeroAbilitySlot::Active2 ? hero.active2 : hero.ultimate);
    if (!player.IsHeroAbilityReady(slot))
    {
        SetMessage("Лихо: способность не готова.", 1.6f);
        audio_.PlayDenied();
        return false;
    }

    Player* disguiseTarget = nullptr;
    if (slot == HeroAbilitySlot::Ultimate)
    {
        const Vector3 origin { player.GetPosition().x, player.GetPosition().y + 0.72f, player.GetPosition().z };
        const Vector3 aim = Normalize(player.IsLocal() ? cameraController_.GetAimDirection() : player.Forward());
        float bestScore = -1.0f;
        for (Player& candidate : players_)
        {
            if (!candidate.IsAlive() || candidate.IsEliminated() || candidate.GetTeamId() == player.GetTeamId())
            {
                continue;
            }
            const Vector3 targetPoint { candidate.GetPosition().x, candidate.GetPosition().y + 0.65f, candidate.GetPosition().z };
            const Vector3 delta { targetPoint.x - origin.x, targetPoint.y - origin.y, targetPoint.z - origin.z };
            const float distance = Length(delta);
            if (distance > 24.0f || distance <= 0.001f)
            {
                continue;
            }
            const Vector3 normalizedDelta = Normalize(delta);
            const float alignment = aim.x * normalizedDelta.x + aim.y * normalizedDelta.y + aim.z * normalizedDelta.z;
            if (alignment < 0.72f)
            {
                continue;
            }
            const std::optional<RaycastHit> wall = world_.Raycast(origin, delta, distance);
            if (wall.has_value() && wall->distance < distance - 0.35f)
            {
                continue;
            }
            const float score = alignment * 2.0f - distance / 24.0f;
            if (score > bestScore)
            {
                bestScore = score;
                disguiseTarget = &candidate;
            }
        }
        if (disguiseTarget == nullptr)
        {
            SetMessage("Лихо: наведитесь на видимого врага, чтобы скопировать облик.", 2.0f);
            audio_.PlayDenied();
            return false;
        }
    }

    player.StartHeroAbilityCooldown(slot, ability.cooldownSeconds, ability.durationSeconds);
    HeroRuntimeState& state = player.MutableHeroState();
    const Color accent = HeroAccentColor(HeroId::Likho);
    if (slot == HeroAbilitySlot::Active1)
    {
        SetMessage("Лихо заглушило шаги. Первый удар проверит атаку в спину.", 2.4f);
        AddWorldEffect(player.GetPosition(), player.Forward(), accent, 0.75f, 0.55f, WorldEffectKind::Trail);
    }
    else if (slot == HeroAbilitySlot::Active2)
    {
        SetMessage("Лихо: следующие удары накладывают кровотечение.", 2.4f);
        AddWorldEffect(player.GetPosition(), accent, 0.70f, 0.45f);
    }
    else
    {
        state.likhoDisguiseTeamId = disguiseTarget->GetTeamId();
        state.likhoDisguisePlayerId = disguiseTarget->GetId();
        state.likhoDisguiseHeroId = disguiseTarget->GetHeroId();
        SetMessage("Лихо приняло искаженный облик чужой команды. Атака или урон раскроют маскировку.", 3.0f);
        AddWorldEffect(player.GetPosition(), player.Forward(), accent, 1.1f, 0.85f, WorldEffectKind::Ring);
    }
    SetHeroAnimation(player, slot == HeroAbilitySlot::Active1
        ? HeroAnimationState::Ability1
        : (slot == HeroAbilitySlot::Active2 ? HeroAnimationState::Ability2 : HeroAnimationState::Ultimate), 0.34f);
    AddFloatingText(ability.name, Vector3 { player.GetPosition().x, player.GetPosition().y + 1.35f, player.GetPosition().z }, accent);
    audio_.PlayPickup();
    return true;
}

bool Game::UseSvidetelAbility(Player& player, HeroAbilitySlot slot)
{
    const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Svidetel);
    const HeroAbilityDefinition& ability = slot == HeroAbilitySlot::Active1
        ? hero.active1
        : (slot == HeroAbilitySlot::Active2 ? hero.active2 : hero.ultimate);
    if (!player.IsHeroAbilityReady(slot))
    {
        SetMessage("Свидетель: способность не готова.", 1.6f);
        audio_.PlayDenied();
        return false;
    }

    if (slot == HeroAbilitySlot::Active2)
    {
        const Vector3 origin { player.GetPosition().x, player.GetPosition().y + 0.72f, player.GetPosition().z };
        const Vector3 direction = player.IsLocal() ? cameraController_.GetAimDirection() : player.Forward();
        const std::optional<RaycastHit> hit = world_.Raycast(origin, direction, 5.5f);
        if (!hit.has_value() || hit->blockData.type == BlockType::EnergyCoreBlock)
        {
            SetMessage("Свидетель: наведитесь на обычные блоки вдали от Кора.", 2.0f);
            audio_.PlayDenied();
            return false;
        }

        int phased = 0;
        for (int x = -1; x <= 1; ++x)
        {
            for (int y = 0; y <= 1; ++y)
            {
                GridPos pos = hit->block;
                if (std::fabs(hit->normal.x) > 0.5f)
                {
                    pos.y += y;
                    pos.z += x;
                }
                else if (std::fabs(hit->normal.z) > 0.5f)
                {
                    pos.x += x;
                    pos.y += y;
                }
                else
                {
                    pos.x += x;
                    pos.z += y;
                }
                const Block* block = world_.GetBlock(pos);
                if (block == nullptr || block->type == BlockType::EnergyCoreBlock || !block->breakable)
                {
                    continue;
                }
                bool nearCore = false;
                const Vector3 center = world_.GridToWorld(pos);
                for (const EnergyCore& core : matchSimulation_.Cores())
                {
                    if (DistanceSquared(center, world_.GridToWorld(core.GetBlockPosition())) < 12.0f)
                    {
                        nearCore = true;
                        break;
                    }
                }
                if (nearCore)
                {
                    continue;
                }
                svidetelPhaseBlocks_.push_back(SvidetelPhaseBlock { pos, *block, ability.durationSeconds });
                RemoveWorldBlock(pos, BlockDeltaReason::PhaseRemove, player.GetId());
                AddWorldEffect(center, HeroAccentColor(HeroId::Svidetel), 0.30f, 0.42f);
                ++phased;
            }
        }
        if (phased == 0)
        {
            SetMessage("Свидетель: участок защищен или слишком близко к Кору.", 2.0f);
            audio_.PlayDenied();
            return false;
        }
        player.StartHeroAbilityCooldown(slot, ability.cooldownSeconds, ability.durationSeconds);
        SetMessage("Свидетель сделал блоки фазовыми на 4 секунды.", 2.4f);
    }
    else
    {
        player.StartHeroAbilityCooldown(slot, ability.cooldownSeconds, ability.durationSeconds);
        if (slot == HeroAbilitySlot::Active1)
        {
            svidetelEchoes_.push_back(SvidetelEcho {
                player.GetPosition(), player.GetId(), player.GetTeamId(), ability.durationSeconds, 0.2f, true, {}, 0.0f });
            SetMessage("Свидетель создал вооруженное Эхо.", 2.2f);
        }
        else
        {
            SetMessage("Свидетель видит контуры врагов, ресурсов и Коров.", 2.6f);
        }
    }

    SetHeroAnimation(player, slot == HeroAbilitySlot::Active1
        ? HeroAnimationState::Ability1
        : (slot == HeroAbilitySlot::Active2 ? HeroAnimationState::Ability2 : HeroAnimationState::Ultimate), 0.38f);
    AddWorldEffect(player.GetPosition(), player.Forward(), HeroAccentColor(HeroId::Svidetel), 0.95f, 0.65f, WorldEffectKind::Ring);
    AddFloatingText(ability.name, Vector3 { player.GetPosition().x, player.GetPosition().y + 1.35f, player.GetPosition().z }, HeroAccentColor(HeroId::Svidetel));
    audio_.PlayPickup();
    return true;
}

void Game::SetHeroAnimation(Player& player, HeroAnimationState state, float seconds)
{
    HeroRuntimeState& heroState = player.MutableHeroState();
    heroState.animationState = state;
    heroState.animationTimer = std::max(0.0f, seconds);
    heroState.animationDuration = std::max(0.0f, seconds);
}

bool Game::UseRadonAbility(Player& player, HeroAbilitySlot slot)
{
    const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Radon);
    const HeroAbilityDefinition* ability = slot == HeroAbilitySlot::Active1
        ? &hero.active1
        : (slot == HeroAbilitySlot::Active2 ? &hero.active2 : &hero.ultimate);
    const HeroAbilityState* state = slot == HeroAbilitySlot::Active1
        ? &player.GetHeroState().active1
        : (slot == HeroAbilitySlot::Active2 ? &player.GetHeroState().active2 : &player.GetHeroState().ultimate);

    if (state->cooldownRemaining > 0.0f)
    {
        SetMessage(hero.name + ": " + ability->name + " на кулдауне еще " + FormatTenths(state->cooldownRemaining) + " с.", 1.7f);
        audio_.PlayDenied();
        return false;
    }

    EnergyCore* core = FindCoreByTeam(player.GetTeamId());
    const bool coreAlive = core != nullptr && core->IsAlive();
    if (slot == HeroAbilitySlot::Ultimate)
    {
        if (!player.GetHeroState().ultimateReady)
        {
            SetMessage("Радон: ульта не готова, заряд " + std::to_string(static_cast<int>(player.GetHeroState().ultimateCharge)) + "%.", 1.8f);
            audio_.PlayDenied();
            return false;
        }

        if (coreAlive)
        {
            HeroRuntimeState& heroState = player.MutableHeroState();
            heroState.ultimatePrimed = !heroState.ultimatePrimed;
            SetHeroAnimation(player, heroState.ultimatePrimed ? HeroAnimationState::UltPrimed : HeroAnimationState::Recovery, 0.45f);
            if (core != nullptr)
            {
                AddWorldEffect(world_.GridToWorld(core->GetBlockPosition()), player.Forward(), HeroAccentColor(HeroId::Radon), 1.05f, 0.85f, WorldEffectKind::CorePulse);
            }
            SetMessage(heroState.ultimatePrimed
                    ? "Радон: перехват разрушения Кора включен."
                    : "Радон: перехват разрушения Кора выключен.",
                2.2f);
            AddEventMessage(heroState.ultimatePrimed ? "Ульта Радона ожидает угрозу Кору." : "Ульта Радона снята с подтверждения.",
                HeroAccentColor(HeroId::Radon),
                2.6f);
            audio_.PlayPickup();
            return true;
        }

        player.StartHeroAbilityCooldown(HeroAbilitySlot::Ultimate, 40.0f, 0.0f);
        UseRadonDestroyedCoreUltimate(player);
        return true;
    }

    player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
    if (slot == HeroAbilitySlot::Active1)
    {
        const bool pull = !coreAlive && (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
        SetHeroAnimation(player, HeroAnimationState::WindUp, pull ? 0.44f : 0.40f);
        UseRadonForcePulse(player, pull);
        return true;
    }

    SetHeroAnimation(player, HeroAnimationState::Ability2, 0.46f);
    UseRadonMolotov(player);
    return true;
}

void Game::UseRadonForcePulse(Player& player, bool pull)
{
    const Vector3 origin {
        player.GetPosition().x,
        player.GetPosition().y + 0.72f,
        player.GetPosition().z
    };
    Vector3 forward = player.IsLocal() ? cameraController_.GetFlatForward() : player.Forward();
    forward = Normalize2D(forward);
    if (Length2D(forward) <= 0.0001f)
    {
        forward = player.Forward();
    }

    int affected = 0;
    const Color pulseColor = pull ? Color { 92, 164, 255, 255 } : HeroAccentColor(HeroId::Radon);
    AddWorldEffect(origin, forward, pulseColor, pull ? 5.2f : 5.6f, 0.34f, pull ? WorldEffectKind::Pull : WorldEffectKind::Cone);
    for (Player& target : players_)
    {
        if (target.GetId() == player.GetId()
            || target.GetTeamId() == player.GetTeamId()
            || !target.IsAlive()
            || target.IsEliminated())
        {
            continue;
        }

        const Vector3 toTarget {
            target.GetPosition().x - player.GetPosition().x,
            0.0f,
            target.GetPosition().z - player.GetPosition().z
        };
        const float distance = Length2D(toTarget);
        if (distance <= 0.1f || distance > 5.6f)
        {
            continue;
        }
        const Vector3 direction = Normalize2D(toTarget);
        if (Dot2D(forward, direction) < 0.48f)
        {
            continue;
        }

        const Vector3 targetEye {
            target.GetPosition().x,
            target.GetPosition().y + 0.72f,
            target.GetPosition().z
        };
        const Vector3 ray {
            targetEye.x - origin.x,
            targetEye.y - origin.y,
            targetEye.z - origin.z
        };
        const float rayDistance = Length(ray);
        const std::optional<RaycastHit> wall = world_.Raycast(origin, ray, rayDistance);
        if (wall.has_value() && wall->distance < rayDistance - 0.45f)
        {
            continue;
        }

        const float sneakMultiplier = pull && target.IsSneaking() ? 0.65f : 1.0f;
        // The pulse must lift the target: grounded knockback is damped by
        // friction so quickly that the ability can read as having no impact.
        const float force = (pull ? 7.6f : 15.0f) * sneakMultiplier * BiomeKnockbackMultiplier();
        const Vector3 impulseDirection = pull ? Vector3 { -direction.x, 0.0f, -direction.z } : direction;
        target.Damage(2);
        target.ApplyKnockback(
            Vector3 { impulseDirection.x * force, pull ? 1.1f : 3.8f, impulseDirection.z * force },
            pull ? 0.35f : 0.58f);
        AddWorldEffect(target.GetPosition(), impulseDirection, pulseColor, 0.34f, 0.30f, WorldEffectKind::Burst);
        AddFloatingText(pull ? "притяжение" : "толчок", target.GetPosition(), HeroAccentColor(HeroId::Radon));
        ++affected;
    }

    AddWorldEffect(player.GetPosition(), forward, pulseColor, 0.62f, 0.42f, WorldEffectKind::Ring);
    SetMessage(pull
            ? "Радон притянул цели перед собой."
            : "Радон выпустил силовой толчок.",
        2.0f);
    if (affected == 0)
    {
        AddEventMessage("Импульс Радона не задел врагов.", Fade(WHITE, 0.76f), 1.8f);
    }
    else
    {
        AddEventMessage("Импульс Радона задел целей: " + std::to_string(affected) + ".", HeroAccentColor(HeroId::Radon), 2.2f);
    }
    audio_.PlayBreakBlock();
}

void Game::UseRadonMolotov(Player& player)
{
    EnergyCore* core = FindCoreByTeam(player.GetTeamId());
    const bool blueFire = core == nullptr || !core->IsAlive();
    Vector3 direction = player.IsLocal() ? cameraController_.GetAimDirection() : player.Forward();
    const float directionLength = Length(direction);
    if (directionLength <= 0.0001f)
    {
        direction = player.Forward();
    }
    else
    {
        direction = Vector3 { direction.x / directionLength, direction.y / directionLength, direction.z / directionLength };
    }

    EnergyProjectile projectile {};
    projectile.position = Vector3 {
        player.GetPosition().x + direction.x * 0.75f,
        player.GetPosition().y + 0.82f + direction.y * 0.75f,
        player.GetPosition().z + direction.z * 0.75f
    };
    projectile.ownerId = player.GetId();
    projectile.ownerTeamId = player.GetTeamId();
    projectile.velocity = Vector3 { direction.x * 10.0f, direction.y * 10.0f + 2.0f, direction.z * 10.0f };
    projectile.damage = blueFire ? 24 : 12;
    projectile.radius = 0.28f;
    projectile.explosionRadius = 1.6f;
    projectile.fireZone = true;
    projectile.blueFire = blueFire;
    projectiles_.push_back(projectile);
    AddWorldEffect(projectile.position, direction, blueFire ? Color { 92, 164, 255, 255 } : Color { 255, 118, 70, 255 }, 0.36f, 0.32f, WorldEffectKind::Trail);

    SetMessage(blueFire ? "Радон бросил синий Молотов." : "Радон бросил коктейль Молотова.", 2.0f);
    AddEventMessage(blueFire ? "Синий огонь Радона горит в 2 раза горячее." : "Огненная область Радона создана.", blueFire ? Color { 92, 164, 255, 255 } : Color { 255, 118, 70, 255 }, 2.4f);
    audio_.PlayBreakBlock();
}

void Game::UseRadonDestroyedCoreUltimate(Player& player)
{
    SetHeroAnimation(player, HeroAnimationState::Ultimate, 0.72f);
    EmitRadonCoreWave(player.GetPosition(), player.GetTeamId(), player.GetId(), 5.8f, 28.0f, 8.2f);
    SetMessage("Радон выпустил нестабильную энергию разрушенного Кора.", 2.6f);
    AddEventMessage("Ульта Радона: нестабильная энергия Кора.", HeroAccentColor(HeroId::Radon), 3.0f);
    audio_.PlayCoreDestroyed();
}

bool Game::TryRadonCoreSacrifice(EnergyCore& core)
{
    for (Player& player : players_)
    {
        HeroRuntimeState& heroState = player.MutableHeroState();
        if (player.GetTeamId() != core.GetTeamId()
            || player.GetHeroId() != HeroId::Radon
            || !player.IsAlive()
            || player.IsEliminated()
            || !heroState.ultimatePrimed
            || !heroState.ultimateReady
            || heroState.ultimate.cooldownRemaining > 0.0f)
        {
            continue;
        }

        core.SetHealth(20);
        Team* team = FindTeam(core.GetTeamId());
        if (team != nullptr)
        {
            team->coreAlive = true;
        }
        player.StartHeroAbilityCooldown(HeroAbilitySlot::Ultimate, 20.0f, 0.0f);
        SetHeroAnimation(player, HeroAnimationState::DeathSacrifice, 0.85f);
        player.KillWithRespawn(kRadonSacrificeRespawnSeconds);
        const Vector3 corePosition = world_.GridToWorld(core.GetBlockPosition());
        EmitRadonCoreWave(corePosition, player.GetTeamId(), player.GetId(), 7.2f, 0.0f, 9.4f);
        AddWorldEffect(corePosition, player.Forward(), HeroAccentColor(HeroId::Radon), 1.8f, 1.05f, WorldEffectKind::Sacrifice);
        AddEventMessage("Радон принял разрушение Кора на себя. Кор оставлен на 20 HP.", HeroAccentColor(HeroId::Radon), 5.0f);
        AddKillFeed("Радон спас Кор ценой жизни", HeroAccentColor(HeroId::Radon), 6.0f);
        return true;
    }
    return false;
}

void Game::EmitRadonCoreWave(Vector3 position, int ownerTeamId, int ownerPlayerId, float radius, float damage, float force)
{
    for (Player& target : players_)
    {
        if (!target.IsAlive() || target.IsEliminated() || target.GetTeamId() == ownerTeamId)
        {
            continue;
        }
        const float distance = std::sqrt(DistanceSquared(target.GetPosition(), position));
        if (distance > radius)
        {
            continue;
        }

        const float fraction = 1.0f - std::clamp(distance / std::max(0.1f, radius), 0.0f, 1.0f);
        if (damage > 0.0f)
        {
            const int scaledDamage = std::max(4, static_cast<int>(damage * (0.55f + fraction * 0.45f) + 0.5f));
            NoteDamageCredit(target.GetId(), ownerPlayerId, "взрывом Радона");
            target.Damage(scaledDamage);
        }
        const Vector3 away = Normalize2D(Vector3 { target.GetPosition().x - position.x, 0.0f, target.GetPosition().z - position.z });
        const float scaledForce = force * (0.55f + fraction * 0.45f) * BiomeKnockbackMultiplier();
        target.ApplyKnockback(Vector3 { away.x * scaledForce, 1.05f + fraction * 0.75f, away.z * scaledForce });
        AddFloatingText(damage > 0.0f ? "нестабильно" : "волна", target.GetPosition(), HeroAccentColor(HeroId::Radon));
    }

    AddWorldEffect(position, Vector3 { 0.0f, 0.0f, 1.0f }, HeroAccentColor(HeroId::Radon), radius, 0.70f, WorldEffectKind::Ring);
    AddCameraShake(0.28f, 0.28f);
}

bool Game::UseOrbitaAbility(Player& player, HeroAbilitySlot slot)
{
    const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Orbita);
    const HeroAbilityDefinition* ability = nullptr;
    const HeroAbilityState* state = nullptr;
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        ability = &hero.active1;
        state = &player.GetHeroState().active1;
        break;
    case HeroAbilitySlot::Active2:
        ability = &hero.active2;
        state = &player.GetHeroState().active2;
        break;
    case HeroAbilitySlot::Ultimate:
        ability = &hero.ultimate;
        state = &player.GetHeroState().ultimate;
        break;
    }

    if (ability == nullptr || state == nullptr)
    {
        return false;
    }
    if (state->cooldownRemaining > 0.0f)
    {
        SetMessage(hero.name + ": " + ability->name + " на кулдауне еще "
            + FormatTenths(state->cooldownRemaining) + " с.", 1.7f);
        audio_.PlayDenied();
        return false;
    }

    HeroRuntimeState& heroState = player.MutableHeroState();
    if (slot == HeroAbilitySlot::Active1)
    {
        player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
        UseOrbitaDash(player);
        return true;
    }

    if (slot == HeroAbilitySlot::Active2)
    {
        if (!UseOrbitaPhantomBlocks(player))
        {
            audio_.PlayDenied();
            return false;
        }

        player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
        return true;
    }

    if (!heroState.ultimateReady)
    {
        SetMessage("Орбита: ульта не готова, заряд "
            + std::to_string(static_cast<int>(heroState.ultimateCharge)) + "%.", 1.8f);
        audio_.PlayDenied();
        return false;
    }
    if (!heroState.orbitaTeleportPrimed)
    {
        const OrbitaTeleportPreview preview = BuildOrbitaTeleportPreview(player);
        if (!preview.visible || !preview.valid)
        {
            audio_.PlayDenied();
            return false;
        }
        heroState.orbitaTeleportPrimed = true;
        heroState.orbitaTeleportPreviewTimer = 4.0f;
        heroState.orbitaTeleportDestination = preview.destination;
        heroState.orbitaTeleportDirection = preview.direction;
        heroState.orbitaTeleportDistance = preview.travelDistance;
        heroState.orbitaTeleportHealthCost = preview.healthCost;
        AddWorldEffect(preview.destination, preview.direction, HeroAccentColor(HeroId::Orbita), 0.82f, 4.0f, WorldEffectKind::Ring);
        AddFloatingText("ТЕЛЕПОРТ?", preview.destination, HeroAccentColor(HeroId::Orbita));
        SetMessage("Орбита: точка отмечена. Нажмите ульту еще раз в течение 4 секунд.", 3.0f);
        audio_.PlayPickup();
        return true;
    }
    if (!UseOrbitaTeleport(player))
    {
        audio_.PlayDenied();
        return false;
    }

    player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
    return true;
}

void Game::UseOrbitaDash(Player& player)
{
    Vector3 forward = player.IsLocal() ? cameraController_.GetFlatForward() : player.Forward();
    forward = Normalize2D(forward);
    if (Length2D(forward) <= 0.0001f)
    {
        forward = player.Forward();
    }

    HeroRuntimeState& heroState = player.MutableHeroState();
    heroState.orbitaMomentumStrike = true;
    heroState.orbitaPulseTimer = 1.2f;
    heroState.orbitaDashRemaining = kOrbitaDashDistance;
    heroState.orbitaDashDirection = forward;
    heroState.orbitaDashLiftRemaining = kOrbitaDashLift;
    player.AddHeroUltimateCharge(7.0f);
    SetHeroAnimation(player, HeroAnimationState::Ability1, 0.28f);

    const Color accent = HeroAccentColor(HeroId::Orbita);
    AddWorldEffect(player.GetPosition(), forward, accent, 1.15f, 0.34f, WorldEffectKind::Trail);
    AddWorldEffect(player.GetPosition(), forward, accent, 0.72f, 0.36f, WorldEffectKind::Ring);
    AddFloatingText("дэш", Vector3 { player.GetPosition().x, player.GetPosition().y + 1.2f, player.GetPosition().z }, accent);
    SetMessage("Орбита делает дэш вперед. Следующий удар получил разгонный импульс.", 2.2f);
    AddEventMessage("Орбита: разгонный импульс готов для следующего удара.", accent, 2.4f);
    audio_.PlayPickup();
}

bool Game::UseOrbitaPhantomBlocks(Player& player)
{
    Vector3 forward = player.IsLocal() ? cameraController_.GetFlatForward() : player.Forward();
    forward = Normalize2D(forward);
    if (Length2D(forward) <= 0.0001f)
    {
        forward = player.Forward();
    }

    const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Orbita);
    const Vector3 playerPosition = player.GetPosition();
    const GridPos underFeet = world_.WorldToGrid(Vector3 { playerPosition.x, playerPosition.y - 1.05f, playerPosition.z });
    std::vector<GridPos> placedPositions;
    placedPositions.reserve(8);

    for (int i = 1; i <= 8; ++i)
    {
        const Vector3 projected {
            playerPosition.x + forward.x * static_cast<float>(i),
            static_cast<float>(underFeet.y),
            playerPosition.z + forward.z * static_cast<float>(i)
        };
        const GridPos pos = world_.WorldToGrid(projected);
        if (std::find(placedPositions.begin(), placedPositions.end(), pos) != placedPositions.end())
        {
            continue;
        }
        if (pos.y < kBuildMinY || pos.y > kBuildMaxY
            || std::abs(pos.x) > kBuildMapRadius
            || std::abs(pos.z) > kBuildMapRadius
            || !world_.IsAir(pos)
            || WouldBlockOverlapPlayer(pos, player.GetId())
            || IsOrbitaCoreRestrictedPosition(world_.GridToWorld(pos), player.GetTeamId()))
        {
            continue;
        }

        if (PlaceWorldBlock(pos, Block { BlockType::EnergyGlassBlock, player.GetTeamId(), true }, false, BlockDeltaReason::TemporaryPlace, player.GetId()))
        {
            placedPositions.push_back(pos);
            heroTemporaryBlocks_.push_back(HeroTemporaryBlock {
                pos,
                player.GetTeamId(),
                std::max(0.1f, hero.active2.durationSeconds
                    + static_cast<float>(placedPositions.size() - 1) * 0.12f)
            });
            AddWorldEffect(world_.GridToWorld(pos), HeroAccentColor(HeroId::Orbita), 0.22f, 0.28f);
        }
    }

    if (placedPositions.empty())
    {
        SetMessage("Орбита: фантомные блоки не нашли свободной безопасной линии.", 2.0f);
        return false;
    }

    HeroRuntimeState& heroState = player.MutableHeroState();
    heroState.orbitaPulseTimer = hero.active2.durationSeconds;
    SetHeroAnimation(player, HeroAnimationState::Ability2, 0.32f);
    const Color accent = HeroAccentColor(HeroId::Orbita);
    AddWorldEffect(player.GetPosition(), forward, accent, 1.45f, 0.42f, WorldEffectKind::Cone);
    AddFloatingText("фантом x" + std::to_string(static_cast<int>(placedPositions.size())),
        Vector3 { player.GetPosition().x, player.GetPosition().y + 1.25f, player.GetPosition().z },
        accent);
    SetMessage("Орбита выставила фантомные блоки на 3 секунды: " + std::to_string(static_cast<int>(placedPositions.size())) + ".", 2.4f);
    AddEventMessage("Фантомные блоки Орбиты постепенно исчезают и не защищают чужой Кор.", accent, 2.8f);
    audio_.PlayBuild();
    return true;
}

OrbitaTeleportPreview Game::BuildOrbitaTeleportPreview(const Player& player) const
{
    OrbitaTeleportPreview preview {};
    if (player.GetHeroId() != HeroId::Orbita || !player.IsAlive() || player.IsEliminated())
    {
        return preview;
    }

    const HeroRuntimeState& heroState = player.GetHeroState();
    if (!heroState.ultimateReady || heroState.ultimate.cooldownRemaining > 0.0f)
    {
        return preview;
    }

    preview.visible = true;
    if (heroState.orbitaTeleportPrimed && heroState.orbitaTeleportPreviewTimer > 0.0f)
    {
        preview.start = player.IsLocal()
            ? cameraController_.GetAimOrigin()
            : Vector3 { player.GetPosition().x, player.GetPosition().y + 0.78f, player.GetPosition().z };
        preview.destination = heroState.orbitaTeleportDestination;
        preview.direction = heroState.orbitaTeleportDirection;
        preview.travelDistance = heroState.orbitaTeleportDistance;
        preview.healthCost = heroState.orbitaTeleportHealthCost;
        preview.valid = IsOrbitaTeleportDestinationSafe(preview.destination, player.GetTeamId(), &preview.reason);
        return preview;
    }
    const Vector3 origin = player.IsLocal()
        ? cameraController_.GetAimOrigin()
        : Vector3 { player.GetPosition().x, player.GetPosition().y + 0.78f, player.GetPosition().z };
    Vector3 direction = player.IsLocal() ? cameraController_.GetAimDirection() : player.Forward();
    direction = Normalize(direction);
    if (Length(direction) <= 0.0001f)
    {
        direction = player.Forward();
    }
    preview.start = origin;
    preview.direction = direction;

    const auto fail = [&preview](std::string reason, Vector3 destination, float travelDistance)
    {
        preview.destination = destination;
        preview.travelDistance = travelDistance;
        preview.healthCost = 0;
        preview.reason = std::move(reason);
        preview.valid = false;
        return preview;
    };

    const std::optional<RaycastHit> hit = world_.Raycast(origin, direction, kOrbitaTeleportMaxDistance);
    Vector3 destination {};
    float travelDistance = kOrbitaTeleportMaxDistance;
    if (hit.has_value())
    {
        const Vector3 hitPoint {
            origin.x + direction.x * hit->distance,
            origin.y + direction.y * hit->distance,
            origin.z + direction.z * hit->distance
        };
        if (hit->distance < 1.1f)
        {
            return fail("точка телепорта слишком близко к препятствию.", hitPoint, hit->distance);
        }
        if (hit->normal.y < 0)
        {
            return fail("точка над головой закрыта.", hitPoint, hit->distance);
        }

        const Vector3 hitBlock = world_.GridToWorld(hit->block);
        if (hit->normal.y > 0)
        {
            destination = Vector3 { hitBlock.x, hitBlock.y + 1.5f, hitBlock.z };
        }
        else
        {
            const Vector3 adjacent = world_.GridToWorld(hit->adjacent);
            destination = Vector3 { adjacent.x, hitBlock.y + 1.5f, adjacent.z };
        }
        travelDistance = hit->distance;
    }
    else
    {
        const Vector3 flat = Normalize2D(direction);
        if (Length2D(flat) <= 0.0001f)
        {
            return fail("нужна видимая точка впереди.", player.GetPosition(), 0.0f);
        }
        destination = Vector3 {
            player.GetPosition().x + flat.x * kOrbitaTeleportMaxDistance,
            player.GetPosition().y,
            player.GetPosition().z + flat.z * kOrbitaTeleportMaxDistance
        };
    }

    std::string reason;
    if (!IsOrbitaTeleportDestinationSafe(destination, player.GetTeamId(), &reason))
    {
        return fail(reason, destination, travelDistance);
    }

    preview.destination = destination;
    preview.travelDistance = travelDistance;
    preview.healthCost = std::clamp(static_cast<int>(travelDistance * kOrbitaTeleportDamagePerBlock + 0.5f), 4, 28);
    preview.valid = true;
    return preview;
}

bool Game::UseOrbitaTeleport(Player& player)
{
    HeroRuntimeState& state = player.MutableHeroState();
    if (!state.orbitaTeleportPrimed || state.orbitaTeleportPreviewTimer <= 0.0f)
    {
        return false;
    }

    std::string reason;
    if (!IsOrbitaTeleportDestinationSafe(state.orbitaTeleportDestination, player.GetTeamId(), &reason))
    {
        state.orbitaTeleportPrimed = false;
        state.orbitaTeleportPreviewTimer = 0.0f;
        SetMessage("Орбита: телепорт отменен. " + reason, 2.0f);
        return false;
    }

    const Vector3 start = player.GetPosition();
    const Vector3 destination = state.orbitaTeleportDestination;
    const Vector3 direction = state.orbitaTeleportDirection;
    const int selfDamage = state.orbitaTeleportHealthCost;
    state.orbitaTeleportPrimed = false;
    state.orbitaTeleportPreviewTimer = 0.0f;
    player.Teleport(destination);
    player.Damage(selfDamage);
    player.MutableHeroState().orbitaPulseTimer = 0.8f;
    SetHeroAnimation(player, HeroAnimationState::Ultimate, 0.36f);

    const Color accent = HeroAccentColor(HeroId::Orbita);
    AddWorldEffect(start, direction, accent, 0.85f, 0.36f, WorldEffectKind::Ring);
    AddWorldEffect(destination, direction, accent, 1.05f, 0.42f, WorldEffectKind::Ring);
    AddFloatingText("-" + std::to_string(selfDamage), Vector3 { destination.x, destination.y + 1.35f, destination.z }, accent);
    SetMessage("Орбита телепортировалась в видимую точку и получила " + std::to_string(selfDamage) + " урона.", 2.5f);
    AddEventMessage("Ульта Орбиты: видимый телепорт завершен.", accent, 2.8f);
    AddCameraShake(0.18f, 0.18f);
    audio_.PlayCoreDestroyed();
    return true;
}

bool Game::IsOrbitaCoreRestrictedPosition(Vector3 position, int teamId) const
{
    for (const EnergyCore& core : matchSimulation_.Cores())
    {
        if (!core.IsAlive() || core.GetTeamId() == teamId)
        {
            continue;
        }
        if (DistanceSquared(position, world_.GridToWorld(core.GetBlockPosition())) <= kOrbitaEnemyCoreRestrictionSq)
        {
            return true;
        }
    }
    return false;
}

bool Game::IsOrbitaTeleportDestinationSafe(Vector3 position, int teamId, std::string* reason) const
{
    const auto fail = [reason](const std::string& text)
    {
        if (reason != nullptr)
        {
            *reason = text;
        }
        return false;
    };

    if (position.y < static_cast<float>(kBuildMinY) || position.y > static_cast<float>(kBuildMaxY + 2))
    {
        return fail("Высота недоступна.");
    }
    if (std::abs(position.x) > static_cast<float>(kBuildMapRadius)
        || std::abs(position.z) > static_cast<float>(kBuildMapRadius))
    {
        return fail("Точка вне карты.");
    }
    if (IsOrbitaCoreRestrictedPosition(position, teamId))
    {
        return fail("Слишком близко к чужому Кору.");
    }
    if (world_.CollidesWithAABB(position, kPlayerCollisionHalfExtents))
    {
        return fail("Точка занята блоками.");
    }
    return true;
}

std::vector<HeroDeviceVisual> Game::BuildHeroDeviceVisuals() const
{
    std::vector<HeroDeviceVisual> devices;
    devices.reserve(bromVacuumBots_.size() + bromTurretDrones_.size() + svidetelEchoes_.size());

    for (const BromVacuumBot& bot : bromVacuumBots_)
    {
        const Team* team = FindTeam(bot.ownerTeamId);
        const Vector3 basePosition = team != nullptr
            ? Vector3 { team->shopPosition.x, team->shopPosition.y + 0.35f, team->shopPosition.z }
            : bot.position;
        Vector3 direction = Normalize(Vector3 {
            basePosition.x - bot.position.x,
            basePosition.y - bot.position.y,
            basePosition.z - bot.position.z
        });
        if (Length(direction) <= 0.0001f)
        {
            direction = Vector3 { 0.0f, 0.0f, 1.0f };
        }

        HeroDeviceVisual visual {};
        visual.kind = HeroDeviceVisualKind::BromVacuumBot;
        visual.position = bot.position;
        visual.target = basePosition;
        visual.direction = direction;
        visual.teamId = bot.ownerTeamId;
        visual.cargoUnits = BromCargoUnits(bot.cargo);
        visual.cargoCapacity = kBromVacuumCapacity;
        visual.temporary = bot.temporary;
        visual.returning = bot.returning;
        visual.active = bot.returning || visual.cargoUnits > 0;
        visual.lifetimeFraction = bot.temporary
            ? std::clamp(bot.lifetime / kBromUltimateDeviceLifetime, 0.0f, 1.0f)
            : 1.0f;
        devices.push_back(visual);
    }

    for (const BromTurretDrone& drone : bromTurretDrones_)
    {
        Vector3 target = drone.shotFlashTimer > 0.0f ? drone.lastShotTarget : Vector3 {
            drone.position.x + 0.0f,
            drone.position.y,
            drone.position.z + 1.0f
        };
        Vector3 direction = Normalize(Vector3 {
            target.x - drone.position.x,
            target.y - drone.position.y,
            target.z - drone.position.z
        });
        if (Length(direction) <= 0.0001f)
        {
            direction = Vector3 { 0.0f, 0.0f, 1.0f };
        }

        HeroDeviceVisual visual {};
        visual.kind = HeroDeviceVisualKind::BromTurretDrone;
        visual.position = drone.position;
        visual.target = target;
        visual.direction = direction;
        visual.teamId = drone.ownerTeamId;
        visual.temporary = drone.temporary;
        visual.active = drone.shotFlashTimer > 0.0f;
        visual.lifetimeFraction = drone.temporary
            ? std::clamp(drone.lifetime / kBromUltimateDeviceLifetime, 0.0f, 1.0f)
            : 1.0f;
        devices.push_back(visual);
    }

    const auto playerById = [this](int playerId) -> const Player*
    {
        for (const Player& player : players_)
        {
            if (player.GetId() == playerId)
            {
                return &player;
            }
        }
        return nullptr;
    };
    const HeroDefinition& konvoy = HeroSystem::GetDefinition(HeroId::Konvoy);
    for (const KonvoyTrap& trap : konvoyTraps_)
    {
        HeroDeviceVisual visual {};
        visual.kind = HeroDeviceVisualKind::KonvoyTrap;
        visual.position = trap.position;
        visual.teamId = trap.ownerTeamId;
        visual.active = trap.flashTimer > 0.0f;
        visual.radius = 0.72f;
        visual.lifetimeFraction = std::clamp(trap.lifetime / std::max(0.1f, konvoy.active1.durationSeconds), 0.0f, 1.0f);
        devices.push_back(visual);
    }

    for (const KonvoyTether& tether : konvoyTethers_)
    {
        const Player* owner = playerById(tether.ownerPlayerId);
        const Player* target = playerById(tether.targetPlayerId);
        if (owner == nullptr || target == nullptr)
        {
            continue;
        }

        HeroDeviceVisual visual {};
        visual.kind = HeroDeviceVisualKind::KonvoyTether;
        visual.position = Vector3 { owner->GetPosition().x, owner->GetPosition().y + 0.62f, owner->GetPosition().z };
        visual.target = Vector3 { target->GetPosition().x, target->GetPosition().y + 0.72f, target->GetPosition().z };
        visual.teamId = tether.ownerTeamId;
        visual.active = tether.flashTimer > 0.0f;
        visual.radius = kKonvoyHandcuffRadius;
        visual.lifetimeFraction = std::clamp(tether.lifetime / std::max(0.1f, konvoy.active2.durationSeconds), 0.0f, 1.0f);
        devices.push_back(visual);
    }

    for (const KonvoyDome& dome : konvoyDomes_)
    {
        HeroDeviceVisual visual {};
        visual.kind = HeroDeviceVisualKind::KonvoyDome;
        visual.position = dome.position;
        visual.teamId = dome.ownerTeamId;
        visual.active = dome.flashTimer > 0.0f;
        visual.radius = kKonvoyDomeVisualRadius;
        visual.lifetimeFraction = std::clamp(dome.lifetime / std::max(0.1f, konvoy.ultimate.durationSeconds), 0.0f, 1.0f);
        devices.push_back(visual);
    }

    for (const SvidetelEcho& echo : svidetelEchoes_)
    {
        HeroDeviceVisual visual {};
        visual.kind = HeroDeviceVisualKind::SvidetelEcho;
        visual.position = echo.position;
        visual.target = echo.lastTarget;
        visual.teamId = echo.ownerTeamId;
        visual.active = echo.flashTimer > 0.0f;
        visual.temporary = !echo.armed;
        visual.radius = echo.armed ? 0.72f : 0.54f;
        visual.lifetimeFraction = std::clamp(echo.lifetime / (echo.armed ? 24.0f : 12.0f), 0.0f, 1.0f);
        devices.push_back(visual);
    }

    return devices;
}

bool Game::UseBromAbility(Player& player, HeroAbilitySlot slot)
{
    const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Brom);
    const HeroAbilityDefinition* ability = nullptr;
    const HeroAbilityState* state = nullptr;
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        ability = &hero.active1;
        state = &player.GetHeroState().active1;
        break;
    case HeroAbilitySlot::Active2:
        ability = &hero.active2;
        state = &player.GetHeroState().active2;
        break;
    case HeroAbilitySlot::Ultimate:
        ability = &hero.ultimate;
        state = &player.GetHeroState().ultimate;
        break;
    }

    if (ability == nullptr || state == nullptr)
    {
        return false;
    }
    if (state->cooldownRemaining > 0.0f)
    {
        SetMessage(hero.name + ": " + ability->name + " на кулдауне еще "
            + FormatTenths(state->cooldownRemaining) + " с.", 1.7f);
        audio_.PlayDenied();
        return false;
    }

    if (slot == HeroAbilitySlot::Active1)
    {
        if (!UseBromVacuumBot(player, false, 0.0f))
        {
            audio_.PlayDenied();
            return false;
        }
        player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
        return true;
    }
    if (slot == HeroAbilitySlot::Active2)
    {
        if (!UseBromTurretDrone(player, false, 0.0f))
        {
            audio_.PlayDenied();
            return false;
        }
        player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
        return true;
    }

    if (!player.GetHeroState().ultimateReady)
    {
        SetMessage("Бром: ульта не готова, заряд "
            + std::to_string(static_cast<int>(player.GetHeroState().ultimateCharge)) + "%.", 1.8f);
        audio_.PlayDenied();
        return false;
    }
    if (!UseBromUltimate(player))
    {
        audio_.PlayDenied();
        return false;
    }
    player.StartHeroAbilityCooldown(HeroAbilitySlot::Ultimate, kBromUltimateCooldownSeconds, kBromUltimateDeviceLifetime);
    return true;
}

bool Game::UseBromVacuumBot(Player& player, bool temporary, float lifetimeSeconds)
{
    if (!temporary)
    {
        const int activeCount = static_cast<int>(std::count_if(
            bromVacuumBots_.begin(),
            bromVacuumBots_.end(),
            [&player](const BromVacuumBot& bot)
            {
                return !bot.temporary && bot.ownerPlayerId == player.GetId();
            }));
        if (activeCount >= 2)
        {
            SetMessage("Бром: одновременно могут работать только два робота-пылесоса.", 2.0f);
            return false;
        }
        if (!player.GetInventory().SpendResource(ResourceType::Iron, kBromVacuumIronCost))
        {
            SetMessage("Бром: для робота-пылесоса нужно 48 железа.", 2.0f);
            return false;
        }
    }

    const int spawnIndex = static_cast<int>(bromVacuumBots_.size() + bromTurretDrones_.size());
    BromVacuumBot bot {};
    bot.position = OffsetAround(player.GetPosition(), spawnIndex, 1.35f);
    bot.ownerPlayerId = player.GetId();
    bot.ownerTeamId = player.GetTeamId();
    bot.temporary = temporary;
    bot.lifetime = temporary ? std::max(0.1f, lifetimeSeconds) : 0.0f;
    bot.pulseTimer = 0.4f;
    bot.invulnerabilityTimer = temporary ? 5.0f : 0.0f;
    bromVacuumBots_.push_back(bot);

    player.AddHeroUltimateCharge(temporary ? 0.0f : 8.0f);
    SetHeroAnimation(player, temporary ? HeroAnimationState::Ultimate : HeroAnimationState::Ability1, 0.34f);
    const Color accent = HeroAccentColor(HeroId::Brom);
    AddWorldEffect(bot.position, accent, temporary ? 0.32f : 0.26f, 0.34f);
    AddFloatingText(temporary ? "временный пылесос" : "робот-пылесос", bot.position, accent);
    SetMessage(temporary ? "Бром собрал временного робота-пылесоса." : "Бром собрал робота-пылесоса за 48 железа.", 2.2f);
    AddEventMessage("Робот-пылесос Брома ищет ресурсы и несет их в командный сундук.", accent, 2.8f);
    audio_.PlayBuild();
    return true;
}

bool Game::UseBromTurretDrone(Player& player, bool temporary, float lifetimeSeconds)
{
    if (!temporary)
    {
        const int activeCount = static_cast<int>(std::count_if(
            bromTurretDrones_.begin(),
            bromTurretDrones_.end(),
            [&player](const BromTurretDrone& drone)
            {
                return !drone.temporary && drone.ownerPlayerId == player.GetId();
            }));
        if (activeCount >= 1)
        {
            SetMessage("Бром: одновременно может работать только один дрон-турель.", 2.0f);
            return false;
        }
        if (!player.GetInventory().SpendResource(ResourceType::Gold, kBromTurretGoldCost))
        {
            SetMessage("Бром: для дрона-турели нужно 12 золота.", 2.0f);
            return false;
        }
    }

    const int spawnIndex = static_cast<int>(bromVacuumBots_.size() + bromTurretDrones_.size());
    BromTurretDrone drone {};
    drone.position = OffsetAround(Vector3 { player.GetPosition().x, player.GetPosition().y + 1.05f, player.GetPosition().z }, spawnIndex, 1.65f);
    drone.ownerPlayerId = player.GetId();
    drone.ownerTeamId = player.GetTeamId();
    drone.temporary = temporary;
    drone.lifetime = temporary ? std::max(0.1f, lifetimeSeconds) : 0.0f;
    drone.fireCooldown = 0.4f;
    drone.pulseTimer = 0.4f;
    drone.invulnerabilityTimer = temporary ? 5.0f : 0.0f;
    bromTurretDrones_.push_back(drone);

    player.AddHeroUltimateCharge(temporary ? 0.0f : 10.0f);
    SetHeroAnimation(player, temporary ? HeroAnimationState::Ultimate : HeroAnimationState::Ability2, 0.34f);
    const Color accent = HeroAccentColor(HeroId::Brom);
    AddWorldEffect(drone.position, accent, temporary ? 0.36f : 0.30f, 0.34f);
    AddFloatingText(temporary ? "временная турель" : "дрон-турель", drone.position, accent);
    SetMessage(temporary ? "Бром собрал временного дрона-турель." : "Бром собрал дрона-турель за 12 золота.", 2.2f);
    AddEventMessage("Дрон-турель Брома стреляет только по игрокам и не атакует Кор.", accent, 2.8f);
    audio_.PlayBuild();
    return true;
}

bool Game::UseBromUltimate(Player& player)
{
    constexpr float absorbRadiusSq = 7.0f * 7.0f;
    int absorbed = 0;
    for (ResourcePickup& pickup : matchSimulation_.Pickups())
    {
        const Vector3 pickupPos = ToVector3(pickup.position);
        if (pickup.collected || DistanceSquared(pickupPos, player.GetPosition()) > absorbRadiusSq)
        {
            continue;
        }

        player.GetInventory().AddResource(pickup.type, pickup.amount);
        absorbed += pickup.amount;
        pickup.collected = true;
        AddWorldEffect(pickupPos, HeroAccentColor(HeroId::Brom), 0.18f, 0.18f);
    }

    const int turretCount = std::min(5, player.GetInventory().GetResource(ResourceType::Gold) / kBromTurretGoldCost);
    if (absorbed == 0 && turretCount == 0)
    {
        SetMessage("Бром: для Сборки обороны нужны ресурсы рядом или в инвентаре.", 2.2f);
        return false;
    }

    int spawned = 0;
    for (int i = 0; i < turretCount; ++i)
    {
        if (player.GetInventory().SpendResource(ResourceType::Gold, kBromTurretGoldCost)
            && UseBromTurretDrone(player, true, kBromUltimateDeviceLifetime))
        {
            ++spawned;
        }
    }

    if (spawned <= 0 && absorbed <= 0)
    {
        SetMessage("Бром: ресурсы не удалось превратить в устройства.", 2.0f);
        return false;
    }

    const Color accent = HeroAccentColor(HeroId::Brom);
    SetHeroAnimation(player, HeroAnimationState::Ultimate, 0.58f);
    AddWorldEffect(player.GetPosition(), player.Forward(), accent, 2.2f, 0.65f, WorldEffectKind::Ring);
    SetMessage("Бром запустил Сборку обороны: устройств " + std::to_string(spawned) + ", время 1.5 минуты.", 3.0f);
    AddEventMessage("Ульта Брома собрала защиту из ресурсов. Поглощено рядом: " + std::to_string(absorbed) + ".", accent, 3.2f);
    audio_.PlayPurchase();
    return true;
}

bool Game::UseKonvoyAbility(Player& player, HeroAbilitySlot slot)
{
    const HeroDefinition& hero = HeroSystem::GetDefinition(HeroId::Konvoy);
    const HeroAbilityDefinition* ability = nullptr;
    const HeroAbilityState* state = nullptr;
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        ability = &hero.active1;
        state = &player.GetHeroState().active1;
        break;
    case HeroAbilitySlot::Active2:
        ability = &hero.active2;
        state = &player.GetHeroState().active2;
        break;
    case HeroAbilitySlot::Ultimate:
        ability = &hero.ultimate;
        state = &player.GetHeroState().ultimate;
        break;
    }

    if (ability == nullptr || state == nullptr)
    {
        return false;
    }
    if (state->cooldownRemaining > 0.0f)
    {
        SetMessage(hero.name + ": " + ability->name + " на кулдауне еще "
            + FormatTenths(state->cooldownRemaining) + " с.", 1.7f);
        audio_.PlayDenied();
        return false;
    }

    bool used = false;
    if (slot == HeroAbilitySlot::Active1)
    {
        used = UseKonvoyTrap(player, ability->durationSeconds);
    }
    else if (slot == HeroAbilitySlot::Active2)
    {
        used = UseKonvoyHandcuffs(player, ability->durationSeconds);
    }
    else
    {
        if (!player.GetHeroState().ultimateReady)
        {
            SetMessage("Конвой: ульта не готова, заряд "
                + std::to_string(static_cast<int>(player.GetHeroState().ultimateCharge)) + "%.", 1.8f);
            audio_.PlayDenied();
            return false;
        }
        used = UseKonvoyDome(player, ability->durationSeconds);
    }

    if (!used)
    {
        audio_.PlayDenied();
        return false;
    }

    player.StartHeroAbilityCooldown(slot, ability->cooldownSeconds, ability->durationSeconds);
    return true;
}

bool Game::UseKonvoyTrap(Player& player, float lifetimeSeconds)
{
    const int activeCount = static_cast<int>(std::count_if(
        konvoyTraps_.begin(),
        konvoyTraps_.end(),
        [&player](const KonvoyTrap& trap)
        {
            return trap.ownerPlayerId == player.GetId();
        }));
    if (activeCount >= kKonvoyMaxTraps)
    {
        SetMessage("Конвой: одновременно может быть до 2 капканов.", 2.0f);
        return false;
    }

    Vector3 forward = player.IsLocal() ? cameraController_.GetFlatForward() : player.Forward();
    forward = Normalize2D(forward);
    if (Length2D(forward) <= 0.0001f)
    {
        forward = player.Forward();
    }

    const Vector3 desired {
        player.GetPosition().x + forward.x * 1.15f,
        player.GetPosition().y,
        player.GetPosition().z + forward.z * 1.15f
    };
    const GridPos column = world_.WorldToGrid(desired);
    std::optional<Vector3> trapPosition;
    const int startY = world_.WorldToGrid(player.GetPosition()).y + 1;
    for (int y = startY; y >= startY - 3; --y)
    {
        const GridPos ground { column.x, y, column.z };
        const GridPos above { column.x, y + 1, column.z };
        if (world_.IsSolid(ground) && world_.IsAir(above))
        {
            const Vector3 groundCenter = world_.GridToWorld(ground);
            trapPosition = Vector3 { desired.x, groundCenter.y + 0.57f, desired.z };
            break;
        }
    }

    if (!trapPosition.has_value())
    {
        SetMessage("Конвой: капкану нужна свободная поверхность рядом.", 2.0f);
        return false;
    }

    KonvoyTrap trap {};
    trap.position = *trapPosition;
    trap.ownerPlayerId = player.GetId();
    trap.ownerTeamId = player.GetTeamId();
    trap.lifetime = std::max(0.1f, lifetimeSeconds);
    trap.flashTimer = 0.45f;
    konvoyTraps_.push_back(trap);

    const Color accent = HeroAccentColor(HeroId::Konvoy);
    SetHeroAnimation(player, HeroAnimationState::Ability1, 0.30f);
    AddWorldEffect(trap.position, forward, accent, 0.52f, 0.38f, WorldEffectKind::Ring);
    AddFloatingText("капкан", trap.position, accent);
    SetMessage("Конвой поставил капкан. Время существования: 45 секунд.", 2.5f);
    AddEventMessage("Капкан Конвоя заметен: его можно обойти или сломать инструментом.", accent, 2.8f);
    audio_.PlayBuild();
    return true;
}

bool Game::UseKonvoyHandcuffs(Player& player, float lifetimeSeconds)
{
    Player* target = FindNearbyEnemyPlayer(player, kKonvoyHandcuffRadius);
    if (target == nullptr)
    {
        SetMessage("Конвой: для наручников нужен противник в радиусе 6 блоков.", 2.0f);
        return false;
    }

    const Vector3 origin { player.GetPosition().x, player.GetPosition().y + 0.78f, player.GetPosition().z };
    const Vector3 targetPoint { target->GetPosition().x, target->GetPosition().y + 0.72f, target->GetPosition().z };
    const Vector3 ray {
        targetPoint.x - origin.x,
        targetPoint.y - origin.y,
        targetPoint.z - origin.z
    };
    const float distance = Length(ray);
    const std::optional<RaycastHit> wall = world_.Raycast(origin, ray, distance);
    if (wall.has_value() && wall->distance < distance - 0.35f)
    {
        SetMessage("Конвой: наручники не проходят через блоки.", 2.0f);
        return false;
    }

    KonvoyTether tether {};
    tether.ownerPlayerId = player.GetId();
    tether.targetPlayerId = target->GetId();
    tether.ownerTeamId = player.GetTeamId();
    tether.lifetime = std::max(0.1f, lifetimeSeconds);
    tether.flashTimer = 0.42f;
    tether.ownerLastHealth = player.GetHealth();
    konvoyTethers_.push_back(tether);
    player.AddHeroUltimateCharge(10.0f);

    const Color accent = HeroAccentColor(HeroId::Konvoy);
    SetHeroAnimation(player, HeroAnimationState::Ability2, 0.34f);
    const Vector3 tetherDirection = Normalize2D(Vector3 {
        target->GetPosition().x - player.GetPosition().x,
        0.0f,
        target->GetPosition().z - player.GetPosition().z
    });
    AddWorldEffect(player.GetPosition(), tetherDirection, accent, kKonvoyHandcuffRadius, 0.42f, WorldEffectKind::Trail);
    AddFloatingText("наручники", target->GetPosition(), accent);
    SetMessage("Конвой связал цель наручниками на 12 секунд. Радиус цепи: 6 блоков.", 2.6f);
    AddEventMessage("Наручники Конвоя притягивают цель и рвутся после накопленных 30+ урона по Конвою.", accent, 3.0f);
    audio_.PlayPickup();
    return true;
}

bool Game::UseKonvoyDome(Player& player, float lifetimeSeconds)
{
    KonvoyDome dome {};
    dome.position = player.GetPosition();
    dome.ownerPlayerId = player.GetId();
    dome.ownerTeamId = player.GetTeamId();
    dome.lifetime = std::max(0.1f, lifetimeSeconds);
    dome.flashTimer = 0.60f;
    for (const Player& target : players_)
    {
        if (target.IsAlive() && !target.IsEliminated()
            && target.GetTeamId() != player.GetTeamId()
            && DistanceSquared(target.GetPosition(), dome.position) < kKonvoyDomeVisualRadius * kKonvoyDomeVisualRadius)
        {
            dome.initiallyInsideEnemyIds.push_back(target.GetId());
        }
    }
    konvoyDomes_.push_back(dome);

    const Color accent = HeroAccentColor(HeroId::Konvoy);
    SetHeroAnimation(player, HeroAnimationState::Ultimate, 0.52f);
    AddWorldEffect(player.GetPosition(), Vector3 { 0.0f, 0.0f, 1.0f }, accent, kKonvoyDomeVisualRadius, 0.80f, WorldEffectKind::CorePulse);
    SetMessage("Конвой развернул Купол содержания на 30 секунд.", 2.8f);
    AddEventMessage("Купол содержания имеет 180 HP: враги не проходят сквозь оболочку, союзники проходят свободно.", accent, 3.2f);
    audio_.PlayPurchase();
    return true;
}

void Game::ApplyBromBlockBreakPassive(Player& player, const Block& block, Vector3 position)
{
    if (player.GetHeroId() != HeroId::Brom || block.teamId < 0 || block.teamId == player.GetTeamId())
    {
        return;
    }

    ResourceType reward = ResourceType::Iron;
    int chance = 28;
    int amount = 1;
    if (block.type == BlockType::StoneBlock || block.type == BlockType::ObsidianBlock)
    {
        reward = ResourceType::Gold;
        chance = block.type == BlockType::ObsidianBlock ? 24 : 18;
    }
    else if (block.type == BlockType::EnergyGlassBlock || block.type == BlockType::ExplosiveBlock)
    {
        reward = ResourceType::Iron;
        chance = 36;
    }

    if (GetRandomValue(1, 100) > chance)
    {
        return;
    }

    player.GetInventory().AddResource(reward, amount);
    player.AddHeroUltimateCharge(BromUltimateChargeForResource(reward, amount));
    const Color accent = HeroAccentColor(HeroId::Brom);
    AddFloatingText("+трофей " + std::to_string(amount) + " " + ToString(reward), position, accent);
    if (player.IsLocal())
    {
        AddEventMessage("Пассивка Брома разобрала вражеский блок на материалы.", accent, 2.2f);
    }
}
