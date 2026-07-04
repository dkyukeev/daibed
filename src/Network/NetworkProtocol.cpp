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
    w.Bool(c.bridgeMode);
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
    c.bridgeMode = r.Bool();
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

// --- Roundtrip smoke (CLI: --protocol-smoke) ---------------------------------
namespace
{
bool VecEqual(const Vec3& a, const Vec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool FloatEqual(float a, float b)
{
    return a == b;
}

bool CommandEqual(const PlayerCommand& a, const PlayerCommand& b)
{
    return a.controlledPlayerId == b.controlledPlayerId && a.tick == b.tick
        && a.moveForward == b.moveForward && a.moveStrafe == b.moveStrafe
        && a.aimYaw == b.aimYaw && a.aimPitch == b.aimPitch
        && a.jump == b.jump && a.sprint == b.sprint && a.sprintTapped == b.sprintTapped
        && a.sneak == b.sneak && a.bridgeMode == b.bridgeMode && a.selectedSlot == b.selectedSlot
        && a.attackPressed == b.attackPressed && a.attackHeld == b.attackHeld
        && a.attackReleased == b.attackReleased && a.placePressed == b.placePressed
        && a.placeHeld == b.placeHeld && a.scopeHeld == b.scopeHeld && a.interact == b.interact
        && a.useAbility1 == b.useAbility1 && a.useAbility2 == b.useAbility2
        && a.useUltimate == b.useUltimate && a.useHeal == b.useHeal && a.useTeleport == b.useTeleport
        && a.useDash == b.useDash && a.useShoot == b.useShoot && a.useFireball == b.useFireball
        && a.useMolotov == b.useMolotov && a.useAlarm == b.useAlarm
        && a.actionSeq == b.actionSeq && a.actionType == b.actionType
        && a.actionParamA == b.actionParamA && a.actionParamB == b.actionParamB
        && a.rewindTick == b.rewindTick;
}

bool InventoryEqual(const InventorySnapshot& a, const InventorySnapshot& b)
{
    if (a.present != b.present
        || a.resources != b.resources
        || a.hotbar.size() != b.hotbar.size()
        || a.main.size() != b.main.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.hotbar.size(); ++i)
    {
        if (a.hotbar[i].itemType != b.hotbar[i].itemType || a.hotbar[i].count != b.hotbar[i].count)
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.main.size(); ++i)
    {
        if (a.main[i].itemType != b.main[i].itemType || a.main[i].count != b.main[i].count)
        {
            return false;
        }
    }
    return true;
}

bool AbilityHudEqual(const HeroAbilityHudSnapshot& a, const HeroAbilityHudSnapshot& b)
{
    return a.present == b.present
        && FloatEqual(a.active1Cooldown, b.active1Cooldown)
        && FloatEqual(a.active1ActiveTimer, b.active1ActiveTimer)
        && FloatEqual(a.active2Cooldown, b.active2Cooldown)
        && FloatEqual(a.active2ActiveTimer, b.active2ActiveTimer)
        && FloatEqual(a.ultimateCooldown, b.ultimateCooldown)
        && FloatEqual(a.ultimateActiveTimer, b.ultimateActiveTimer)
        && FloatEqual(a.ultimateCharge, b.ultimateCharge)
        && a.ultimatePrimed == b.ultimatePrimed
        && FloatEqual(a.bowDrawTimer, b.bowDrawTimer)
        && a.blasterState == b.blasterState
        && FloatEqual(a.blasterLoadTimer, b.blasterLoadTimer);
}

bool ItemSlotsEqual(const std::vector<ItemStackSnapshot>& a, const std::vector<ItemStackSnapshot>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].itemType != b[i].itemType || a[i].count != b[i].count)
        {
            return false;
        }
    }
    return true;
}

bool TeamChestEqual(const TeamChestSnapshot& a, const TeamChestSnapshot& b)
{
    return a.teamId == b.teamId
        && a.resources == b.resources
        && ItemSlotsEqual(a.slots, b.slots);
}

bool PlayerScoreEqual(const PlayerScoreSnapshot& a, const PlayerScoreSnapshot& b)
{
    return a.playerId == b.playerId
        && a.kills == b.kills
        && a.deaths == b.deaths
        && a.finalDeaths == b.finalDeaths
        && a.coreDamage == b.coreDamage
        && a.coresDestroyed == b.coresDestroyed;
}

bool PlayerEqual(const PlayerSnapshot& a, const PlayerSnapshot& b)
{
    return a.playerId == b.playerId && a.playerName == b.playerName
        && a.teamId == b.teamId && a.heroId == b.heroId
        && VecEqual(a.position, b.position) && VecEqual(a.velocity, b.velocity)
        && FloatEqual(a.yaw, b.yaw) && a.health == b.health && a.maxHealth == b.maxHealth && a.alive == b.alive
        && a.eliminated == b.eliminated && FloatEqual(a.respawnTimer, b.respawnTimer)
        && a.selectedSlot == b.selectedSlot
        && a.animationState == b.animationState
        && FloatEqual(a.animationTimer, b.animationTimer)
        && FloatEqual(a.animationDuration, b.animationDuration)
        && InventoryEqual(a.inventory, b.inventory)
        && AbilityHudEqual(a.abilityHud, b.abilityHud)
        && a.disguiseTeamId == b.disguiseTeamId && a.disguiseHeroId == b.disguiseHeroId;
}

bool CoreEqual(const CoreSnapshot& a, const CoreSnapshot& b)
{
    return a.teamId == b.teamId && a.health == b.health && a.maxHealth == b.maxHealth
        && a.alive == b.alive;
}

