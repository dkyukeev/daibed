#include "BotAI.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{
constexpr const char* kRoleTuningNames[] { "defender", "rusher", "collector", "fighter" };

bool ExtractJsonNumber(const std::string& text, const std::string& key, float& value)
{
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = text.find(pattern);
    if (keyPos == std::string::npos)
    {
        return false;
    }

    const std::size_t colon = text.find(':', keyPos + pattern.size());
    if (colon == std::string::npos)
    {
        return false;
    }

    const char* begin = text.c_str() + colon + 1;
    char* end = nullptr;
    const float parsed = std::strtof(begin, &end);
    if (end == begin)
    {
        return false;
    }

    value = parsed;
    return true;
}

bool ExtractJsonInt(const std::string& text, const std::string& key, int& value)
{
    float parsed = 0.0f;
    if (!ExtractJsonNumber(text, key, parsed))
    {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool ExtractJsonString(const std::string& text, const std::string& key, std::string& value)
{
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = text.find(pattern);
    if (keyPos == std::string::npos)
    {
        return false;
    }

    const std::size_t colon = text.find(':', keyPos + pattern.size());
    if (colon == std::string::npos)
    {
        return false;
    }
    const std::size_t firstQuote = text.find('"', colon + 1);
    if (firstQuote == std::string::npos)
    {
        return false;
    }

    std::string parsed;
    bool escaping = false;
    for (std::size_t i = firstQuote + 1; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (escaping)
        {
            parsed.push_back(ch);
            escaping = false;
            continue;
        }
        if (ch == '\\')
        {
            escaping = true;
            continue;
        }
        if (ch == '"')
        {
            value = parsed;
            return true;
        }
        parsed.push_back(ch);
    }

    return false;
}

void ExtractRoleTuning(const std::string& text, const std::string& prefix, BotRoleTuning& role)
{
    ExtractJsonNumber(text, prefix + "desiredBlocks", role.desiredBlocks);
    ExtractJsonNumber(text, prefix + "retreatHealthCoreAlive", role.retreatHealthCoreAlive);
    ExtractJsonNumber(text, prefix + "retreatHealthFinalLife", role.retreatHealthFinalLife);
    ExtractJsonNumber(text, prefix + "fightHealth", role.fightHealth);
    ExtractJsonNumber(text, prefix + "lootReturnValue", role.lootReturnValue);
    ExtractJsonNumber(text, prefix + "engageRange", role.engageRange);
    ExtractJsonNumber(text, prefix + "roleLockSeconds", role.roleLockSeconds);
    ExtractJsonNumber(text, prefix + "pressureBiasScale", role.pressureBiasScale);
    ExtractJsonNumber(text, prefix + "defenseBiasScale", role.defenseBiasScale);
    ExtractJsonNumber(text, prefix + "resourceBiasScale", role.resourceBiasScale);
    ExtractJsonNumber(text, prefix + "combatBiasScale", role.combatBiasScale);
    ExtractJsonNumber(text, prefix + "aggression", role.aggression);
}

void ExtractGenomeFields(const std::string& text, const std::string& prefix, BotTuningGenome& genome)
{
    ExtractJsonInt(text, prefix + "schemaVersion", genome.schemaVersion);
    ExtractJsonString(text, prefix + "id", genome.id);
    ExtractJsonInt(text, prefix + "generation", genome.generation);
    ExtractJsonNumber(text, prefix + "fitness", genome.fitness);

    for (int i = 0; i < 4; ++i)
    {
        ExtractRoleTuning(text, prefix + kRoleTuningNames[i] + ".", genome.roles[i]);
    }

    ExtractJsonNumber(text, prefix + "intentLockScale", genome.intentLockScale);
    ExtractJsonNumber(text, prefix + "roleLockScale", genome.roleLockScale);
    ExtractJsonNumber(text, prefix + "fightRequiredMarginEasy", genome.fightRequiredMarginEasy);
    ExtractJsonNumber(text, prefix + "fightRequiredMarginNormal", genome.fightRequiredMarginNormal);
    ExtractJsonNumber(text, prefix + "fightRequiredMarginHard", genome.fightRequiredMarginHard);
    ExtractJsonNumber(text, prefix + "retreatPowerMarginEasy", genome.retreatPowerMarginEasy);
    ExtractJsonNumber(text, prefix + "retreatPowerMarginNormal", genome.retreatPowerMarginNormal);
    ExtractJsonNumber(text, prefix + "retreatPowerMarginHard", genome.retreatPowerMarginHard);
    ExtractJsonNumber(text, prefix + "allyAssistWeight", genome.allyAssistWeight);
    ExtractJsonNumber(text, prefix + "enemyAssistWeight", genome.enemyAssistWeight);
    ExtractJsonNumber(text, prefix + "shieldPower", genome.shieldPower);
    ExtractJsonNumber(text, prefix + "speedBoostPower", genome.speedBoostPower);
    ExtractJsonNumber(text, prefix + "strategicDefenseUrgencyScale", genome.strategicDefenseUrgencyScale);
    ExtractJsonNumber(text, prefix + "repairUrgencyScale", genome.repairUrgencyScale);
    ExtractJsonNumber(text, prefix + "strategicAttackUrgencyScale", genome.strategicAttackUrgencyScale);
    ExtractJsonNumber(text, prefix + "lateAttackUrgencyScale", genome.lateAttackUrgencyScale);
    ExtractJsonNumber(text, prefix + "attackSlotBonus", genome.attackSlotBonus);
    ExtractJsonNumber(text, prefix + "attackSlotPenalty", genome.attackSlotPenalty);
    ExtractJsonNumber(text, prefix + "breakCoordinationPenalty", genome.breakCoordinationPenalty);
    ExtractJsonNumber(text, prefix + "pressureCoordinationPenalty", genome.pressureCoordinationPenalty);
    ExtractJsonNumber(text, prefix + "lateCoordinationPenalty", genome.lateCoordinationPenalty);
    ExtractJsonNumber(text, prefix + "strategicEconomyBonus", genome.strategicEconomyBonus);
    ExtractJsonNumber(text, prefix + "personalEconomyBonus", genome.personalEconomyBonus);
    ExtractJsonNumber(text, prefix + "strategicPressureEconomyPenalty", genome.strategicPressureEconomyPenalty);
    ExtractJsonNumber(text, prefix + "easyPlanCadence", genome.easyPlanCadence);
    ExtractJsonNumber(text, prefix + "normalPlanCadence", genome.normalPlanCadence);
    ExtractJsonNumber(text, prefix + "hardPlanCadence", genome.hardPlanCadence);
    ExtractJsonNumber(text, prefix + "earlyEconomySeconds", genome.earlyEconomySeconds);
    ExtractJsonNumber(text, prefix + "pressurePhaseSeconds", genome.pressurePhaseSeconds);
    ExtractJsonNumber(text, prefix + "latePressureSeconds", genome.latePressureSeconds);
    ExtractJsonNumber(text, prefix + "allInSeconds", genome.allInSeconds);
    ExtractJsonNumber(text, prefix + "navigation.runupDistanceBlocks", genome.navigationRunupDistanceBlocks);
    ExtractJsonNumber(text, prefix + "navigation.takeoffDelaySeconds", genome.navigationTakeoffDelaySeconds);
    ExtractJsonNumber(text, prefix + "navigation.takeoffEdgeOffsetBlocks", genome.navigationTakeoffEdgeOffsetBlocks);
    ExtractJsonNumber(text, prefix + "navigation.takeoffGapScale", genome.navigationTakeoffGapScale);
    ExtractJsonNumber(text, prefix + "navigation.airControlScale", genome.navigationAirControlScale);
    ExtractJsonNumber(text, prefix + "navigation.landingCorrectionGain", genome.navigationLandingCorrectionGain);
    ExtractJsonNumber(text, prefix + "navigation.fallRiskPenalty", genome.navigationFallRiskPenalty);
}

void WriteRoleTuningJson(std::ofstream& file, const char* prefix, const BotRoleTuning& role, bool comma)
{
    file << "  \"" << prefix << ".desiredBlocks\": " << role.desiredBlocks << ",\n";
    file << "  \"" << prefix << ".retreatHealthCoreAlive\": " << role.retreatHealthCoreAlive << ",\n";
    file << "  \"" << prefix << ".retreatHealthFinalLife\": " << role.retreatHealthFinalLife << ",\n";
    file << "  \"" << prefix << ".fightHealth\": " << role.fightHealth << ",\n";
    file << "  \"" << prefix << ".lootReturnValue\": " << role.lootReturnValue << ",\n";
    file << "  \"" << prefix << ".engageRange\": " << role.engageRange << ",\n";
    file << "  \"" << prefix << ".roleLockSeconds\": " << role.roleLockSeconds << ",\n";
    file << "  \"" << prefix << ".pressureBiasScale\": " << role.pressureBiasScale << ",\n";
    file << "  \"" << prefix << ".defenseBiasScale\": " << role.defenseBiasScale << ",\n";
    file << "  \"" << prefix << ".resourceBiasScale\": " << role.resourceBiasScale << ",\n";
    file << "  \"" << prefix << ".combatBiasScale\": " << role.combatBiasScale << ",\n";
    file << "  \"" << prefix << ".aggression\": " << role.aggression << (comma ? "," : "") << "\n";
}

unsigned int HashFloat(unsigned int hash, float value)
{
    const int scaled = static_cast<int>(value * 1000.0f);
    hash ^= static_cast<unsigned int>(scaled) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    return hash;
}

float Clamped(float value, float minValue, float maxValue)
{
    return std::clamp(value, minValue, maxValue);
}
}

BotTuningGenome DefaultBotTuningGenome()
{
    BotTuningGenome genome {};
    genome.schemaVersion = 2;
    genome.id = "default";
    genome.generation = 0;
    genome.roles[static_cast<int>(BotRole::Defender)] = BotRoleTuning { 24.0f, 34.0f, 20.0f, 26.0f, 30.0f, 7.4f, 7.0f, 0.75f, 1.20f, 0.85f, 0.90f, 0.80f };
    genome.roles[static_cast<int>(BotRole::Rusher)] = BotRoleTuning { 36.0f, 28.0f, 20.0f, 22.0f, 30.0f, 4.6f, 5.0f, 1.20f, 0.72f, 0.72f, 0.95f, 1.20f };
    genome.roles[static_cast<int>(BotRole::Collector)] = BotRoleTuning { 20.0f, 44.0f, 20.0f, 54.0f, 14.0f, 3.1f, 6.0f, 0.55f, 0.90f, 1.25f, 0.70f, 0.65f };
    genome.roles[static_cast<int>(BotRole::Fighter)] = BotRoleTuning { 28.0f, 30.0f, 20.0f, 28.0f, 30.0f, 8.6f, 4.6f, 1.00f, 0.95f, 0.75f, 1.25f, 1.10f };
    return genome;
}

void ClampBotTuningGenome(BotTuningGenome& genome)
{
    genome.schemaVersion = std::max(2, genome.schemaVersion);
    genome.generation = std::max(0, genome.generation);
    for (BotRoleTuning& role : genome.roles)
    {
        role.desiredBlocks = Clamped(role.desiredBlocks, 4.0f, 64.0f);
        role.retreatHealthCoreAlive = Clamped(role.retreatHealthCoreAlive, 4.0f, 92.0f);
        role.retreatHealthFinalLife = Clamped(role.retreatHealthFinalLife, 4.0f, 70.0f);
        role.fightHealth = Clamped(role.fightHealth, 4.0f, 92.0f);
        role.lootReturnValue = Clamped(role.lootReturnValue, 4.0f, 90.0f);
        role.engageRange = Clamped(role.engageRange, 1.5f, 14.0f);
        role.roleLockSeconds = Clamped(role.roleLockSeconds, 1.0f, 16.0f);
        role.pressureBiasScale = Clamped(role.pressureBiasScale, 0.20f, 2.20f);
        role.defenseBiasScale = Clamped(role.defenseBiasScale, 0.20f, 2.20f);
        role.resourceBiasScale = Clamped(role.resourceBiasScale, 0.20f, 2.20f);
        role.combatBiasScale = Clamped(role.combatBiasScale, 0.20f, 2.20f);
        role.aggression = Clamped(role.aggression, 0.25f, 2.20f);
    }

    genome.intentLockScale = Clamped(genome.intentLockScale, 0.45f, 1.80f);
    genome.roleLockScale = Clamped(genome.roleLockScale, 0.45f, 1.80f);
    genome.fightRequiredMarginEasy = Clamped(genome.fightRequiredMarginEasy, -20.0f, 60.0f);
    genome.fightRequiredMarginNormal = Clamped(genome.fightRequiredMarginNormal, -35.0f, 45.0f);
    genome.fightRequiredMarginHard = Clamped(genome.fightRequiredMarginHard, -55.0f, 30.0f);
    genome.retreatPowerMarginEasy = Clamped(genome.retreatPowerMarginEasy, -80.0f, 10.0f);
    genome.retreatPowerMarginNormal = Clamped(genome.retreatPowerMarginNormal, -85.0f, 5.0f);
    genome.retreatPowerMarginHard = Clamped(genome.retreatPowerMarginHard, -100.0f, 0.0f);
    genome.allyAssistWeight = Clamped(genome.allyAssistWeight, 0.15f, 0.95f);
    genome.enemyAssistWeight = Clamped(genome.enemyAssistWeight, 0.15f, 1.10f);
    genome.shieldPower = Clamped(genome.shieldPower, 0.0f, 40.0f);
    genome.speedBoostPower = Clamped(genome.speedBoostPower, 0.0f, 32.0f);
    genome.strategicDefenseUrgencyScale = Clamped(genome.strategicDefenseUrgencyScale, 0.05f, 0.85f);
    genome.repairUrgencyScale = Clamped(genome.repairUrgencyScale, 0.05f, 0.95f);
    genome.strategicAttackUrgencyScale = Clamped(genome.strategicAttackUrgencyScale, 0.05f, 0.85f);
    genome.lateAttackUrgencyScale = Clamped(genome.lateAttackUrgencyScale, 0.0f, 0.75f);
    genome.attackSlotBonus = Clamped(genome.attackSlotBonus, -80.0f, 220.0f);
    genome.attackSlotPenalty = Clamped(genome.attackSlotPenalty, -260.0f, 40.0f);
    genome.breakCoordinationPenalty = Clamped(genome.breakCoordinationPenalty, 0.0f, 180.0f);
    genome.pressureCoordinationPenalty = Clamped(genome.pressureCoordinationPenalty, 0.0f, 220.0f);
    genome.lateCoordinationPenalty = Clamped(genome.lateCoordinationPenalty, 0.0f, 220.0f);
    genome.strategicEconomyBonus = Clamped(genome.strategicEconomyBonus, -80.0f, 220.0f);
    genome.personalEconomyBonus = Clamped(genome.personalEconomyBonus, -80.0f, 260.0f);
    genome.strategicPressureEconomyPenalty = Clamped(genome.strategicPressureEconomyPenalty, -60.0f, 180.0f);
    genome.easyPlanCadence = Clamped(genome.easyPlanCadence, 0.8f, 8.0f);
    genome.normalPlanCadence = Clamped(genome.normalPlanCadence, 0.6f, 6.0f);
    genome.hardPlanCadence = Clamped(genome.hardPlanCadence, 0.4f, 5.0f);
    genome.earlyEconomySeconds = Clamped(genome.earlyEconomySeconds, 12.0f, 80.0f);
    genome.pressurePhaseSeconds = Clamped(genome.pressurePhaseSeconds, 18.0f, 95.0f);
    genome.latePressureSeconds = Clamped(genome.latePressureSeconds, 70.0f, 190.0f);
    genome.allInSeconds = Clamped(genome.allInSeconds, 110.0f, 290.0f);
    genome.navigationRunupDistanceBlocks = Clamped(genome.navigationRunupDistanceBlocks, 0.55f, 2.75f);
    genome.navigationTakeoffDelaySeconds = Clamped(genome.navigationTakeoffDelaySeconds, 0.04f, 0.34f);
    genome.navigationTakeoffEdgeOffsetBlocks = Clamped(genome.navigationTakeoffEdgeOffsetBlocks, 0.02f, 0.45f);
    genome.navigationTakeoffGapScale = Clamped(genome.navigationTakeoffGapScale, 0.0f, 0.35f);
    genome.navigationAirControlScale = Clamped(genome.navigationAirControlScale, 0.25f, 1.0f);
    genome.navigationLandingCorrectionGain = Clamped(genome.navigationLandingCorrectionGain, 0.15f, 1.0f);
    genome.navigationFallRiskPenalty = Clamped(genome.navigationFallRiskPenalty, 0.25f, 4.0f);
}

unsigned int BotTuningGenomeHash(const BotTuningGenome& genome)
{
    unsigned int hash = 2166136261u;
    for (char ch : genome.id)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619u;
    }
    hash ^= static_cast<unsigned int>(genome.generation);
    for (const BotRoleTuning& role : genome.roles)
    {
        hash = HashFloat(hash, role.desiredBlocks);
        hash = HashFloat(hash, role.retreatHealthCoreAlive);
        hash = HashFloat(hash, role.fightHealth);
        hash = HashFloat(hash, role.engageRange);
        hash = HashFloat(hash, role.aggression);
    }
    hash = HashFloat(hash, genome.intentLockScale);
    hash = HashFloat(hash, genome.fightRequiredMarginNormal);
    hash = HashFloat(hash, genome.allyAssistWeight);
    hash = HashFloat(hash, genome.strategicAttackUrgencyScale);
    hash = HashFloat(hash, genome.allInSeconds);
    hash = HashFloat(hash, genome.navigationRunupDistanceBlocks);
    hash = HashFloat(hash, genome.navigationTakeoffDelaySeconds);
    hash = HashFloat(hash, genome.navigationTakeoffEdgeOffsetBlocks);
    hash = HashFloat(hash, genome.navigationTakeoffGapScale);
    hash = HashFloat(hash, genome.navigationAirControlScale);
    hash = HashFloat(hash, genome.navigationLandingCorrectionGain);
    hash = HashFloat(hash, genome.navigationFallRiskPenalty);
    return hash;
}

