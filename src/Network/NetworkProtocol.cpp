#include "Network/NetworkProtocol.h"

#include <cstring> // std::memcpy
#include <iostream>

// See NetworkProtocol.h / docs/NETWORK_PREP_PLAN.md for the encoding contract.
// This stays raylib-free and depends only on the snapshot/command value types.

const char* ToString(MessageType type)
{
    switch (type)
    {
    case MessageType::Invalid:
        return "Invalid";
    case MessageType::PlayerCommand:
        return "PlayerCommand";
    case MessageType::MatchSnapshot:
        return "MatchSnapshot";
    case MessageType::Connect:
        return "Connect";
    case MessageType::ConnectAck:
        return "ConnectAck";
    case MessageType::Disconnect:
        return "Disconnect";
    case MessageType::Heartbeat:
        return "Heartbeat";
    case MessageType::ConnectDenied:
        return "ConnectDenied";
    case MessageType::LobbyUpdate:
        return "LobbyUpdate";
    case MessageType::LobbySnapshot:
        return "LobbySnapshot";
    case MessageType::SnapshotDelta:
        return "SnapshotDelta";
    case MessageType::FullResyncRequest:
        return "FullResyncRequest";
    case MessageType::ReliableAck:
        return "ReliableAck";
    case MessageType::PlayerCommandBatch:
        return "PlayerCommandBatch";
    case MessageType::PacketFragment:
        return "PacketFragment";
    }
    return "Unknown";
}

const char* ToString(DecodeStatus status)
{
    switch (status)
    {
    case DecodeStatus::Ok:
        return "Ok";
    case DecodeStatus::TooShort:
        return "TooShort";
    case DecodeStatus::BadMagic:
        return "BadMagic";
    case DecodeStatus::VersionMismatch:
        return "VersionMismatch";
    case DecodeStatus::WrongType:
        return "WrongType";
    case DecodeStatus::BadPayload:
        return "BadPayload";
    }
    return "Unknown";
}

namespace
{
// Upper bound on any length-prefixed array so a corrupt/huge count can never make
// the decoder allocate or loop unboundedly (the bounds-checked reads would fail
// eventually, but we reject early and cheaply).
constexpr std::uint32_t kMaxArrayLen = 1u << 20;
constexpr std::uint32_t kMaxCommandBatchLen = 8;

// --- Little-endian byte writer ----------------------------------------------
class ByteWriter
{
public:
    void U8(std::uint8_t v) { bytes_.push_back(v); }
    void U16(std::uint16_t v)
    {
        U8(static_cast<std::uint8_t>(v & 0xFF));
        U8(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }
    void U32(std::uint32_t v)
    {
        U16(static_cast<std::uint16_t>(v & 0xFFFF));
        U16(static_cast<std::uint16_t>((v >> 16) & 0xFFFF));
    }
    void I32(std::int32_t v) { U32(static_cast<std::uint32_t>(v)); }
    void F32(float v)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        U32(bits);
    }
    void Bool(bool v) { U8(v ? 1u : 0u); }
    void Enum(int v) { I32(v); }
    void Str(const std::string& s)
    {
        U32(static_cast<std::uint32_t>(s.size()));
        for (char c : s)
        {
            U8(static_cast<std::uint8_t>(c));
        }
    }

    const std::vector<std::uint8_t>& Bytes() const { return bytes_; }
    std::vector<std::uint8_t> Take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

// --- Little-endian byte reader (bounds-checked; never reads out of range) ----
class ByteReader
{
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool Ok() const { return ok_; }
    std::size_t Remaining() const { return pos_ <= size_ ? size_ - pos_ : 0; }