bool GeneratorEqual(const GeneratorSnapshot& a, const GeneratorSnapshot& b)
{
    return a.resourceType == b.resourceType && a.teamId == b.teamId
        && VecEqual(a.position, b.position);
}

bool PickupEqual(const PickupSnapshot& a, const PickupSnapshot& b)
{
    return a.resourceType == b.resourceType && a.amount == b.amount
        && VecEqual(a.position, b.position);
}

bool DroppedItemEqual(const DroppedItemSnapshot& a, const DroppedItemSnapshot& b)
{
    return a.id == b.id && a.itemType == b.itemType && a.count == b.count
        && VecEqual(a.position, b.position) && VecEqual(a.velocity, b.velocity)
        && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerPickupDelay == b.ownerPickupDelay
        && a.lifetime == b.lifetime
        && a.age == b.age;
}

bool DeltaEqual(const BlockDelta& a, const BlockDelta& b)
{
    return a.tick == b.tick && a.position == b.position && a.oldType == b.oldType
        && a.newType == b.newType && a.oldTeamId == b.oldTeamId && a.newTeamId == b.newTeamId
        && a.ownerPlayerId == b.ownerPlayerId && a.reason == b.reason;
}

bool ProjectileEqual(const ProjectileSnapshot& a, const ProjectileSnapshot& b)
{
    return a.id == b.id && a.kind == b.kind && VecEqual(a.position, b.position)
        && VecEqual(a.velocity, b.velocity) && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerTeamId == b.ownerTeamId && a.remainingLifetime == b.remainingLifetime
        && a.fireZone == b.fireZone && a.visibility == b.visibility;
}

bool ExplosiveEqual(const ExplosiveSnapshot& a, const ExplosiveSnapshot& b)
{
    return a.id == b.id && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.remainingTimer == b.remainingTimer && a.radius == b.radius
        && a.visibility == b.visibility;
}

bool HazardZoneEqual(const HazardZoneSnapshot& a, const HazardZoneSnapshot& b)
{
    return a.id == b.id && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.remainingLifetime == b.remainingLifetime && a.radius == b.radius
        && a.blueFire == b.blueFire && a.visibility == b.visibility;
}

bool HeroDeviceEqual(const HeroDeviceSnapshot& a, const HeroDeviceSnapshot& b)
{
    return a.id == b.id && a.type == b.type && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.targetPlayerId == b.targetPlayerId && a.remainingLifetime == b.remainingLifetime
        && a.health == b.health && a.visibility == b.visibility;
}

bool StatusEffectEqual(const StatusEffectSnapshot& a, const StatusEffectSnapshot& b)
{
    return a.id == b.id && a.type == b.type && VecEqual(a.position, b.position)
        && a.targetPlayerId == b.targetPlayerId && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerTeamId == b.ownerTeamId && a.remaining == b.remaining
        && a.amount == b.amount && a.visibility == b.visibility;
}

bool ActionResultEqual(const ActionResultSnapshot& a, const ActionResultSnapshot& b)
{
    return a.playerId == b.playerId
        && a.resultSeq == b.resultSeq
        && a.actionSeq == b.actionSeq
        && a.actionType == b.actionType
        && a.subjectType == b.subjectType
        && a.actorPlayerId == b.actorPlayerId
        && a.targetPlayerId == b.targetPlayerId
        && a.targetTeamId == b.targetTeamId
        && a.amount == b.amount
        && a.flags == b.flags
        && a.success == b.success
        && VecEqual(a.position, b.position)
        && a.message == b.message
        && a.color == b.color
        && FloatEqual(a.seconds, b.seconds)
        && FloatEqual(a.radius, b.radius);
}

bool WorldEventEqual(const WorldEventSnapshot& a, const WorldEventSnapshot& b)
{
    return a.eventSeq == b.eventSeq
        && a.kind == b.kind
        && a.actorPlayerId == b.actorPlayerId
        && a.targetPlayerId == b.targetPlayerId
        && a.targetTeamId == b.targetTeamId
        && VecEqual(a.position, b.position)
        && a.subjectType == b.subjectType
        && a.amount == b.amount
        && a.flags == b.flags
        && a.cause == b.cause;
}