bool LoadBotTuningGenomeSetFromJsonFile(
    const std::string& path,
    std::array<BotTuningGenome, 4>& genomes,
    std::string* errorMessage)
{
    std::ifstream file(path);
    if (!file)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "could not open " + path;
        }
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    BotTuningGenome base = DefaultBotTuningGenome();
    ExtractGenomeFields(text, "", base);
    ClampBotTuningGenome(base);
    for (BotTuningGenome& genome : genomes)
    {
        genome = base;
    }

    for (int teamId = 0; teamId < 4; ++teamId)
    {
        BotTuningGenome teamGenome = base;
        ExtractGenomeFields(text, "team" + std::to_string(teamId) + ".", teamGenome);
        ClampBotTuningGenome(teamGenome);
        genomes[teamId] = teamGenome;
    }

    return true;
}

bool WriteBotTuningGenomeJsonFile(
    const std::string& path,
    const BotTuningGenome& genome,
    std::string* errorMessage)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "could not write " + path;
        }
        return false;
    }

    file << "{\n";
    file << "  \"schemaVersion\": " << genome.schemaVersion << ",\n";
    file << "  \"id\": \"" << genome.id << "\",\n";
    file << "  \"generation\": " << genome.generation << ",\n";
    file << "  \"fitness\": " << genome.fitness << ",\n";
    for (int i = 0; i < 4; ++i)
    {
        WriteRoleTuningJson(file, kRoleTuningNames[i], genome.roles[i], true);
    }
    file << "  \"intentLockScale\": " << genome.intentLockScale << ",\n";
    file << "  \"roleLockScale\": " << genome.roleLockScale << ",\n";
    file << "  \"fightRequiredMarginEasy\": " << genome.fightRequiredMarginEasy << ",\n";
    file << "  \"fightRequiredMarginNormal\": " << genome.fightRequiredMarginNormal << ",\n";
    file << "  \"fightRequiredMarginHard\": " << genome.fightRequiredMarginHard << ",\n";
    file << "  \"retreatPowerMarginEasy\": " << genome.retreatPowerMarginEasy << ",\n";
    file << "  \"retreatPowerMarginNormal\": " << genome.retreatPowerMarginNormal << ",\n";
    file << "  \"retreatPowerMarginHard\": " << genome.retreatPowerMarginHard << ",\n";
    file << "  \"allyAssistWeight\": " << genome.allyAssistWeight << ",\n";
    file << "  \"enemyAssistWeight\": " << genome.enemyAssistWeight << ",\n";
    file << "  \"shieldPower\": " << genome.shieldPower << ",\n";
    file << "  \"speedBoostPower\": " << genome.speedBoostPower << ",\n";
    file << "  \"strategicDefenseUrgencyScale\": " << genome.strategicDefenseUrgencyScale << ",\n";
    file << "  \"repairUrgencyScale\": " << genome.repairUrgencyScale << ",\n";
    file << "  \"strategicAttackUrgencyScale\": " << genome.strategicAttackUrgencyScale << ",\n";
    file << "  \"lateAttackUrgencyScale\": " << genome.lateAttackUrgencyScale << ",\n";
    file << "  \"attackSlotBonus\": " << genome.attackSlotBonus << ",\n";
    file << "  \"attackSlotPenalty\": " << genome.attackSlotPenalty << ",\n";
    file << "  \"breakCoordinationPenalty\": " << genome.breakCoordinationPenalty << ",\n";
    file << "  \"pressureCoordinationPenalty\": " << genome.pressureCoordinationPenalty << ",\n";
    file << "  \"lateCoordinationPenalty\": " << genome.lateCoordinationPenalty << ",\n";
    file << "  \"strategicEconomyBonus\": " << genome.strategicEconomyBonus << ",\n";
    file << "  \"personalEconomyBonus\": " << genome.personalEconomyBonus << ",\n";
    file << "  \"strategicPressureEconomyPenalty\": " << genome.strategicPressureEconomyPenalty << ",\n";
    file << "  \"easyPlanCadence\": " << genome.easyPlanCadence << ",\n";
    file << "  \"normalPlanCadence\": " << genome.normalPlanCadence << ",\n";
    file << "  \"hardPlanCadence\": " << genome.hardPlanCadence << ",\n";
    file << "  \"earlyEconomySeconds\": " << genome.earlyEconomySeconds << ",\n";
    file << "  \"pressurePhaseSeconds\": " << genome.pressurePhaseSeconds << ",\n";
    file << "  \"latePressureSeconds\": " << genome.latePressureSeconds << ",\n";
    file << "  \"allInSeconds\": " << genome.allInSeconds << ",\n";
    file << "  \"navigation.runupDistanceBlocks\": " << genome.navigationRunupDistanceBlocks << ",\n";
    file << "  \"navigation.takeoffDelaySeconds\": " << genome.navigationTakeoffDelaySeconds << ",\n";
    file << "  \"navigation.takeoffEdgeOffsetBlocks\": " << genome.navigationTakeoffEdgeOffsetBlocks << ",\n";
    file << "  \"navigation.takeoffGapScale\": " << genome.navigationTakeoffGapScale << ",\n";
    file << "  \"navigation.airControlScale\": " << genome.navigationAirControlScale << ",\n";
    file << "  \"navigation.landingCorrectionGain\": " << genome.navigationLandingCorrectionGain << ",\n";
    file << "  \"navigation.fallRiskPenalty\": " << genome.navigationFallRiskPenalty << "\n";
    file << "}\n";
    return true;
}