    std::uint8_t U8()
    {
        if (Remaining() < 1)
        {
            ok_ = false;
            return 0;
        }
        return data_[pos_++];
    }
    std::uint16_t U16()
    {
        const std::uint16_t lo = U8();
        const std::uint16_t hi = U8();
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }
    std::uint32_t U32()
    {
        const std::uint32_t lo = U16();
        const std::uint32_t hi = U16();
        return lo | (hi << 16);
    }
    std::int32_t I32() { return static_cast<std::int32_t>(U32()); }
    float F32()
    {
        const std::uint32_t bits = U32();
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    bool Bool() { return U8() != 0; }
    int Enum() { return I32(); }
    std::string Str()
    {
        const std::uint32_t len = Count();
        std::string s;
        s.reserve(len);
        for (std::uint32_t i = 0; i < len && ok_; ++i)
        {
            s.push_back(static_cast<char>(U8()));
        }
        return s;
    }

    // Read a length prefix, rejecting absurd counts up front.
    std::uint32_t Count()
    {
        const std::uint32_t count = U32();
        if (count > kMaxArrayLen)
        {
            ok_ = false;
            return 0;
        }
        return count;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// --- Leaf serializers --------------------------------------------------------
void WriteVec3(ByteWriter& w, const Vec3& v)
{
    w.F32(v.x);
    w.F32(v.y);
    w.F32(v.z);
}
Vec3 ReadVec3(ByteReader& r)
{
    Vec3 v;
    v.x = r.F32();
    v.y = r.F32();
    v.z = r.F32();
    return v;
}

void WriteGridPos(ByteWriter& w, const GridPos& p)
{
    w.I32(p.x);
    w.I32(p.y);
    w.I32(p.z);
}
GridPos ReadGridPos(ByteReader& r)
{
    GridPos p;
    p.x = r.I32();
    p.y = r.I32();
    p.z = r.I32();
    return p;
}

// --- PlayerCommand -----------------------------------------------------------
void WritePlayerCommand(ByteWriter& w, const PlayerCommand& c)
{
    w.U32(c.controlledPlayerId);
    w.U32(c.tick);
    w.F32(c.moveForward);
    w.F32(c.moveStrafe);
    w.F32(c.aimYaw);
    w.F32(c.aimPitch);
    w.Bool(c.jump);
    w.Bool(c.sprint);
    w.Bool(c.sprintTapped);
    w.Bool(c.sneak);
    w.I32(c.selectedSlot);
    w.Bool(c.attackPressed);
    w.Bool(c.attackHeld);
    w.Bool(c.attackReleased);
    w.Bool(c.placePressed);
    w.Bool(c.placeHeld);
    w.Bool(c.scopeHeld);
    w.Bool(c.interact);
    w.Bool(c.useAbility1);
    w.Bool(c.useAbility2);
    w.Bool(c.useUltimate);
    w.Bool(c.useHeal);
    w.Bool(c.useTeleport);
    w.Bool(c.useDash);
    w.Bool(c.useShoot);
    w.Bool(c.useFireball);
    w.Bool(c.useMolotov);
    w.Bool(c.useAlarm);
    w.U32(c.actionSeq);
    w.I32(c.actionType);
    w.I32(c.actionParamA);
    w.I32(c.actionParamB);
    w.U32(c.rewindTick);
}
void ReadPlayerCommand(ByteReader& r, PlayerCommand& c)
{
    c.controlledPlayerId = r.U32();
    c.tick = r.U32();
    c.moveForward = r.F32();
    c.moveStrafe = r.F32();
    c.aimYaw = r.F32();
    c.aimPitch = r.F32();
    c.jump = r.Bool();
    c.sprint = r.Bool();
    c.sprintTapped = r.Bool();
    c.sneak = r.Bool();
    c.selectedSlot = r.I32();
    c.attackPressed = r.Bool();
    c.attackHeld = r.Bool();
    c.attackReleased = r.Bool();
    c.placePressed = r.Bool();
    c.placeHeld = r.Bool();
    c.scopeHeld = r.Bool();
    c.interact = r.Bool();
    c.useAbility1 = r.Bool();
    c.useAbility2 = r.Bool();
    c.useUltimate = r.Bool();
    c.useHeal = r.Bool();
    c.useTeleport = r.Bool();
    c.useDash = r.Bool();
    c.useShoot = r.Bool();
    c.useFireball = r.Bool();
    c.useMolotov = r.Bool();
    c.useAlarm = r.Bool();
    c.actionSeq = r.U32();
    c.actionType = r.I32();
    c.actionParamA = r.I32();
    c.actionParamB = r.I32();
    c.rewindTick = r.U32();
}

// --- Snapshot pieces ---------------------------------------------------------
void WriteInventory(ByteWriter& w, const InventorySnapshot& inv)
{
    w.Bool(inv.present);
    w.I32(inv.resources[0]);
    w.I32(inv.resources[1]);
    w.I32(inv.resources[2]);
    w.U32(static_cast<std::uint32_t>(inv.hotbar.size()));
    for (const ItemStackSnapshot& slot : inv.hotbar)
    {
        w.I32(slot.itemType);
        w.I32(slot.count);
    }
    w.U32(static_cast<std::uint32_t>(inv.main.size()));
    for (const ItemStackSnapshot& slot : inv.main)
    {
        w.I32(slot.itemType);
        w.I32(slot.count);
    }
}
void ReadInventory(ByteReader& r, InventorySnapshot& inv)
{
    inv.present = r.Bool();
    inv.resources[0] = r.I32();
    inv.resources[1] = r.I32();
    inv.resources[2] = r.I32();
    const std::uint32_t count = r.Count();
    inv.hotbar.clear();
    for (std::uint32_t i = 0; i < count && r.Ok(); ++i)
    {
        ItemStackSnapshot slot;
        slot.itemType = r.I32();
        slot.count = r.I32();
        inv.hotbar.push_back(slot);
    }
    const std::uint32_t mainCount = r.Count();
    inv.main.clear();
    for (std::uint32_t i = 0; i < mainCount && r.Ok(); ++i)
    {
        ItemStackSnapshot slot;
        slot.itemType = r.I32();
        slot.count = r.I32();
        inv.main.push_back(slot);
    }
}

void WriteAbilityHud(ByteWriter& w, const HeroAbilityHudSnapshot& hud)
{
    w.Bool(hud.present);
    w.F32(hud.active1Cooldown);
    w.F32(hud.active1ActiveTimer);
    w.F32(hud.active2Cooldown);
    w.F32(hud.active2ActiveTimer);
    w.F32(hud.ultimateCooldown);
    w.F32(hud.ultimateActiveTimer);
    w.F32(hud.ultimateCharge);
    w.Bool(hud.ultimatePrimed);
    w.F32(hud.bowDrawTimer);
    w.I32(hud.blasterState);
    w.F32(hud.blasterLoadTimer);
}
void ReadAbilityHud(ByteReader& r, HeroAbilityHudSnapshot& hud)
{
    hud.present = r.Bool();
    hud.active1Cooldown = r.F32();
    hud.active1ActiveTimer = r.F32();
    hud.active2Cooldown = r.F32();
    hud.active2ActiveTimer = r.F32();
    hud.ultimateCooldown = r.F32();
    hud.ultimateActiveTimer = r.F32();
    hud.ultimateCharge = r.F32();
    hud.ultimatePrimed = r.Bool();
    hud.bowDrawTimer = r.F32();
    hud.blasterState = r.I32();
    hud.blasterLoadTimer = r.F32();
}

void WriteTeamChestSnapshot(ByteWriter& w, const TeamChestSnapshot& chest)
{
    w.I32(chest.teamId);
    w.I32(chest.resources[0]);
    w.I32(chest.resources[1]);
    w.I32(chest.resources[2]);
    w.U32(static_cast<std::uint32_t>(chest.slots.size()));
    for (const ItemStackSnapshot& slot : chest.slots)
    {
        w.I32(slot.itemType);
        w.I32(slot.count);
    }
}

void ReadTeamChestSnapshot(ByteReader& r, TeamChestSnapshot& chest)
{
    chest.teamId = r.I32();
    chest.resources[0] = r.I32();
    chest.resources[1] = r.I32();
    chest.resources[2] = r.I32();
    const std::uint32_t count = r.Count();
    chest.slots.clear();
    for (std::uint32_t i = 0; i < count && r.Ok(); ++i)
    {
        ItemStackSnapshot slot;
        slot.itemType = r.I32();
        slot.count = r.I32();
        chest.slots.push_back(slot);
    }
}

void WritePlayerScoreSnapshot(ByteWriter& w, const PlayerScoreSnapshot& score)
{
    w.I32(score.playerId);
    w.I32(score.kills);
    w.I32(score.deaths);
    w.I32(score.finalDeaths);
    w.I32(score.coreDamage);
    w.I32(score.coresDestroyed);
}

void ReadPlayerScoreSnapshot(ByteReader& r, PlayerScoreSnapshot& score)
{
    score.playerId = r.I32();
    score.kills = r.I32();
    score.deaths = r.I32();
    score.finalDeaths = r.I32();
    score.coreDamage = r.I32();
    score.coresDestroyed = r.I32();
}

void WriteActionResultSnapshot(ByteWriter& w, const ActionResultSnapshot& result)
{
    w.I32(result.playerId);
    w.U32(result.resultSeq);
    w.U32(result.actionSeq);
    w.I32(result.actionType);
    w.I32(result.subjectType);
    w.I32(result.actorPlayerId);
    w.I32(result.targetPlayerId);
    w.I32(result.targetTeamId);
    w.I32(result.amount);
    w.I32(result.flags);
    w.Bool(result.success);
    WriteVec3(w, result.position);
    w.Str(result.message);
    w.I32(result.color[0]);
    w.I32(result.color[1]);
    w.I32(result.color[2]);
    w.I32(result.color[3]);
    w.F32(result.seconds);
    w.F32(result.radius);
}

void ReadActionResultSnapshot(ByteReader& r, ActionResultSnapshot& result)
{
    result.playerId = r.I32();
    result.resultSeq = r.U32();
    result.actionSeq = r.U32();
    result.actionType = r.I32();
    result.subjectType = r.I32();
    result.actorPlayerId = r.I32();
    result.targetPlayerId = r.I32();
    result.targetTeamId = r.I32();
    result.amount = r.I32();
    result.flags = r.I32();
    result.success = r.Bool();
    result.position = ReadVec3(r);
    result.message = r.Str();
    result.color[0] = r.I32();
    result.color[1] = r.I32();
    result.color[2] = r.I32();
    result.color[3] = r.I32();
    result.seconds = r.F32();
    result.radius = r.F32();
}

void WriteWorldEventSnapshot(ByteWriter& w, const WorldEventSnapshot& event)
{
    w.U32(event.eventSeq);
    w.I32(event.kind);
    w.I32(event.actorPlayerId);
    w.I32(event.targetPlayerId);
    w.I32(event.targetTeamId);
    WriteVec3(w, event.position);
    w.I32(event.subjectType);
    w.I32(event.amount);
    w.I32(event.flags);
    w.Str(event.cause);
}

void ReadWorldEventSnapshot(ByteReader& r, WorldEventSnapshot& event)
{
    event.eventSeq = r.U32();
    event.kind = r.I32();
    event.actorPlayerId = r.I32();
    event.targetPlayerId = r.I32();
    event.targetTeamId = r.I32();
    event.position = ReadVec3(r);
    event.subjectType = r.I32();
    event.amount = r.I32();
    event.flags = r.I32();
    event.cause = r.Str();
}

void WritePlayerSnapshot(ByteWriter& w, const PlayerSnapshot& p)
{
    w.I32(p.playerId);
    w.Str(p.playerName);
    w.I32(p.teamId);
    w.I32(p.heroId);
    WriteVec3(w, p.position);
    WriteVec3(w, p.velocity);
    w.F32(p.yaw);
    w.I32(p.health);
    w.I32(p.maxHealth);
    w.Bool(p.alive);
    w.Bool(p.eliminated);
    w.F32(p.respawnTimer);
    w.I32(p.selectedSlot);
    w.I32(p.animationState);
    w.F32(p.animationTimer);
    w.F32(p.animationDuration);
    WriteInventory(w, p.inventory);
    WriteAbilityHud(w, p.abilityHud);
    w.I32(p.disguiseTeamId);
    w.I32(p.disguiseHeroId);
}
void ReadPlayerSnapshot(ByteReader& r, PlayerSnapshot& p)
{
    p.playerId = r.I32();
    p.playerName = r.Str();
    p.teamId = r.I32();
    p.heroId = r.I32();
    p.position = ReadVec3(r);
    p.velocity = ReadVec3(r);
    p.yaw = r.F32();
    p.health = r.I32();
    p.maxHealth = r.I32();
    p.alive = r.Bool();
    p.eliminated = r.Bool();
    p.respawnTimer = r.F32();
    p.selectedSlot = r.I32();
    p.animationState = r.I32();
    p.animationTimer = r.F32();
    p.animationDuration = r.F32();
    ReadInventory(r, p.inventory);
    ReadAbilityHud(r, p.abilityHud);
    p.disguiseTeamId = r.I32();
    p.disguiseHeroId = r.I32();
}

void WriteCoreSnapshot(ByteWriter& w, const CoreSnapshot& core)
{
    w.I32(core.teamId);
    w.I32(core.health);
    w.I32(core.maxHealth);
    w.Bool(core.alive);
}
void ReadCoreSnapshot(ByteReader& r, CoreSnapshot& core)
{
    core.teamId = r.I32();
    core.health = r.I32();
    core.maxHealth = r.I32();
    core.alive = r.Bool();
}

void WriteGeneratorSnapshot(ByteWriter& w, const GeneratorSnapshot& generator)
{
    w.I32(generator.resourceType);
    w.I32(generator.teamId);
    WriteVec3(w, generator.position);
}
void ReadGeneratorSnapshot(ByteReader& r, GeneratorSnapshot& generator)
{
    generator.resourceType = r.I32();
    generator.teamId = r.I32();
    generator.position = ReadVec3(r);
}

void WritePickupSnapshot(ByteWriter& w, const PickupSnapshot& pickup)
{
    w.I32(pickup.resourceType);
    w.I32(pickup.amount);
    WriteVec3(w, pickup.position);
}
void ReadPickupSnapshot(ByteReader& r, PickupSnapshot& pickup)
{
    pickup.resourceType = r.I32();
    pickup.amount = r.I32();
    pickup.position = ReadVec3(r);
}

void WriteDroppedItemSnapshot(ByteWriter& w, const DroppedItemSnapshot& dropped)
{
    w.I32(dropped.id);
    w.I32(dropped.itemType);
    w.I32(dropped.count);
    WriteVec3(w, dropped.position);
    WriteVec3(w, dropped.velocity);
    w.I32(dropped.ownerPlayerId);
    w.F32(dropped.ownerPickupDelay);
    w.F32(dropped.lifetime);
    w.F32(dropped.age);
}
void ReadDroppedItemSnapshot(ByteReader& r, DroppedItemSnapshot& dropped)
{
    dropped.id = r.I32();
    dropped.itemType = r.I32();
    dropped.count = r.I32();
    dropped.position = ReadVec3(r);
    dropped.velocity = ReadVec3(r);
    dropped.ownerPlayerId = r.I32();
    dropped.ownerPickupDelay = r.F32();
    dropped.lifetime = r.F32();
    dropped.age = r.F32();
}

void WriteBlockDelta(ByteWriter& w, const BlockDelta& d)
{
    w.U32(d.tick);
    WriteGridPos(w, d.position);
    w.Enum(static_cast<int>(d.oldType));
    w.Enum(static_cast<int>(d.newType));
    w.I32(d.oldTeamId);
    w.I32(d.newTeamId);
    w.I32(d.oldVariant);
    w.I32(d.newVariant);
    w.I32(d.ownerPlayerId);
    w.Enum(static_cast<int>(d.reason));
}
void ReadBlockDelta(ByteReader& r, BlockDelta& d)
{
    d.tick = r.U32();
    d.position = ReadGridPos(r);
    d.oldType = static_cast<BlockType>(r.Enum());
    d.newType = static_cast<BlockType>(r.Enum());
    d.oldTeamId = r.I32();
    d.newTeamId = r.I32();
    d.oldVariant = r.I32();
    d.newVariant = r.I32();
    d.ownerPlayerId = r.I32();
    d.reason = static_cast<BlockDeltaReason>(r.Enum());
}

void WriteVisibility(ByteWriter& w, SnapshotVisibility visibility)
{
    w.Enum(static_cast<int>(visibility));
}
SnapshotVisibility ReadVisibility(ByteReader& r)
{
    return static_cast<SnapshotVisibility>(r.Enum());
}

void WriteProjectileSnapshot(ByteWriter& w, const ProjectileSnapshot& projectile)
{
    w.I32(projectile.id);
    w.I32(projectile.kind);
    WriteVec3(w, projectile.position);
    WriteVec3(w, projectile.velocity);
    w.I32(projectile.ownerPlayerId);
    w.I32(projectile.ownerTeamId);
    w.F32(projectile.remainingLifetime);
    w.Bool(projectile.fireZone);
    WriteVisibility(w, projectile.visibility);
}
void ReadProjectileSnapshot(ByteReader& r, ProjectileSnapshot& projectile)
{
    projectile.id = r.I32();
    projectile.kind = r.I32();
    projectile.position = ReadVec3(r);
    projectile.velocity = ReadVec3(r);
    projectile.ownerPlayerId = r.I32();
    projectile.ownerTeamId = r.I32();
    projectile.remainingLifetime = r.F32();
    projectile.fireZone = r.Bool();
    projectile.visibility = ReadVisibility(r);
}

void WriteExplosiveSnapshot(ByteWriter& w, const ExplosiveSnapshot& explosive)
{
    w.I32(explosive.id);
    WriteVec3(w, explosive.position);
    w.I32(explosive.ownerPlayerId);
    w.I32(explosive.ownerTeamId);
    w.F32(explosive.remainingTimer);
    w.F32(explosive.radius);
    WriteVisibility(w, explosive.visibility);
    WriteVec3(w, explosive.velocity);
}
void ReadExplosiveSnapshot(ByteReader& r, ExplosiveSnapshot& explosive)
{
    explosive.id = r.I32();
    explosive.position = ReadVec3(r);
    explosive.ownerPlayerId = r.I32();
    explosive.ownerTeamId = r.I32();
    explosive.remainingTimer = r.F32();
    explosive.radius = r.F32();
    explosive.visibility = ReadVisibility(r);
    explosive.velocity = ReadVec3(r);
}

void WriteHazardZoneSnapshot(ByteWriter& w, const HazardZoneSnapshot& hazard)
{
    w.I32(hazard.id);
    WriteVec3(w, hazard.position);
    w.I32(hazard.ownerPlayerId);
    w.I32(hazard.ownerTeamId);
    w.F32(hazard.remainingLifetime);
    w.F32(hazard.radius);
    w.Bool(hazard.blueFire);
    WriteVisibility(w, hazard.visibility);
}
void ReadHazardZoneSnapshot(ByteReader& r, HazardZoneSnapshot& hazard)
{
    hazard.id = r.I32();
    hazard.position = ReadVec3(r);
    hazard.ownerPlayerId = r.I32();
    hazard.ownerTeamId = r.I32();
    hazard.remainingLifetime = r.F32();
    hazard.radius = r.F32();
    hazard.blueFire = r.Bool();
    hazard.visibility = ReadVisibility(r);
}

void WriteHeroDeviceSnapshot(ByteWriter& w, const HeroDeviceSnapshot& device)
{
    w.I32(device.id);
    w.Enum(static_cast<int>(device.type));
    WriteVec3(w, device.position);
    w.I32(device.ownerPlayerId);
    w.I32(device.ownerTeamId);
    w.I32(device.targetPlayerId);
    w.F32(device.remainingLifetime);
    w.I32(device.health);
    WriteVisibility(w, device.visibility);
}
void ReadHeroDeviceSnapshot(ByteReader& r, HeroDeviceSnapshot& device)
{
    device.id = r.I32();
    device.type = static_cast<HeroDeviceType>(r.Enum());
    device.position = ReadVec3(r);
    device.ownerPlayerId = r.I32();
    device.ownerTeamId = r.I32();
    device.targetPlayerId = r.I32();
    device.remainingLifetime = r.F32();
    device.health = r.I32();
    device.visibility = ReadVisibility(r);
}

void WriteStatusEffectSnapshot(ByteWriter& w, const StatusEffectSnapshot& status)
{
    w.I32(status.id);
    w.Enum(static_cast<int>(status.type));
    WriteVec3(w, status.position);
    w.I32(status.targetPlayerId);
    w.I32(status.ownerPlayerId);
    w.I32(status.ownerTeamId);
    w.F32(status.remaining);
    w.I32(status.amount);
    WriteVisibility(w, status.visibility);
}
void ReadStatusEffectSnapshot(ByteReader& r, StatusEffectSnapshot& status)
{
    status.id = r.I32();
    status.type = static_cast<StatusEffectType>(r.Enum());
    status.position = ReadVec3(r);
    status.targetPlayerId = r.I32();
    status.ownerPlayerId = r.I32();
    status.ownerTeamId = r.I32();
    status.remaining = r.F32();
    status.amount = r.I32();
    status.visibility = ReadVisibility(r);
}

void WriteLobbyUpdate(ByteWriter& w, const LobbyUpdate& update)
{
    w.Str(update.playerName);
    w.I32(update.selectedTeam);
    w.I32(update.selectedHero);
    w.Bool(update.ready);
    w.Bool(update.startRequested);
}

void ReadLobbyUpdate(ByteReader& r, LobbyUpdate& update)
{
    update.playerName = r.Str();
    update.selectedTeam = r.I32();
    update.selectedHero = r.I32();
    update.ready = r.Bool();
    update.startRequested = r.Bool();
}

void WriteLobbyPlayer(ByteWriter& w, const LobbyPlayerState& player)
{
    w.I32(player.clientId);
    w.I32(player.assignedPlayerId);
    w.Str(player.playerName);
    w.I32(player.selectedTeam);
    w.I32(player.selectedHero);
    w.Bool(player.ready);
    w.Bool(player.connected);
    w.Bool(player.startRequested);
}

void ReadLobbyPlayer(ByteReader& r, LobbyPlayerState& player)
{
    player.clientId = r.I32();
    player.assignedPlayerId = r.I32();
    player.playerName = r.Str();
    player.selectedTeam = r.I32();
    player.selectedHero = r.I32();
    player.ready = r.Bool();
    player.connected = r.Bool();
    player.startRequested = r.Bool();
}

void WriteLobbySnapshot(ByteWriter& w, const LobbySnapshot& snapshot)
{
    w.U32(snapshot.revision);
    w.I32(snapshot.hostClientId);
    w.Str(snapshot.serverName);
    w.Bool(snapshot.privateServer);
    w.I32(snapshot.maxPlayers);
    w.I32(snapshot.teamCount);
    w.I32(snapshot.maxTeamSize);
    w.I32(snapshot.heroCount);
    w.Bool(snapshot.enforceUniqueHeroesPerTeam);
    w.Bool(snapshot.requireAllReady);
    w.Bool(snapshot.canStart);
    w.Bool(snapshot.matchStarting);
    w.Bool(snapshot.matchStarted);
    w.I32(snapshot.worldBiome);
    w.I32(snapshot.worldLayout);
    w.I32(snapshot.matchMode);
    w.Str(snapshot.statusMessage);
    w.U32(static_cast<std::uint32_t>(snapshot.players.size()));
    for (const LobbyPlayerState& player : snapshot.players)
    {
        WriteLobbyPlayer(w, player);
    }
}

void ReadLobbySnapshot(ByteReader& r, LobbySnapshot& snapshot)
{
    snapshot.revision = r.U32();
    snapshot.hostClientId = r.I32();
    snapshot.serverName = r.Str();
    snapshot.privateServer = r.Bool();
    snapshot.maxPlayers = r.I32();
    snapshot.teamCount = r.I32();
    snapshot.maxTeamSize = r.I32();
    snapshot.heroCount = r.I32();
    snapshot.enforceUniqueHeroesPerTeam = r.Bool();
    snapshot.requireAllReady = r.Bool();
    snapshot.canStart = r.Bool();
    snapshot.matchStarting = r.Bool();
    snapshot.matchStarted = r.Bool();
    snapshot.worldBiome = r.I32();
    snapshot.worldLayout = r.I32();
    snapshot.matchMode = r.I32();
    snapshot.statusMessage = r.Str();
    snapshot.players.clear();
    const std::uint32_t count = r.Count();
    for (std::uint32_t i = 0; i < count && r.Ok(); ++i)
    {
        LobbyPlayerState player;
        ReadLobbyPlayer(r, player);
        snapshot.players.push_back(player);
    }
}

// MatchSnapshot header: the scalar match state, separate from the entity lists.
void WriteSnapshotHeader(ByteWriter& w, const MatchSnapshot& s)
{
    w.U32(s.tick);
    w.U32(s.lastProcessedCommandTick);
    w.F32(s.matchTime);
    w.Enum(static_cast<int>(s.phase));
    w.I32(s.winnerTeamId);
}
void ReadSnapshotHeader(ByteReader& r, MatchSnapshot& s)
{
    s.tick = r.U32();
    s.lastProcessedCommandTick = r.U32();
    s.matchTime = r.F32();
    s.phase = static_cast<MatchPhase>(r.Enum());
    s.winnerTeamId = r.I32();
}

void WriteSnapshotSections(ByteWriter& payload, const MatchSnapshot& snapshot)
{
    payload.U32(static_cast<std::uint32_t>(snapshot.players.size()));
    for (const PlayerSnapshot& player : snapshot.players)
    {
        WritePlayerSnapshot(payload, player);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.matchScores.size()));
    for (const PlayerScoreSnapshot& score : snapshot.matchScores)
    {
        WritePlayerScoreSnapshot(payload, score);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.cores.size()));
    for (const CoreSnapshot& core : snapshot.cores)
    {
        WriteCoreSnapshot(payload, core);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.teamChests.size()));
    for (const TeamChestSnapshot& chest : snapshot.teamChests)
    {
        WriteTeamChestSnapshot(payload, chest);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.generators.size()));
    for (const GeneratorSnapshot& generator : snapshot.generators)
    {
        WriteGeneratorSnapshot(payload, generator);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.pickups.size()));
    for (const PickupSnapshot& pickup : snapshot.pickups)
    {
        WritePickupSnapshot(payload, pickup);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.droppedItems.size()));
    for (const DroppedItemSnapshot& dropped : snapshot.droppedItems)
    {
        WriteDroppedItemSnapshot(payload, dropped);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.blockDeltas.size()));
    for (const BlockDelta& delta : snapshot.blockDeltas)
    {
        WriteBlockDelta(payload, delta);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.projectiles.size()));
    for (const ProjectileSnapshot& projectile : snapshot.projectiles)
    {
        WriteProjectileSnapshot(payload, projectile);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.explosives.size()));
    for (const ExplosiveSnapshot& explosive : snapshot.explosives)
    {
        WriteExplosiveSnapshot(payload, explosive);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.hazardZones.size()));
    for (const HazardZoneSnapshot& hazard : snapshot.hazardZones)
    {
        WriteHazardZoneSnapshot(payload, hazard);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.heroDevices.size()));
    for (const HeroDeviceSnapshot& device : snapshot.heroDevices)
    {
        WriteHeroDeviceSnapshot(payload, device);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.statusEffects.size()));
    for (const StatusEffectSnapshot& status : snapshot.statusEffects)
    {
        WriteStatusEffectSnapshot(payload, status);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.actionResults.size()));
    for (const ActionResultSnapshot& result : snapshot.actionResults)
    {
        WriteActionResultSnapshot(payload, result);
    }
    payload.U32(static_cast<std::uint32_t>(snapshot.worldEvents.size()));
    for (const WorldEventSnapshot& event : snapshot.worldEvents)
    {
        WriteWorldEventSnapshot(payload, event);
    }
}

