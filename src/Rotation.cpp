/*
 * mod-rotation — cœur du module : configuration, interception du sort
 * déclencheur, helpers communs et aiguillage classe/spécialisation.
 */

#include "Rotation.h"

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "CreatureAI.h"
#include "Duration.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

namespace ModRotation
{
Settings config;

uint32 MaxRank(Player* p, uint32 firstRankId)
{
    uint32 best = 0;
    for (SpellInfo const* info = sSpellMgr->GetSpellInfo(firstRankId); info; info = info->GetNextRankSpell())
        if (p->HasSpell(info->Id))
            best = info->Id;

    return best;
}

static bool CanPayCost(Player* p, SpellInfo const* info)
{
    if (info->PowerType != POWER_MANA && info->PowerType != POWER_RAGE && info->PowerType != POWER_ENERGY)
        return true; // runes, etc. : vérifié par le cœur au moment du cast

    int32 cost = info->CalcPowerCost(p, info->GetSchoolMask());
    return int32(p->GetPower(Powers(info->PowerType))) >= cost;
}

static bool ReadyToCast(Player* p, SpellInfo const* info)
{
    if (!info)
        return false;

    if (p->HasSpellCooldown(info->Id))
        return false;

    if (p->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        return false;

    // Posture/forme requise non satisfaite (ex. Pourfendre en Posture
    // berserker) : on ignore silencieusement au lieu de spammer l'erreur.
    if (info->CheckShapeshift(p->GetShapeshiftForm()) != SPELL_CAST_OK)
        return false;

    return CanPayCost(p, info);
}

bool TryCast(Player* p, Unit* target, uint32 spellId)
{
    if (!spellId || !target)
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!ReadyToCast(p, info))
        return false;

    return p->CastSpell(target, spellId, false) == SPELL_CAST_OK;
}

bool TryCastRank(Player* p, Unit* target, uint32 firstRankId)
{
    return TryCast(p, target, MaxRank(p, firstRankId));
}

bool TryCastAt(Player* p, Unit* where, uint32 firstRankId)
{
    uint32 id = MaxRank(p, firstRankId);
    if (!id || !where)
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    if (!ReadyToCast(p, info))
        return false;

    return p->CastSpell(where->GetPositionX(), where->GetPositionY(), where->GetPositionZ(), id, false) == SPELL_CAST_OK;
}

bool KeepAuraOn(Player* p, Unit* target, uint32 firstRankId)
{
    if (!target || target->GetAuraOfRankedSpell(firstRankId, p->GetGUID()))
        return false;

    return TryCastRank(p, target, firstRankId);
}

bool KeepSelfBuff(Player* p, uint32 firstRankId)
{
    if (p->GetAuraOfRankedSpell(firstRankId))
        return false;

    return TryCastRank(p, p, firstRankId);
}

bool IsBleedImmune(Unit* target)
{
    uint32 type = target->GetCreatureType();
    return type == CREATURE_TYPE_MECHANICAL || type == CREATURE_TYPE_ELEMENTAL;
}

void PetAttack(Ctx& c)
{
    if (!c.target)
        return;

    if (Pet* pet = c.me->GetPet())
        if (pet->IsAlive() && !pet->GetVictim() && pet->AI())
            pet->AI()->AttackStart(c.target);
}

void EngageMelee(Ctx& c)
{
    if (c.target && c.me->GetVictim() != c.target)
        c.me->Attack(c.target, true);

    PetAttack(c);
}

void EngageRanged(Ctx& c)
{
    if (!c.target)
        return;

    PetAttack(c);

    // Tir automatique (75) pour les chasseurs
    if (c.me->getClass() == CLASS_HUNTER && !c.me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        c.me->CastSpell(c.target, 75, false);
}

// Compte les ennemis en combat autour de la cible. C'est ce qui décide de
// l'AoE pour toutes les spécialisations.
//
// Se baser sur getAttackers() (« qui me frappe ») serait faux : un distant
// n'a personne à sa portée de mêlée, et en groupe seul le tank est attaqué —
// l'AoE ne se déclenchait donc jamais pour les autres. Voir l'issue #2.
static uint32 CountEnemiesAround(Player* p, Unit* center, float radius)
{
    typedef Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> Searcher;

    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, p, radius);
    Searcher searcher(center, units, check);

    // Cell::VisitObjects ne parcourt que le conteneur de grille (créatures) :
    // on ajoute le conteneur monde pour compter aussi les joueurs hostiles.
    CellCoord coord(Acore::ComputeCellCoord(center->GetPositionX(), center->GetPositionY()));
    Cell cell(coord);
    TypeContainerVisitor<Searcher, GridTypeMapContainer>  gridVisitor(searcher);
    TypeContainerVisitor<Searcher, WorldTypeMapContainer> worldVisitor(searcher);
    cell.Visit(coord, gridVisitor, *center->GetMap(), *center, radius);
    cell.Visit(coord, worldVisitor, *center->GetMap(), *center, radius);

    uint32 count = 0;
    for (Unit* u : units)
    {
        // Hors combat : on ignore, pour ne pas déclencher une AoE au sol qui
        // pullerait un groupe voisin resté paisible.
        if (u != center && !u->IsInCombat())
            continue;

        if (p->IsValidAttackTarget(u))
            ++count;
    }

    return count;
}

static void ExecuteRotation(Player* p)
{
    if (!p->IsAlive())
        return;

    // Ne pas couper une incantation ou une canalisation en cours
    // (le Tir automatique n'est pas bloquant).
    if (p->IsNonMeleeSpellCast(false, false, true))
        return;

    Ctx c;
    c.me = p;

    Unit* sel = p->GetSelectedUnit();
    if (sel && sel->IsAlive())
    {
        if (p->IsValidAttackTarget(sel))
            c.target = sel;
        else if (sel != p && p->IsValidAssistTarget(sel))
            c.friendly = sel;
    }

    if (!c.friendly)
        c.friendly = p;

    if (c.target)
    {
        c.inMelee = p->IsWithinMeleeRange(c.target);
        c.enemies = CountEnemiesAround(p, c.target, config.AoeRadius);
    }

    c.tree = p->GetMostPointsTalentTree();

    switch (p->getClass())
    {
        case CLASS_WARRIOR:      Warrior(c);     break;
        case CLASS_PALADIN:      Paladin(c);     break;
        case CLASS_HUNTER:       Hunter(c);      break;
        case CLASS_ROGUE:        Rogue(c);       break;
        case CLASS_PRIEST:       Priest(c);      break;
        case CLASS_DEATH_KNIGHT: DeathKnight(c); break;
        case CLASS_SHAMAN:       Shaman(c);      break;
        case CLASS_MAGE:         Mage(c);        break;
        case CLASS_WARLOCK:      Warlock(c);     break;
        case CLASS_DRUID:        Druid(c);       break;
        default: break;
    }
}
} // namespace ModRotation