const char* ToString(BotState state)
{
    switch (state)
    {
    case BotState::Collect:
        return "Сбор";
    case BotState::Shop:
        return "Магазин";
    case BotState::Bridge:
        return "Мост";
    case BotState::BreakDefense:
        return "Взлом защиты";
    case BotState::AttackCore:
        return "Атака Кора";
    case BotState::Fight:
        return "Бой";
    case BotState::Retreat:
        return "Отход";
    }

    return "Неизвестно";
}

const char* ToString(BotRole role)
{
    switch (role)
    {
    case BotRole::Defender:
        return "Защита";
    case BotRole::Rusher:
        return "Раш";
    case BotRole::Collector:
        return "Сбор";
    case BotRole::Fighter:
        return "Бой";
    }

    return "Неизвестно";
}

const char* ToString(BotArchetype archetype)
{
    switch (archetype)
    {
    case BotArchetype::CautiousDefender: return "cautious defender";
    case BotArchetype::AggressiveRusher: return "aggressive rusher";
    case BotArchetype::FrugalBuilder: return "frugal builder";
    case BotArchetype::IsolationHunter: return "isolation hunter";
    case BotArchetype::TeamHelper: return "team helper";
    case BotArchetype::ImpulsiveDuelist: return "impulsive duelist";
    case BotArchetype::Engineer: return "engineer";
    case BotArchetype::Opportunist: return "opportunist";
    }
    return "unknown";
}