void ReadSnapshotSections(ByteReader& r, MatchSnapshot& out)
{
    out.players.clear();
    const std::uint32_t playerCount = r.Count();
    for (std::uint32_t i = 0; i < playerCount && r.Ok(); ++i)
    {
        PlayerSnapshot player;
        ReadPlayerSnapshot(r, player);
        out.players.push_back(player);
    }
    out.matchScores.clear();
    const std::uint32_t scoreCount = r.Count();
    for (std::uint32_t i = 0; i < scoreCount && r.Ok(); ++i)
    {
        PlayerScoreSnapshot score;
        ReadPlayerScoreSnapshot(r, score);
        out.matchScores.push_back(score);
    }
    out.cores.clear();
    const std::uint32_t coreCount = r.Count();
    for (std::uint32_t i = 0; i < coreCount && r.Ok(); ++i)
    {
        CoreSnapshot core;
        ReadCoreSnapshot(r, core);
        out.cores.push_back(core);
    }
    out.teamChests.clear();
    const std::uint32_t teamChestCount = r.Count();
    for (std::uint32_t i = 0; i < teamChestCount && r.Ok(); ++i)
    {
        TeamChestSnapshot chest;
        ReadTeamChestSnapshot(r, chest);
        out.teamChests.push_back(chest);
    }
    out.generators.clear();
    const std::uint32_t generatorCount = r.Count();
    for (std::uint32_t i = 0; i < generatorCount && r.Ok(); ++i)
    {
        GeneratorSnapshot generator;
        ReadGeneratorSnapshot(r, generator);
        out.generators.push_back(generator);
    }
    out.pickups.clear();
    const std::uint32_t pickupCount = r.Count();
    for (std::uint32_t i = 0; i < pickupCount && r.Ok(); ++i)
    {
        PickupSnapshot pickup;
        ReadPickupSnapshot(r, pickup);
        out.pickups.push_back(pickup);
    }
    out.droppedItems.clear();
    const std::uint32_t droppedCount = r.Count();
    for (std::uint32_t i = 0; i < droppedCount && r.Ok(); ++i)
    {
        DroppedItemSnapshot dropped;
        ReadDroppedItemSnapshot(r, dropped);
        out.droppedItems.push_back(dropped);
    }
    out.blockDeltas.clear();
    const std::uint32_t deltaCount = r.Count();
    for (std::uint32_t i = 0; i < deltaCount && r.Ok(); ++i)
    {
        BlockDelta delta;
        ReadBlockDelta(r, delta);
        out.blockDeltas.push_back(delta);
    }
    out.projectiles.clear();
    const std::uint32_t projectileCount = r.Count();
    for (std::uint32_t i = 0; i < projectileCount && r.Ok(); ++i)
    {
        ProjectileSnapshot projectile;
        ReadProjectileSnapshot(r, projectile);
        out.projectiles.push_back(projectile);
    }
    out.explosives.clear();
    const std::uint32_t explosiveCount = r.Count();
    for (std::uint32_t i = 0; i < explosiveCount && r.Ok(); ++i)
    {
        ExplosiveSnapshot explosive;
        ReadExplosiveSnapshot(r, explosive);
        out.explosives.push_back(explosive);
    }
    out.hazardZones.clear();
    const std::uint32_t hazardCount = r.Count();
    for (std::uint32_t i = 0; i < hazardCount && r.Ok(); ++i)
    {
        HazardZoneSnapshot hazard;
        ReadHazardZoneSnapshot(r, hazard);
        out.hazardZones.push_back(hazard);
    }
    out.heroDevices.clear();
    const std::uint32_t deviceCount = r.Count();
    for (std::uint32_t i = 0; i < deviceCount && r.Ok(); ++i)
    {
        HeroDeviceSnapshot device;
        ReadHeroDeviceSnapshot(r, device);
        out.heroDevices.push_back(device);
    }
    out.statusEffects.clear();
    const std::uint32_t statusCount = r.Count();
    for (std::uint32_t i = 0; i < statusCount && r.Ok(); ++i)
    {
        StatusEffectSnapshot effect;
        ReadStatusEffectSnapshot(r, effect);
        out.statusEffects.push_back(effect);
    }
    out.actionResults.clear();
    const std::uint32_t actionResultCount = r.Count();
    for (std::uint32_t i = 0; i < actionResultCount && r.Ok(); ++i)
    {
        ActionResultSnapshot result;
        ReadActionResultSnapshot(r, result);
        out.actionResults.push_back(result);
    }
    out.worldEvents.clear();
    const std::uint32_t worldEventCount = r.Count();
    for (std::uint32_t i = 0; i < worldEventCount && r.Ok(); ++i)
    {
        WorldEventSnapshot event;
        ReadWorldEventSnapshot(r, event);
        out.worldEvents.push_back(event);
    }
}