bool SnapshotEqual(const MatchSnapshot& a, const MatchSnapshot& b)
{
    if (a.tick != b.tick || a.lastProcessedCommandTick != b.lastProcessedCommandTick
        || a.matchTime != b.matchTime || a.phase != b.phase
        || a.winnerTeamId != b.winnerTeamId
        || a.players.size() != b.players.size()
        || a.matchScores.size() != b.matchScores.size()
        || a.cores.size() != b.cores.size()
        || a.teamChests.size() != b.teamChests.size()
        || a.generators.size() != b.generators.size()
        || a.pickups.size() != b.pickups.size()
        || a.droppedItems.size() != b.droppedItems.size()
        || a.blockDeltas.size() != b.blockDeltas.size()
        || a.projectiles.size() != b.projectiles.size()
        || a.explosives.size() != b.explosives.size()
        || a.hazardZones.size() != b.hazardZones.size()
        || a.heroDevices.size() != b.heroDevices.size()
        || a.statusEffects.size() != b.statusEffects.size()
        || a.actionResults.size() != b.actionResults.size()
        || a.worldEvents.size() != b.worldEvents.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.players.size(); ++i)
    {
        if (!PlayerEqual(a.players[i], b.players[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.matchScores.size(); ++i)
    {
        if (!PlayerScoreEqual(a.matchScores[i], b.matchScores[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.cores.size(); ++i)
    {
        if (!CoreEqual(a.cores[i], b.cores[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.teamChests.size(); ++i)
    {
        if (!TeamChestEqual(a.teamChests[i], b.teamChests[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.generators.size(); ++i)
    {
        if (!GeneratorEqual(a.generators[i], b.generators[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.pickups.size(); ++i)
    {
        if (!PickupEqual(a.pickups[i], b.pickups[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.droppedItems.size(); ++i)
    {
        if (!DroppedItemEqual(a.droppedItems[i], b.droppedItems[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.blockDeltas.size(); ++i)
    {
        if (!DeltaEqual(a.blockDeltas[i], b.blockDeltas[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.projectiles.size(); ++i)
    {
        if (!ProjectileEqual(a.projectiles[i], b.projectiles[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.explosives.size(); ++i)
    {
        if (!ExplosiveEqual(a.explosives[i], b.explosives[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.hazardZones.size(); ++i)
    {
        if (!HazardZoneEqual(a.hazardZones[i], b.hazardZones[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.heroDevices.size(); ++i)
    {
        if (!HeroDeviceEqual(a.heroDevices[i], b.heroDevices[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.statusEffects.size(); ++i)
    {
        if (!StatusEffectEqual(a.statusEffects[i], b.statusEffects[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.actionResults.size(); ++i)
    {
        if (!ActionResultEqual(a.actionResults[i], b.actionResults[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.worldEvents.size(); ++i)
    {
        if (!WorldEventEqual(a.worldEvents[i], b.worldEvents[i]))
        {
            return false;
        }
    }
    return true;
}

bool LobbyUpdateEqual(const LobbyUpdate& a, const LobbyUpdate& b)
{
    return a.playerName == b.playerName
        && a.selectedTeam == b.selectedTeam
        && a.selectedHero == b.selectedHero
        && a.ready == b.ready
        && a.startRequested == b.startRequested;
}

bool LobbyPlayerEqual(const LobbyPlayerState& a, const LobbyPlayerState& b)
{
    return a.clientId == b.clientId
        && a.assignedPlayerId == b.assignedPlayerId
        && a.playerName == b.playerName
        && a.selectedTeam == b.selectedTeam
        && a.selectedHero == b.selectedHero
        && a.ready == b.ready
        && a.connected == b.connected
        && a.startRequested == b.startRequested;
}

bool SnapshotEqual(const LobbySnapshot& a, const LobbySnapshot& b)
{
    if (a.revision != b.revision
        || a.hostClientId != b.hostClientId
        || a.serverName != b.serverName
        || a.privateServer != b.privateServer
        || a.maxPlayers != b.maxPlayers
        || a.teamCount != b.teamCount
        || a.maxTeamSize != b.maxTeamSize
        || a.heroCount != b.heroCount
        || a.enforceUniqueHeroesPerTeam != b.enforceUniqueHeroesPerTeam
        || a.requireAllReady != b.requireAllReady
        || a.canStart != b.canStart
        || a.matchStarting != b.matchStarting
        || a.matchStarted != b.matchStarted
        || a.worldBiome != b.worldBiome
        || a.worldLayout != b.worldLayout
        || a.matchMode != b.matchMode
        || a.statusMessage != b.statusMessage
        || a.players.size() != b.players.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.players.size(); ++i)
    {
        if (!LobbyPlayerEqual(a.players[i], b.players[i]))
        {
            return false;
        }
    }
    return true;
}

PlayerCommand MakeSampleCommand()
{
    PlayerCommand c;
    c.controlledPlayerId = 7;
    c.tick = 1234;
    c.moveForward = 1.0f;
    c.moveStrafe = -0.5f;
    c.aimYaw = 1.5708f;
    c.aimPitch = -0.25f;
    c.jump = true;
    c.sprint = true;
    c.sprintTapped = false;
    c.sneak = false;
    c.bridgeMode = true;
    c.selectedSlot = 5;
    c.attackPressed = true;
    c.attackHeld = false;
    c.attackReleased = true;
    c.placePressed = false;
    c.placeHeld = true;
    c.scopeHeld = true;
    c.interact = true;
    c.useAbility1 = true;
    c.useAbility2 = false;
    c.useUltimate = true;
    c.useHeal = false;
    c.useTeleport = true;
    c.useDash = false;
    c.useShoot = true;
    c.useFireball = false;
    c.useMolotov = true;
    c.useAlarm = false;
    c.actionSeq = 42;
    c.actionType = static_cast<int>(PlayerActionType::BuyItem);
    c.actionParamA = 101;
    c.actionParamB = 4;
    c.rewindTick = 1200;
    return c;
}

MatchSnapshot MakeSampleSnapshot()
{
    MatchSnapshot s;
    s.tick = 4242;
    s.lastProcessedCommandTick = 4217;
    s.matchTime = 70.5f;
    s.phase = MatchPhase::Playing;
    s.winnerTeamId = -1;

    PlayerSnapshot p0;
    p0.playerId = 1;
    p0.playerName = "Alice";
    p0.teamId = 0;
    p0.heroId = 3;
    p0.position = Vec3 { -38.0f, 1.5f, 0.0f };
    p0.velocity = Vec3 { 0.25f, -9.8f, 0.0f };
    p0.yaw = 0.125f;
    p0.health = 80;
    p0.maxHealth = 100;
    p0.alive = true;
    p0.eliminated = false;
    p0.respawnTimer = 0.0f;
    p0.selectedSlot = 2;
    p0.animationState = 5; // non-default, mirrors a HeroAnimationState index
    p0.animationTimer = 0.21f;
    p0.animationDuration = 0.32f;
    p0.inventory.present = true;
    p0.inventory.resources = { 13, 4, 1 };
    p0.inventory.hotbar = { { 9, 64 }, { 14, 1 }, { 0, 0 } };
    p0.inventory.main = { { 17, 2 }, { 18, 4 }, { 0, 0 } };
    p0.abilityHud.present = true;
    p0.abilityHud.active1Cooldown = 3.2f;
    p0.abilityHud.active1ActiveTimer = 0.0f;
    p0.abilityHud.active2Cooldown = 0.0f;
    p0.abilityHud.active2ActiveTimer = 1.4f;
    p0.abilityHud.ultimateCooldown = 12.5f;
    p0.abilityHud.ultimateActiveTimer = 0.0f;
    p0.abilityHud.ultimateCharge = 64.0f;
    p0.abilityHud.ultimatePrimed = true;
    p0.abilityHud.bowDrawTimer = 0.42f;
    p0.abilityHud.blasterState = 1;
    p0.abilityHud.blasterLoadTimer = 0.18f;
    p0.disguiseTeamId = -1;
    p0.disguiseHeroId = -1;
    s.players.push_back(p0);

    PlayerSnapshot p1;
    p1.playerId = 2;
    p1.playerName = "Bob";
    p1.teamId = 1;
    p1.heroId = 5;
    p1.position = Vec3 { 38.0f, 1.5f, 2.0f };
    p1.velocity = Vec3 {};
    p1.yaw = -2.75f;
    p1.health = 0;
    p1.maxHealth = 100;
    p1.alive = false;
    p1.eliminated = true;
    p1.respawnTimer = 7.0f;
    p1.selectedSlot = 0;
    p1.inventory.present = false; // stripped (as the visibility filter would)
    p1.disguiseTeamId = 0;        // pretend an actively-disguised Likho
    p1.disguiseHeroId = 7;
    s.players.push_back(p1);

    s.matchScores.push_back(PlayerScoreSnapshot { 1, 5, 2, 0, 128, 1 });
    s.matchScores.push_back(PlayerScoreSnapshot { 2, 1, 4, 1, 32, 0 });

    s.cores.push_back(CoreSnapshot { 0, 500, 500, true });
    s.cores.push_back(CoreSnapshot { 1, 0, 500, false });

    TeamChestSnapshot chest;
    chest.teamId = 0;
    chest.resources = { 16, 3, 1 };
    chest.slots = { { 23, 16 }, { 24, 3 }, { 6, 12 }, { 0, 0 } };
    s.teamChests.push_back(chest);

    s.generators.push_back(GeneratorSnapshot { 0, 0, Vec3 { -10.0f, 1.0f, 3.0f } });
    s.generators.push_back(GeneratorSnapshot { 2, -1, Vec3 { 0.0f, 2.0f, 0.0f } });
    s.pickups.push_back(PickupSnapshot { 0, 4, Vec3 { -9.0f, 1.2f, 3.5f } });
    s.droppedItems.push_back(DroppedItemSnapshot { 401, 14, 2, Vec3 { 6.0f, 1.3f, -4.0f } });

    BlockDelta d;
    d.tick = 4242;
    d.position = GridPos { 7, 21, -7 };
    d.oldType = BlockType::Air;
    d.newType = BlockType::StoneBlock;
    d.oldTeamId = -1;
    d.newTeamId = 0;
    d.ownerPlayerId = 1;
    d.reason = BlockDeltaReason::PlayerPlace;
    s.blockDeltas.push_back(d);

    s.projectiles.push_back(ProjectileSnapshot {
        3, 1, Vec3 { 1.0f, 2.0f, 3.0f }, Vec3 { 4.0f, 0.0f, 0.0f },
        1, 0, 1.5f, true, SnapshotVisibility::Public });
    s.explosives.push_back(ExplosiveSnapshot {
        4, Vec3 { -2.0f, 1.0f, 2.0f }, 1, 0, 2.25f, 3.5f, SnapshotVisibility::Public });
    s.hazardZones.push_back(HazardZoneSnapshot {
        5, Vec3 { 3.0f, 1.0f, -3.0f }, 2, 1, 4.0f, 2.0f, true, SnapshotVisibility::Public });
    s.heroDevices.push_back(HeroDeviceSnapshot {
        6, HeroDeviceType::KonvoyTrap, Vec3 { 8.0f, 1.0f, 8.0f },
        1, 0, -1, 7.0f, 48, SnapshotVisibility::OwnerTeam });
    s.statusEffects.push_back(StatusEffectSnapshot {
        7, StatusEffectType::Shield, Vec3 { -38.0f, 1.5f, 0.0f },
        1, -1, 0, 3.0f, 2, SnapshotVisibility::Private });
    s.actionResults.push_back(ActionResultSnapshot {
        1,
        7,
        42,
        static_cast<int>(PlayerActionType::BuyItem),
        0,
        -1,
        -1,
        -1,
        0,
        0,
        true,
        Vec3 { 4.0f, 5.0f, 6.0f },
        "Purchased wood.",
        { 128, 238, 166, 255 },
        1.6f,
        0.0f });
    // Phase 4/5 slice: utility / hero ability / projectile owner-private results
    // ride the same generic ActionResultSnapshot fields as BuyItem above (see
    // docs/NETWORK_PREP_PLAN.md) — exercised here so the roundtrip covers the
    // new PlayerActionType values, not just their (already generic) wire shape.
    s.actionResults.push_back(ActionResultSnapshot {
        1,
        8,
        0,
        static_cast<int>(PlayerActionType::UtilityUse),
        2,
        -1,
        -1,
        -1,
        0,
        1,
        true,
        Vec3 { -1.0f, 2.0f, 0.5f },
        "Dash pearl used.",
        { 112, 232, 255, 255 },
        0.30f,
        0.30f });
    s.actionResults.push_back(ActionResultSnapshot {
        1,
        9,
        0,
        static_cast<int>(PlayerActionType::HeroAbility),
        0,
        -1,
        -1,
        -1,
        1,
        32,
        false,
        Vec3 { 3.0f, 1.5f, -2.0f },
        "Radon: active2 on cooldown.",
        { 255, 96, 82, 255 },
        1.7f,
        0.95f });
    s.actionResults.push_back(ActionResultSnapshot {
        1,
        10,
        0,
        static_cast<int>(PlayerActionType::ProjectileLaunch),
        1,
        -1,
        -1,
        -1,
        0,
        1,
        true,
        Vec3 { 0.5f, 1.2f, 3.0f },
        "Bow: critical shot!",
        { 255, 255, 255, 255 },
        1.6f,
        0.0f });
    s.worldEvents.push_back(WorldEventSnapshot {
        1,
        static_cast<int>(WorldEventKind::PlayerDied),
        2,
        1,
        0,
        Vec3 { 1.0f, 1.0f, 1.0f },
        0,
        0,
        0,
        "топором Свидетеля" });

    return s;
}

LobbyUpdate MakeSampleLobbyUpdate()
{
    LobbyUpdate update;
    update.playerName = "Alice";
    update.selectedTeam = 0;
    update.selectedHero = 2;
    update.ready = true;
    update.startRequested = true;
    return update;
}

LobbySnapshot MakeSampleLobbySnapshot()
{
    LobbySnapshot snapshot;
    snapshot.revision = 77;
    snapshot.hostClientId = 11;
    snapshot.serverName = "Private Test";
    snapshot.privateServer = true;
    snapshot.maxPlayers = 8;
    snapshot.teamCount = 4;
    snapshot.maxTeamSize = 2;
    snapshot.heroCount = 6;
    snapshot.enforceUniqueHeroesPerTeam = true;
    snapshot.requireAllReady = true;
    snapshot.canStart = true;
    snapshot.matchStarting = true;
    snapshot.matchStarted = false;
    snapshot.worldBiome = 3;  // distinctive non-defaults so the roundtrip exercises them.
    snapshot.worldLayout = 1;
    snapshot.matchMode = 1;
    snapshot.statusMessage = "all ready";
    snapshot.players.push_back(
        LobbyPlayerState { 11, 1, "Alice", 0, 2, true, true, true });
    snapshot.players.push_back(
        LobbyPlayerState { 12, 2, "Bob", 1, 3, true, true, false });
    return snapshot;
}
} // namespace

int RunProtocolSmoke()
{
    std::cout << "protocol-smoke: version=" << kProtocolVersion
              << " magic=0x" << std::hex << kProtocolMagic << std::dec
              << " headerBytes=" << kPacketHeaderSize << '\n';

    bool ok = true;

    // 1) PlayerCommand roundtrip.
    const PlayerCommand command = MakeSampleCommand();
    const std::vector<std::uint8_t> commandBytes = EncodePlayerCommand(99, command);
    PacketHeader commandHeader;
    PlayerCommand decodedCommand;
    const DecodeStatus commandStatus = DecodePlayerCommand(
        commandBytes.data(), commandBytes.size(), commandHeader, decodedCommand);
    const bool commandOk = commandStatus == DecodeStatus::Ok
        && commandHeader.type == MessageType::PlayerCommand
        && commandHeader.sequence == 99
        && commandHeader.tick == command.tick
        && CommandEqual(command, decodedCommand);
    ok = ok && commandOk;
    std::cout << "protocol-smoke: PlayerCommand bytes=" << commandBytes.size()
              << " status=" << ToString(commandStatus)
              << " seq=" << commandHeader.sequence << " tick=" << commandHeader.tick
              << " roundtrip=" << (commandOk ? "ok" : "FAIL") << '\n';

    PlayerCommand command2 = command;
    command2.tick = command.tick + 1;
    command2.attackPressed = false;
    command2.placePressed = true;
    const std::vector<PlayerCommand> commandBatch { command, command2 };
    const std::vector<std::uint8_t> commandBatchBytes = EncodePlayerCommandBatch(98, commandBatch);
    PacketHeader commandBatchHeader;
    std::vector<PlayerCommand> decodedCommandBatch;
    const DecodeStatus commandBatchStatus = DecodePlayerCommandBatch(
        commandBatchBytes.data(), commandBatchBytes.size(), commandBatchHeader, decodedCommandBatch);
    const bool commandBatchOk = commandBatchStatus == DecodeStatus::Ok
        && commandBatchHeader.type == MessageType::PlayerCommandBatch
        && commandBatchHeader.sequence == 98
        && commandBatchHeader.tick == command2.tick
        && decodedCommandBatch.size() == commandBatch.size()
        && CommandEqual(decodedCommandBatch[0], command)
        && CommandEqual(decodedCommandBatch[1], command2);
    ok = ok && commandBatchOk;
    std::cout << "protocol-smoke: PlayerCommandBatch bytes=" << commandBatchBytes.size()
              << " status=" << ToString(commandBatchStatus)
              << " count=" << decodedCommandBatch.size()
              << " roundtrip=" << (commandBatchOk ? "ok" : "FAIL") << '\n';

    // 2) MatchSnapshot roundtrip (header + players + cores + block deltas).
    const MatchSnapshot snapshot = MakeSampleSnapshot();
    const std::vector<std::uint8_t> snapshotBytes = EncodeMatchSnapshot(100, snapshot);
    PacketHeader snapshotHeader;
    MatchSnapshot decodedSnapshot;
    const DecodeStatus snapshotStatus = DecodeMatchSnapshot(
        snapshotBytes.data(), snapshotBytes.size(), snapshotHeader, decodedSnapshot);
    const bool snapshotOk = snapshotStatus == DecodeStatus::Ok
        && snapshotHeader.type == MessageType::MatchSnapshot
        && snapshotHeader.tick == snapshot.tick
        && SnapshotEqual(snapshot, decodedSnapshot);
    ok = ok && snapshotOk;
    std::cout << "protocol-smoke: MatchSnapshot bytes=" << snapshotBytes.size()
              << " status=" << ToString(snapshotStatus)
              << " players=" << decodedSnapshot.players.size()
              << " cores=" << decodedSnapshot.cores.size()
              << " generators=" << decodedSnapshot.generators.size()
              << " pickups=" << decodedSnapshot.pickups.size()
              << " blockDeltas=" << decodedSnapshot.blockDeltas.size()
              << " projectiles=" << decodedSnapshot.projectiles.size()
              << " roundtrip=" << (snapshotOk ? "ok" : "FAIL") << '\n';

    // 3) Snapshot delta roundtrip + loss/reorder compatibility checks.
    MatchSnapshot deltaTarget = snapshot;
    deltaTarget.tick = snapshot.tick + 3;
    deltaTarget.lastProcessedCommandTick = snapshot.lastProcessedCommandTick + 2;
    deltaTarget.matchTime = snapshot.matchTime + 0.05f;
    deltaTarget.players[0].position.x += 1.25f;
    deltaTarget.players[0].inventory.main[0].count += 1;
    deltaTarget.players.pop_back();
    deltaTarget.matchScores[0].kills += 1;
    deltaTarget.matchScores[0].coreDamage += 9;
    deltaTarget.matchScores.pop_back();
    deltaTarget.cores[0].health -= 25;
    deltaTarget.teamChests[0].resources[0] += 5;
    deltaTarget.teamChests[0].slots[0].count += 5;
    deltaTarget.teamChests[0].slots.push_back(ItemStackSnapshot { 20, 1 });
    deltaTarget.pickups[0].amount += 2;
    deltaTarget.droppedItems.clear();
    BlockDelta d2 = snapshot.blockDeltas.front();
    d2.tick = deltaTarget.tick;
    d2.position.x += 1;
    d2.reason = BlockDeltaReason::ReplicationTest;
    deltaTarget.blockDeltas.push_back(d2);
    deltaTarget.projectiles[0].position.z += 2.0f;
    deltaTarget.explosives[0].remainingTimer -= 0.5f;
    deltaTarget.hazardZones.clear();
    deltaTarget.heroDevices[0].health -= 3;
    deltaTarget.statusEffects[0].remaining -= 0.25f;
    deltaTarget.actionResults[0].actionSeq += 1;
    deltaTarget.actionResults[0].success = false;
    deltaTarget.actionResults[0].message = "Purchase denied.";

    constexpr std::uint32_t kBaselineSequence = 100;
    constexpr std::uint32_t kDeltaSequence = 103;
    const MatchSnapshotDelta delta =
        BuildSnapshotDelta(snapshot, deltaTarget, kBaselineSequence, 1);
    const std::vector<std::uint8_t> deltaBytes = EncodeSnapshotDelta(kDeltaSequence, delta);
    PacketHeader deltaHeader;
    MatchSnapshotDelta decodedDelta;
    const DecodeStatus deltaStatus =
        DecodeSnapshotDelta(deltaBytes.data(), deltaBytes.size(), deltaHeader, decodedDelta);
    MatchSnapshot applied = snapshot;
    std::uint32_t appliedSequence = kBaselineSequence;
    const SnapshotDeltaApplyStatus applyStatus = ApplySnapshotDeltaIfCompatible(
        true, applied, appliedSequence, deltaHeader.sequence, decodedDelta);
    const bool deltaOk = deltaStatus == DecodeStatus::Ok
        && deltaHeader.type == MessageType::SnapshotDelta
        && applyStatus == SnapshotDeltaApplyStatus::Applied
        && appliedSequence == kDeltaSequence
        && SnapshotEqual(applied, deltaTarget)
        && deltaBytes.size() < snapshotBytes.size();
    ok = ok && deltaOk;
    std::cout << "protocol-smoke: SnapshotDelta bytes=" << deltaBytes.size()
              << " fullBytes=" << snapshotBytes.size()
              << " status=" << ToString(deltaStatus)
              << " apply=" << ToString(applyStatus)
              << " changedPlayers=" << decodedDelta.players.size()
              << " removedPlayers=" << decodedDelta.removedPlayerIds.size()
              << " bandwidth=" << (deltaBytes.size() < snapshotBytes.size() ? "ok" : "FAIL")
              << " roundtrip=" << (deltaOk ? "ok" : "FAIL") << '\n';

    MatchSnapshot oldState = applied;
    std::uint32_t oldSequence = appliedSequence;
    const SnapshotDeltaApplyStatus oldStatus = ApplySnapshotDeltaIfCompatible(
        true, oldState, oldSequence, kDeltaSequence - 1, decodedDelta);
    MatchSnapshot missingState;
    std::uint32_t missingSequence = 0;
    const SnapshotDeltaApplyStatus missingStatus = ApplySnapshotDeltaIfCompatible(
        false, missingState, missingSequence, kDeltaSequence, decodedDelta);
    MatchSnapshot gapState = snapshot;
    std::uint32_t gapSequence = kBaselineSequence - 1;
    const SnapshotDeltaApplyStatus gapStatus = ApplySnapshotDeltaIfCompatible(
        true, gapState, gapSequence, kDeltaSequence, decodedDelta);
    const bool deltaLossOk = oldStatus == SnapshotDeltaApplyStatus::OldSnapshot
        && missingStatus == SnapshotDeltaApplyStatus::MissingBaseline
        && gapStatus == SnapshotDeltaApplyStatus::BaselineMismatch
        && !SnapshotDeltaStatusNeedsFullResync(oldStatus)
        && SnapshotDeltaStatusNeedsFullResync(missingStatus)
        && SnapshotDeltaStatusNeedsFullResync(gapStatus);
    ok = ok && deltaLossOk;
    std::cout << "protocol-smoke: delta ordering old=" << ToString(oldStatus)
              << " missing=" << ToString(missingStatus)
              << " gap=" << ToString(gapStatus)
              << " fullResyncRequest=" << (deltaLossOk ? "ok" : "FAIL")
              << " handled=" << (deltaLossOk ? "ok" : "FAIL") << '\n';

    MatchSnapshot branchTarget = deltaTarget;
    branchTarget.tick += 2;
    branchTarget.matchTime += 0.033f;
    branchTarget.players[0].position.x += 0.5f;
    const MatchSnapshotDelta branchDelta =
        BuildSnapshotDelta(snapshot, branchTarget, kBaselineSequence, 1);
    MatchSnapshot currentClientState = applied;
    std::uint32_t currentClientSequence = appliedSequence;
    constexpr std::uint32_t kBranchDeltaSequence = 104;
    const SnapshotDeltaApplyStatus branchCurrentStatus = ApplySnapshotDeltaIfCompatible(
        true, currentClientState, currentClientSequence, kBranchDeltaSequence, branchDelta);
    MatchSnapshot recoveredFromHistory = snapshot;
    std::uint32_t recoveredSequence = kBaselineSequence;
    const SnapshotDeltaApplyStatus branchHistoryStatus = ApplySnapshotDeltaIfCompatible(
        true, recoveredFromHistory, recoveredSequence, kBranchDeltaSequence, branchDelta);
    const bool deltaHistoryOk = branchCurrentStatus == SnapshotDeltaApplyStatus::BaselineMismatch
        && branchHistoryStatus == SnapshotDeltaApplyStatus::Applied
        && recoveredSequence == kBranchDeltaSequence
        && SnapshotEqual(recoveredFromHistory, branchTarget);
    ok = ok && deltaHistoryOk;
    std::cout << "protocol-smoke: delta history recovery current="
              << ToString(branchCurrentStatus)
              << " history=" << ToString(branchHistoryStatus)
              << " handled=" << (deltaHistoryOk ? "ok" : "FAIL") << '\n';

    // 4) Lobby packets roundtrip.
    const LobbyUpdate lobbyUpdate = MakeSampleLobbyUpdate();
    const std::vector<std::uint8_t> lobbyUpdateBytes = EncodeLobbyUpdate(101, lobbyUpdate);
    PacketHeader lobbyUpdateHeader;
    LobbyUpdate decodedLobbyUpdate;
    const DecodeStatus lobbyUpdateStatus = DecodeLobbyUpdate(
        lobbyUpdateBytes.data(), lobbyUpdateBytes.size(), lobbyUpdateHeader, decodedLobbyUpdate);
    const bool lobbyUpdateOk = lobbyUpdateStatus == DecodeStatus::Ok
        && lobbyUpdateHeader.type == MessageType::LobbyUpdate
        && lobbyUpdateHeader.sequence == 101
        && LobbyUpdateEqual(lobbyUpdate, decodedLobbyUpdate);
    ok = ok && lobbyUpdateOk;

    const LobbySnapshot lobbySnapshot = MakeSampleLobbySnapshot();
    const std::vector<std::uint8_t> lobbySnapshotBytes = EncodeLobbySnapshot(102, lobbySnapshot);
    PacketHeader lobbySnapshotHeader;
    LobbySnapshot decodedLobbySnapshot;
    const DecodeStatus lobbySnapshotStatus = DecodeLobbySnapshot(
        lobbySnapshotBytes.data(), lobbySnapshotBytes.size(), lobbySnapshotHeader, decodedLobbySnapshot);
    const bool lobbySnapshotOk = lobbySnapshotStatus == DecodeStatus::Ok
        && lobbySnapshotHeader.type == MessageType::LobbySnapshot
        && lobbySnapshotHeader.tick == lobbySnapshot.revision
        && SnapshotEqual(lobbySnapshot, decodedLobbySnapshot);
    ok = ok && lobbySnapshotOk;
    std::cout << "protocol-smoke: LobbyUpdate bytes=" << lobbyUpdateBytes.size()
              << " status=" << ToString(lobbyUpdateStatus)
              << " roundtrip=" << (lobbyUpdateOk ? "ok" : "FAIL") << '\n';
    std::cout << "protocol-smoke: LobbySnapshot bytes=" << lobbySnapshotBytes.size()
              << " status=" << ToString(lobbySnapshotStatus)
              << " players=" << decodedLobbySnapshot.players.size()
              << " roundtrip=" << (lobbySnapshotOk ? "ok" : "FAIL") << '\n';

    // 4) Truncated packets must be rejected, never crash. Try every cut length.
    bool truncationSafe = true;
    for (std::size_t cut = 0; cut < snapshotBytes.size(); ++cut)
    {
        PacketHeader h;
        MatchSnapshot partial;
        const DecodeStatus s = DecodeMatchSnapshot(snapshotBytes.data(), cut, h, partial);
        if (s == DecodeStatus::Ok)
        {
            truncationSafe = false; // a short buffer must never decode as Ok
            break;
        }
    }
    // Also a zero-length and a sub-header buffer.
    {
        PacketHeader h;
        PlayerCommand pc;
        const DecodeStatus z = DecodePlayerCommand(nullptr, 0, h, pc);
        const DecodeStatus tiny = DecodePlayerCommand(commandBytes.data(), 3, h, pc);
        truncationSafe = truncationSafe && z == DecodeStatus::TooShort && tiny == DecodeStatus::TooShort;
    }
    ok = ok && truncationSafe;
    std::cout << "protocol-smoke: truncation rejected=" << (truncationSafe ? "ok" : "FAIL") << '\n';

    // 5) Version mismatch: corrupt the version field (bytes 4..5) and decode.
    std::vector<std::uint8_t> versioned = commandBytes;
    versioned[4] = static_cast<std::uint8_t>((kProtocolVersion + 1) & 0xFF);
    versioned[5] = static_cast<std::uint8_t>(((kProtocolVersion + 1) >> 8) & 0xFF);
    PacketHeader versionHeader;
    PlayerCommand ignored;
    const DecodeStatus versionStatus = DecodePlayerCommand(
        versioned.data(), versioned.size(), versionHeader, ignored);
    const bool versionOk = versionStatus == DecodeStatus::VersionMismatch
        && versionHeader.protocolVersion == kProtocolVersion + 1;
    ok = ok && versionOk;
    std::cout << "protocol-smoke: versionMismatch status=" << ToString(versionStatus)
              << " wireVersion=" << versionHeader.protocolVersion
              << " handled=" << (versionOk ? "ok" : "FAIL") << '\n';

    // 6) Bad magic / random bytes: rejected cleanly.
    const std::uint8_t garbage[] { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05 };
    PacketHeader garbageHeader;
    PlayerCommand garbageOut;
    const DecodeStatus garbageStatus = DecodePlayerCommand(
        garbage, sizeof(garbage), garbageHeader, garbageOut);
    const bool garbageOk = garbageStatus == DecodeStatus::BadMagic;
    ok = ok && garbageOk;
    std::cout << "protocol-smoke: badMagic status=" << ToString(garbageStatus)
              << " handled=" << (garbageOk ? "ok" : "FAIL") << '\n';

    // 7) Wrong-type: decode a command packet as a snapshot.
    PacketHeader wrongHeader;
    MatchSnapshot wrongOut;
    const DecodeStatus wrongStatus = DecodeMatchSnapshot(
        commandBytes.data(), commandBytes.size(), wrongHeader, wrongOut);
    const bool wrongOk = wrongStatus == DecodeStatus::WrongType;
    ok = ok && wrongOk;
    std::cout << "protocol-smoke: wrongType status=" << ToString(wrongStatus)
              << " handled=" << (wrongOk ? "ok" : "FAIL") << '\n';

    // 8) PacketFragment codec: chunk a synthetic oversized packet, decode each
    // fragment, reassemble by the fixed layout, and compare byte-for-byte.
    bool fragmentOk = true;
    {
        std::vector<std::uint8_t> big(3000);
        for (std::size_t i = 0; i < big.size(); ++i)
        {
            big[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
        }
        constexpr std::size_t chunkBytes = 1024;
        const std::uint16_t count =
            static_cast<std::uint16_t>((big.size() + chunkBytes - 1) / chunkBytes);
        std::vector<std::uint8_t> reassembled(big.size());
        for (std::uint16_t i = 0; i < count && fragmentOk; ++i)
        {
            const std::size_t offset = static_cast<std::size_t>(i) * chunkBytes;
            const std::size_t len = big.size() - offset < chunkBytes
                ? big.size() - offset
                : chunkBytes;
            const std::vector<std::uint8_t> wire = EncodePacketFragment(
                900 + i, /*fragmentId*/ 777, i, count,
                static_cast<std::uint32_t>(big.size()), big.data() + offset, len);
            PacketHeader fragHeader;
            std::uint32_t fragmentId = 0;
            std::uint16_t index = 0;
            std::uint16_t total = 0;
            std::uint32_t totalSize = 0;
            std::vector<std::uint8_t> chunk;
            const DecodeStatus status = DecodePacketFragment(
                wire.data(), wire.size(), fragHeader, fragmentId, index, total, totalSize, chunk);
            fragmentOk = fragmentOk
                && status == DecodeStatus::Ok
                && fragmentId == 777
                && index == i
                && total == count
                && totalSize == big.size()
                && chunk.size() == len;
            if (fragmentOk)
            {
                std::copy(chunk.begin(), chunk.end(),
                          reassembled.begin() + static_cast<std::ptrdiff_t>(offset));
            }
        }
        fragmentOk = fragmentOk && reassembled == big;
        // A truncated fragment must be rejected, not crash.
        const std::vector<std::uint8_t> wire = EncodePacketFragment(
            1, 2, 0, 1, 8, big.data(), 8);
        PacketHeader truncHeader;
        std::uint32_t a = 0;
        std::uint16_t b = 0;
        std::uint16_t c = 0;
        std::uint32_t d = 0;
        std::vector<std::uint8_t> e;
        fragmentOk = fragmentOk
            && DecodePacketFragment(wire.data(), wire.size() - 4, truncHeader, a, b, c, d, e)
                != DecodeStatus::Ok;
    }
    ok = ok && fragmentOk;
    std::cout << "protocol-smoke: packetFragment roundtrip=" << (fragmentOk ? "ok" : "FAIL") << '\n';

    std::cout << (ok ? "PROTOCOL_SMOKE_OK" : "PROTOCOL_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 6;
}