const char* ToString(BotIntent intent)
{
    switch (intent)
    {
    case BotIntent::DefendCore:
        return "Защита";
    case BotIntent::RepairCoreDefense:
        return "Ремонт";
    case BotIntent::GearUp:
        return "Закуп";
    case BotIntent::SecureResources:
        return "Ресурсы";
    case BotIntent::PressureCore:
        return "Давление";
    case BotIntent::BreakCoreDefense:
        return "Взлом";
    case BotIntent::FightEnemy:
        return "Бой";
    case BotIntent::ChaseWeakEnemy:
        return "Погоня";
    case BotIntent::RetreatHome:
        return "Отход";
    case BotIntent::Recover:
        return "Восст.";
    }

    return "Неизвестно";
}

const char* ToString(CoordinationSignal signal)
{
    switch (signal)
    {
    case CoordinationSignal::None:
        return "Нет";
    case CoordinationSignal::AttackingCore:
        return "Атака Кора";
    case CoordinationSignal::DefendingCore:
        return "Защита Кора";
    case CoordinationSignal::BuildingBridge:
        return "Строит мост";
    case CoordinationSignal::Retreating:
        return "Отходит";
    case CoordinationSignal::CallingForHelp:
        return "Зовет помощь";
    case CoordinationSignal::HoldingMid:
        return "Держит центр";
    }

    return "Неизвестно";
}

const char* ToString(StrategicGoal goal)
{
    switch (goal)
    {
    case StrategicGoal::Idle:
        return "Ожидание";
    case StrategicGoal::EconomicPhase:
        return "Экономика";
    case StrategicGoal::BridgePush:
        return "Пуш мостом";
    case StrategicGoal::CoreAssault:
        return "Штурм Кора";
    case StrategicGoal::BaseDefense:
        return "Защита базы";
    case StrategicGoal::HuntPlayers:
        return "Охота";
    case StrategicGoal::MidControl:
        return "Контроль центра";
    }

    return "Неизвестно";
}