void WriteRemovedU32(ByteWriter& w, const std::vector<std::uint32_t>& values)
{
    w.U32(static_cast<std::uint32_t>(values.size()));
    for (std::uint32_t value : values)
    {
        w.U32(value);
    }
}

void ReadRemovedU32(ByteReader& r, std::vector<std::uint32_t>& values)
{
    values.clear();
    const std::uint32_t count = r.Count();
    for (std::uint32_t i = 0; i < count && r.Ok(); ++i)
    {
        values.push_back(r.U32());
    }
}

void WriteRemovedI32(ByteWriter& w, const std::vector<int>& values)
{
    w.U32(static_cast<std::uint32_t>(values.size()));
    for (int value : values)
    {
        w.I32(value);
    }
}

void ReadRemovedI32(ByteReader& r, std::vector<int>& values)
{
    values.clear();
    const std::uint32_t count = r.Count();
    for (std::uint32_t i = 0; i < count && r.Ok(); ++i)
    {
        values.push_back(r.I32());
    }
}

// --- Packet framing ----------------------------------------------------------
void WritePacketHeader(ByteWriter& w, MessageType type, std::uint32_t sequence,
                       std::uint32_t tick, std::uint32_t payloadSize)
{
    w.U32(kProtocolMagic);
    w.U16(kProtocolVersion);
    w.U8(static_cast<std::uint8_t>(type));
    w.U32(sequence);
    w.U32(tick);
    w.U32(payloadSize);
}