using namespace ModRotation;

class rotation_world_script : public WorldScript
{
public:
    rotation_world_script() : WorldScript("rotation_world_script") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.Enabled             = sConfigMgr->GetOption<bool>("Rotation.Enable", true);
        config.SpellId             = sConfigMgr->GetOption<uint32>("Rotation.SpellId", 47322);
        config.AutoLearn           = sConfigMgr->GetOption<bool>("Rotation.AutoLearn", true);
        config.Announce            = sConfigMgr->GetOption<bool>("Rotation.Announce", false);
        config.AnnounceMessage     = sConfigMgr->GetOption<std::string>("Rotation.Announce.Message",
            "One-button |cff4CFF00Rotation|r module active: place the Rotation spell from your spellbook on your action bar.");
        config.AoeThreshold        = sConfigMgr->GetOption<uint32>("Rotation.AoE.Threshold", 2);
        config.AoeRadius           = sConfigMgr->GetOption<float>("Rotation.AoE.Radius", 10.0f);
        config.RageDumpThreshold   = sConfigMgr->GetOption<uint32>("Rotation.RageDump.Threshold", 50);
        config.EnergyDumpThreshold = sConfigMgr->GetOption<uint32>("Rotation.EnergyDump.Threshold", 60);
        config.HealInjuredPct      = sConfigMgr->GetOption<uint32>("Rotation.Heal.InjuredPct", 85);
        config.HealLowPct          = sConfigMgr->GetOption<uint32>("Rotation.Heal.LowPct", 60);
        config.HealUrgentPct       = sConfigMgr->GetOption<uint32>("Rotation.Heal.UrgentPct", 35);
    }
};

class rotation_player_script : public PlayerScript
{
public:
    rotation_player_script() : PlayerScript("rotation_player_script") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!config.Enabled)
            return;

        if (config.AutoLearn && !player->HasSpell(config.SpellId))
            player->learnSpell(config.SpellId);

        if (config.Announce && !config.AnnounceMessage.empty())
            ChatHandler(player->GetSession()).SendSysMessage(config.AnnounceMessage.c_str());
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!config.Enabled || !spell || spell->m_spellInfo->Id != config.SpellId)
            return;

        // Lancer un autre sort pendant la préparation du sort déclencheur n'est
        // pas sûr : on diffère la rotation d'un tick de mise à jour.
        ObjectGuid guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([guid]()
        {
            if (Player* p = ObjectAccessor::FindPlayer(guid))
                ExecuteRotation(p);
        }, Milliseconds(1));
    }
};

void AddRotationScripts()
{
    new rotation_world_script();
    new rotation_player_script();
}