std::vector<std::uint8_t> FramePacket(MessageType type, std::uint32_t sequence,
                                      std::uint32_t tick, const std::vector<std::uint8_t>& payload)
{
    ByteWriter w;
    WritePacketHeader(w, type, sequence, tick, static_cast<std::uint32_t>(payload.size()));
    for (std::uint8_t b : payload)
    {
        w.U8(b);
    }
    return w.Take();
}

// Read + validate the header, leaving the reader positioned at the payload.
DecodeStatus ReadAndValidateHeader(ByteReader& r, PacketHeader& header)
{
    const std::uint32_t magic = r.U32();
    if (!r.Ok())
    {
        return DecodeStatus::TooShort;
    }
    if (magic != kProtocolMagic)
    {
        return DecodeStatus::BadMagic;
    }
    const std::uint16_t version = r.U16();
    const std::uint8_t type = r.U8();
    const std::uint32_t sequence = r.U32();
    const std::uint32_t tick = r.U32();
    const std::uint32_t payloadSize = r.U32();
    if (!r.Ok())
    {
        return DecodeStatus::TooShort;
    }
    header.magic = magic;
    header.protocolVersion = version;
    header.type = static_cast<MessageType>(type);
    header.sequence = sequence;
    header.tick = tick;
    header.payloadSize = payloadSize;
    if (version != kProtocolVersion)
    {
        return DecodeStatus::VersionMismatch;
    }
    // The declared payload must actually be present (catches truncation).
    if (payloadSize > r.Remaining())
    {
        return DecodeStatus::TooShort;
    }
    return DecodeStatus::Ok;
}
} // namespace

// --- Public encoders ---------------------------------------------------------
std::vector<std::uint8_t> EncodePlayerCommand(std::uint32_t sequence, const PlayerCommand& command)
{
    ByteWriter payload;
    WritePlayerCommand(payload, command);
    return FramePacket(MessageType::PlayerCommand, sequence, command.tick, payload.Bytes());
}

std::vector<std::uint8_t> EncodePlayerCommandBatch(
    std::uint32_t sequence, const std::vector<PlayerCommand>& commands)
{
    ByteWriter payload;
    payload.U32(static_cast<std::uint32_t>(commands.size()));
    for (const PlayerCommand& command : commands)
    {
        WritePlayerCommand(payload, command);
    }
    const std::uint32_t tick = commands.empty() ? 0u : commands.back().tick;
    return FramePacket(MessageType::PlayerCommandBatch, sequence, tick, payload.Bytes());
}

std::vector<std::uint8_t> EncodeMatchSnapshot(std::uint32_t sequence, const MatchSnapshot& snapshot)
{
    ByteWriter payload;
    WriteSnapshotHeader(payload, snapshot);
    WriteSnapshotSections(payload, snapshot);
    return FramePacket(MessageType::MatchSnapshot, sequence, snapshot.tick, payload.Bytes());
}

std::vector<std::uint8_t> EncodeSnapshotDelta(std::uint32_t sequence, const MatchSnapshotDelta& delta)
{
    ByteWriter payload;
    payload.U32(delta.tick);
    payload.U32(delta.baselineTick);
    payload.U32(delta.baselineSequence);
    payload.U32(delta.lastProcessedCommandTick);
    payload.F32(delta.matchTime);
    payload.Enum(static_cast<int>(delta.phase));
    payload.I32(delta.winnerTeamId);

    payload.U32(static_cast<std::uint32_t>(delta.players.size()));
    for (const PlayerSnapshot& player : delta.players)
    {
        WritePlayerSnapshot(payload, player);
    }
    WriteRemovedI32(payload, delta.removedPlayerIds);

    payload.U32(static_cast<std::uint32_t>(delta.matchScores.size()));
    for (const PlayerScoreSnapshot& score : delta.matchScores)
    {
        WritePlayerScoreSnapshot(payload, score);
    }
    WriteRemovedI32(payload, delta.removedScorePlayerIds);

    payload.U32(static_cast<std::uint32_t>(delta.cores.size()));
    for (const CoreSnapshot& core : delta.cores)
    {
        WriteCoreSnapshot(payload, core);
    }
    WriteRemovedI32(payload, delta.removedCoreTeamIds);

    payload.U32(static_cast<std::uint32_t>(delta.teamChests.size()));
    for (const TeamChestSnapshot& chest : delta.teamChests)
    {
        WriteTeamChestSnapshot(payload, chest);
    }
    WriteRemovedI32(payload, delta.removedTeamChestTeamIds);

    payload.U32(static_cast<std::uint32_t>(delta.generators.size()));
    for (const IndexedGeneratorSnapshot& entry : delta.generators)
    {
        payload.U32(entry.index);
        WriteGeneratorSnapshot(payload, entry.value);
    }
    WriteRemovedU32(payload, delta.removedGeneratorIndices);

    payload.U32(static_cast<std::uint32_t>(delta.pickups.size()));
    for (const IndexedPickupSnapshot& entry : delta.pickups)
    {
        payload.U32(entry.index);
        WritePickupSnapshot(payload, entry.value);
    }
    WriteRemovedU32(payload, delta.removedPickupIndices);

    payload.U32(static_cast<std::uint32_t>(delta.droppedItems.size()));
    for (const IndexedDroppedItemSnapshot& entry : delta.droppedItems)
    {
        payload.U32(entry.index);
        WriteDroppedItemSnapshot(payload, entry.value);
    }
    WriteRemovedU32(payload, delta.removedDroppedItemIndices);

    payload.U32(static_cast<std::uint32_t>(delta.blockDeltas.size()));
    for (const BlockDelta& block : delta.blockDeltas)
    {
        WriteBlockDelta(payload, block);
    }

    payload.U32(static_cast<std::uint32_t>(delta.projectiles.size()));
    for (const ProjectileSnapshot& projectile : delta.projectiles)
    {
        WriteProjectileSnapshot(payload, projectile);
    }
    WriteRemovedI32(payload, delta.removedProjectileIds);

    payload.U32(static_cast<std::uint32_t>(delta.explosives.size()));
    for (const ExplosiveSnapshot& explosive : delta.explosives)
    {
        WriteExplosiveSnapshot(payload, explosive);
    }
    WriteRemovedI32(payload, delta.removedExplosiveIds);

    payload.U32(static_cast<std::uint32_t>(delta.hazardZones.size()));
    for (const HazardZoneSnapshot& hazard : delta.hazardZones)
    {
        WriteHazardZoneSnapshot(payload, hazard);
    }
    WriteRemovedI32(payload, delta.removedHazardZoneIds);

    payload.U32(static_cast<std::uint32_t>(delta.heroDevices.size()));
    for (const HeroDeviceSnapshot& device : delta.heroDevices)
    {
        WriteHeroDeviceSnapshot(payload, device);
    }
    WriteRemovedI32(payload, delta.removedHeroDeviceIds);

    payload.U32(static_cast<std::uint32_t>(delta.statusEffects.size()));
    for (const StatusEffectSnapshot& status : delta.statusEffects)
    {
        WriteStatusEffectSnapshot(payload, status);
    }
    WriteRemovedI32(payload, delta.removedStatusEffectIds);

    payload.U32(static_cast<std::uint32_t>(delta.actionResults.size()));
    for (const ActionResultSnapshot& result : delta.actionResults)
    {
        WriteActionResultSnapshot(payload, result);
    }
    payload.U32(static_cast<std::uint32_t>(delta.worldEvents.size()));
    for (const WorldEventSnapshot& event : delta.worldEvents)
    {
        WriteWorldEventSnapshot(payload, event);
    }

    return FramePacket(MessageType::SnapshotDelta, sequence, delta.tick, payload.Bytes());
}

std::vector<std::uint8_t> EncodeControl(MessageType type, std::uint32_t sequence, std::uint32_t tick)
{
    // Header-only control packet (empty payload).
    return FramePacket(type, sequence, tick, {});
}

std::vector<std::uint8_t> EncodeConnect(std::uint32_t sequence, const std::string& token)
{
    ByteWriter payload;
    payload.Str(token);
    return FramePacket(MessageType::Connect, sequence, 0, payload.Bytes());
}

DecodeStatus DecodeConnect(const std::uint8_t* data, std::size_t size,
                           PacketHeader& header, std::string& token)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::Connect)
    {
        return DecodeStatus::WrongType;
    }
    token = r.Str();
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

std::vector<std::uint8_t> EncodeConnectAck(std::uint32_t sequence, int assignedPlayerId)
{
    ByteWriter payload;
    payload.I32(assignedPlayerId);
    return FramePacket(MessageType::ConnectAck, sequence, 0, payload.Bytes());
}

std::vector<std::uint8_t> EncodeConnectDenied(std::uint32_t sequence, const std::string& reason)
{
    ByteWriter payload;
    payload.Str(reason);
    return FramePacket(MessageType::ConnectDenied, sequence, 0, payload.Bytes());
}

DecodeStatus DecodeConnectDenied(const std::uint8_t* data, std::size_t size,
                                 PacketHeader& header, std::string& reason)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::ConnectDenied)
    {
        return DecodeStatus::WrongType;
    }
    reason = r.Str();
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

std::vector<std::uint8_t> EncodeLobbyUpdate(std::uint32_t sequence, const LobbyUpdate& update)
{
    ByteWriter payload;
    WriteLobbyUpdate(payload, update);
    return FramePacket(MessageType::LobbyUpdate, sequence, 0, payload.Bytes());
}

DecodeStatus DecodeLobbyUpdate(const std::uint8_t* data, std::size_t size,
                               PacketHeader& header, LobbyUpdate& update)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::LobbyUpdate)
    {
        return DecodeStatus::WrongType;
    }
    ReadLobbyUpdate(r, update);
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

std::vector<std::uint8_t> EncodeLobbySnapshot(std::uint32_t sequence, const LobbySnapshot& snapshot)
{
    ByteWriter payload;
    WriteLobbySnapshot(payload, snapshot);
    return FramePacket(MessageType::LobbySnapshot, sequence, snapshot.revision, payload.Bytes());
}

DecodeStatus DecodeLobbySnapshot(const std::uint8_t* data, std::size_t size,
                                 PacketHeader& header, LobbySnapshot& snapshot)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::LobbySnapshot)
    {
        return DecodeStatus::WrongType;
    }
    ReadLobbySnapshot(r, snapshot);
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

std::vector<std::uint8_t> EncodeFullResyncRequest(
    std::uint32_t sequence, std::uint32_t lastKnownSequence, std::uint32_t lastKnownTick)
{
    ByteWriter payload;
    payload.U32(lastKnownSequence);
    payload.U32(lastKnownTick);
    return FramePacket(MessageType::FullResyncRequest, sequence, lastKnownTick, payload.Bytes());
}

DecodeStatus DecodeFullResyncRequest(
    const std::uint8_t* data, std::size_t size, PacketHeader& header,
    std::uint32_t& lastKnownSequence, std::uint32_t& lastKnownTick)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::FullResyncRequest)
    {
        return DecodeStatus::WrongType;
    }
    lastKnownSequence = r.U32();
    lastKnownTick = r.U32();
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

std::vector<std::uint8_t> EncodeReliableAck(
    std::uint32_t sequence, std::uint32_t ackedSequence, MessageType ackedType)
{
    ByteWriter payload;
    payload.U32(ackedSequence);
    payload.U8(static_cast<std::uint8_t>(ackedType));
    return FramePacket(MessageType::ReliableAck, sequence, ackedSequence, payload.Bytes());
}

DecodeStatus DecodeReliableAck(
    const std::uint8_t* data, std::size_t size, PacketHeader& header,
    std::uint32_t& ackedSequence, MessageType& ackedType)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::ReliableAck)
    {
        return DecodeStatus::WrongType;
    }
    ackedSequence = r.U32();
    ackedType = static_cast<MessageType>(r.U8());
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

std::vector<std::uint8_t> EncodePacketFragment(
    std::uint32_t sequence, std::uint32_t fragmentId, std::uint16_t fragmentIndex,
    std::uint16_t fragmentCount, std::uint32_t totalSize,
    const std::uint8_t* chunk, std::size_t chunkSize)
{
    ByteWriter payload;
    payload.U32(fragmentId);
    payload.U16(fragmentIndex);
    payload.U16(fragmentCount);
    payload.U32(totalSize);
    payload.U32(static_cast<std::uint32_t>(chunkSize));
    for (std::size_t i = 0; i < chunkSize; ++i)
    {
        payload.U8(chunk[i]);
    }
    return FramePacket(MessageType::PacketFragment, sequence, fragmentId, payload.Bytes());
}

DecodeStatus DecodePacketFragment(
    const std::uint8_t* data, std::size_t size, PacketHeader& header,
    std::uint32_t& fragmentId, std::uint16_t& fragmentIndex, std::uint16_t& fragmentCount,
    std::uint32_t& totalSize, std::vector<std::uint8_t>& chunk)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::PacketFragment)
    {
        return DecodeStatus::WrongType;
    }
    fragmentId = r.U32();
    fragmentIndex = r.U16();
    fragmentCount = r.U16();
    totalSize = r.U32();
    const std::uint32_t chunkSize = r.U32();
    if (!r.Ok() || chunkSize > kMaxArrayLen || chunkSize > r.Remaining())
    {
        return DecodeStatus::BadPayload;
    }
    chunk.resize(chunkSize);
    for (std::uint32_t i = 0; i < chunkSize; ++i)
    {
        chunk[i] = r.U8();
    }
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

DecodeStatus DecodeConnectAck(const std::uint8_t* data, std::size_t size,
                              PacketHeader& header, int& assignedPlayerId)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::ConnectAck)
    {
        return DecodeStatus::WrongType;
    }
    assignedPlayerId = r.I32();
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

// --- Public decoders ---------------------------------------------------------
DecodeStatus DecodeHeader(const std::uint8_t* data, std::size_t size, PacketHeader& header)
{
    ByteReader r(data, size);
    return ReadAndValidateHeader(r, header);
}

DecodeStatus DecodePlayerCommand(const std::uint8_t* data, std::size_t size,
                                 PacketHeader& header, PlayerCommand& out)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::PlayerCommand)
    {
        return DecodeStatus::WrongType;
    }
    ReadPlayerCommand(r, out);
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

DecodeStatus DecodePlayerCommandBatch(
    const std::uint8_t* data, std::size_t size, PacketHeader& header,
    std::vector<PlayerCommand>& out)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::PlayerCommandBatch)
    {
        return DecodeStatus::WrongType;
    }
    const std::uint32_t count = r.Count();
    if (count > kMaxCommandBatchLen)
    {
        return DecodeStatus::BadPayload;
    }
    std::vector<PlayerCommand> commands;
    commands.reserve(count);
    for (std::uint32_t i = 0; i < count && r.Ok(); ++i)
    {
        PlayerCommand command;
        ReadPlayerCommand(r, command);
        commands.push_back(command);
    }
    if (!r.Ok())
    {
        return DecodeStatus::BadPayload;
    }
    out = std::move(commands);
    return DecodeStatus::Ok;
}

DecodeStatus DecodeMatchSnapshot(const std::uint8_t* data, std::size_t size,
                                 PacketHeader& header, MatchSnapshot& out)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::MatchSnapshot)
    {
        return DecodeStatus::WrongType;
    }
    ReadSnapshotHeader(r, out);
    ReadSnapshotSections(r, out);
    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}

DecodeStatus DecodeSnapshotDelta(const std::uint8_t* data, std::size_t size,
                                 PacketHeader& header, MatchSnapshotDelta& out)
{
    ByteReader r(data, size);
    const DecodeStatus status = ReadAndValidateHeader(r, header);
    if (status != DecodeStatus::Ok)
    {
        return status;
    }
    if (header.type != MessageType::SnapshotDelta)
    {
        return DecodeStatus::WrongType;
    }

    out.tick = r.U32();
    out.baselineTick = r.U32();
    out.baselineSequence = r.U32();
    out.lastProcessedCommandTick = r.U32();
    out.matchTime = r.F32();
    out.phase = static_cast<MatchPhase>(r.Enum());
    out.winnerTeamId = r.I32();

    out.players.clear();
    const std::uint32_t playerCount = r.Count();
    for (std::uint32_t i = 0; i < playerCount && r.Ok(); ++i)
    {
        PlayerSnapshot player;
        ReadPlayerSnapshot(r, player);
        out.players.push_back(player);
    }
    ReadRemovedI32(r, out.removedPlayerIds);

    out.matchScores.clear();
    const std::uint32_t scoreCount = r.Count();
    for (std::uint32_t i = 0; i < scoreCount && r.Ok(); ++i)
    {
        PlayerScoreSnapshot score;
        ReadPlayerScoreSnapshot(r, score);
        out.matchScores.push_back(score);
    }
    ReadRemovedI32(r, out.removedScorePlayerIds);

    out.cores.clear();
    const std::uint32_t coreCount = r.Count();
    for (std::uint32_t i = 0; i < coreCount && r.Ok(); ++i)
    {
        CoreSnapshot core;
        ReadCoreSnapshot(r, core);
        out.cores.push_back(core);
    }
    ReadRemovedI32(r, out.removedCoreTeamIds);

    out.teamChests.clear();
    const std::uint32_t teamChestCount = r.Count();
    for (std::uint32_t i = 0; i < teamChestCount && r.Ok(); ++i)
    {
        TeamChestSnapshot chest;
        ReadTeamChestSnapshot(r, chest);
        out.teamChests.push_back(chest);
    }
    ReadRemovedI32(r, out.removedTeamChestTeamIds);

    out.generators.clear();
    const std::uint32_t generatorCount = r.Count();
    for (std::uint32_t i = 0; i < generatorCount && r.Ok(); ++i)
    {
        IndexedGeneratorSnapshot entry;
        entry.index = r.U32();
        ReadGeneratorSnapshot(r, entry.value);
        out.generators.push_back(entry);
    }
    ReadRemovedU32(r, out.removedGeneratorIndices);

    out.pickups.clear();
    const std::uint32_t pickupCount = r.Count();
    for (std::uint32_t i = 0; i < pickupCount && r.Ok(); ++i)
    {
        IndexedPickupSnapshot entry;
        entry.index = r.U32();
        ReadPickupSnapshot(r, entry.value);
        out.pickups.push_back(entry);
    }
    ReadRemovedU32(r, out.removedPickupIndices);

    out.droppedItems.clear();
    const std::uint32_t droppedCount = r.Count();
    for (std::uint32_t i = 0; i < droppedCount && r.Ok(); ++i)
    {
        IndexedDroppedItemSnapshot entry;
        entry.index = r.U32();
        ReadDroppedItemSnapshot(r, entry.value);
        out.droppedItems.push_back(entry);
    }
    ReadRemovedU32(r, out.removedDroppedItemIndices);

    out.blockDeltas.clear();
    const std::uint32_t blockCount = r.Count();
    for (std::uint32_t i = 0; i < blockCount && r.Ok(); ++i)
    {
        BlockDelta block;
        ReadBlockDelta(r, block);
        out.blockDeltas.push_back(block);
    }

    out.projectiles.clear();
    const std::uint32_t projectileCount = r.Count();
    for (std::uint32_t i = 0; i < projectileCount && r.Ok(); ++i)
    {
        ProjectileSnapshot projectile;
        ReadProjectileSnapshot(r, projectile);
        out.projectiles.push_back(projectile);
    }
    ReadRemovedI32(r, out.removedProjectileIds);

    out.explosives.clear();
    const std::uint32_t explosiveCount = r.Count();
    for (std::uint32_t i = 0; i < explosiveCount && r.Ok(); ++i)
    {
        ExplosiveSnapshot explosive;
        ReadExplosiveSnapshot(r, explosive);
        out.explosives.push_back(explosive);
    }
    ReadRemovedI32(r, out.removedExplosiveIds);

    out.hazardZones.clear();
    const std::uint32_t hazardCount = r.Count();
    for (std::uint32_t i = 0; i < hazardCount && r.Ok(); ++i)
    {
        HazardZoneSnapshot hazard;
        ReadHazardZoneSnapshot(r, hazard);
        out.hazardZones.push_back(hazard);
    }
    ReadRemovedI32(r, out.removedHazardZoneIds);

    out.heroDevices.clear();
    const std::uint32_t deviceCount = r.Count();
    for (std::uint32_t i = 0; i < deviceCount && r.Ok(); ++i)
    {
        HeroDeviceSnapshot device;
        ReadHeroDeviceSnapshot(r, device);
        out.heroDevices.push_back(device);
    }
    ReadRemovedI32(r, out.removedHeroDeviceIds);

    out.statusEffects.clear();
    const std::uint32_t statusCount = r.Count();
    for (std::uint32_t i = 0; i < statusCount && r.Ok(); ++i)
    {
        StatusEffectSnapshot effect;
        ReadStatusEffectSnapshot(r, effect);
        out.statusEffects.push_back(effect);
    }
    ReadRemovedI32(r, out.removedStatusEffectIds);

    out.actionResults.clear();
    const std::uint32_t actionResultCount = r.Count();
    for (std::uint32_t i = 0; i < actionResultCount && r.Ok(); ++i)
    {
        ActionResultSnapshot result;
        ReadActionResultSnapshot(r, result);
        out.actionResults.push_back(result);
    }
    out.worldEvents.clear();
    const std::uint32_t worldEventCount = r.Count();
    for (std::uint32_t i = 0; i < worldEventCount && r.Ok(); ++i)
    {
        WorldEventSnapshot event;
        ReadWorldEventSnapshot(r, event);
        out.worldEvents.push_back(event);
    }

    return r.Ok() ? DecodeStatus::Ok : DecodeStatus::BadPayload;
}
