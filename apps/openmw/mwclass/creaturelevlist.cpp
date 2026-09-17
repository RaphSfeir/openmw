#include "creaturelevlist.hpp"

#include <cstdint>
#include <optional>

#include <components/esm3/actoridconverter.hpp>
#include <components/esm3/creaturelevliststate.hpp>
#include <components/esm3/loadlevlist.hpp>

#include "../mwmechanics/levelledlist.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/customdata.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/worldmodel.hpp"

#include "../mwmechanics/creaturestats.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

namespace MWClass
{
    class CreatureLevListCustomData : public MWWorld::TypedCustomData<CreatureLevListCustomData>
    {
    public:
        ESM::RefNum mSpawnedActor;
        bool mSpawn = true; // Should a new creature be spawned?

        MWWorld::Ptr getSpawnedPtr() const
        {
            if (mSpawnedActor.isSet())
                return MWBase::Environment::get().getWorldModel()->getPtr(mSpawnedActor);
            return {};
        }

        CreatureLevListCustomData& asCreatureLevListCustomData() override { return *this; }
        const CreatureLevListCustomData& asCreatureLevListCustomData() const override { return *this; }
    };

    CreatureLevList::CreatureLevList()
        : MWWorld::RegisteredClass<CreatureLevList>(ESM::CreatureLevList::sRecordId)
    {
    }

    MWWorld::Ptr CreatureLevList::copyToCellImpl(const MWWorld::ConstPtr& ptr, MWWorld::CellStore& cell) const
    {
        const MWWorld::LiveCellRef<ESM::CreatureLevList>* ref = ptr.get<ESM::CreatureLevList>();

        return MWWorld::Ptr(cell.insert(ref), &cell);
    }

    void CreatureLevList::adjustPosition(const MWWorld::Ptr& ptr, bool force) const
    {
        if (ptr.getRefData().getCustomData() == nullptr)
            return;

        CreatureLevListCustomData& customData = ptr.getRefData().getCustomData()->asCreatureLevListCustomData();
        MWWorld::Ptr creature = customData.getSpawnedPtr();
        if (!creature.isEmpty())
            MWBase::Environment::get().getWorld()->adjustPosition(creature, force);
    }

    std::string_view CreatureLevList::getName(const MWWorld::ConstPtr& ptr) const
    {
        return {};
    }

    bool CreatureLevList::hasToolTip(const MWWorld::ConstPtr& ptr) const
    {
        return false;
    }

    void CreatureLevList::respawn(const MWWorld::Ptr& ptr) const
    {
        ensureCustomData(ptr);

        CreatureLevListCustomData& customData = ptr.getRefData().getCustomData()->asCreatureLevListCustomData();
        if (customData.mSpawn)
            return;

        MWWorld::Ptr creature = customData.getSpawnedPtr();
        if (!creature.isEmpty())
        {
            const MWMechanics::CreatureStats& creatureStats = creature.getClass().getCreatureStats(creature);
            if (creature.getCellRef().getCount() == 0)
                customData.mSpawn = true;
            else if (creatureStats.isDead())
            {
                const MWWorld::Store<ESM::GameSetting>& gmst
                    = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>();
                static const float fCorpseRespawnDelay = gmst.find("fCorpseRespawnDelay")->mValue.getFloat();
                static const float fCorpseClearDelay = gmst.find("fCorpseClearDelay")->mValue.getFloat();

                float delay = std::min(fCorpseRespawnDelay, fCorpseClearDelay);
                if (creatureStats.getTimeOfDeath() + delay <= MWBase::Environment::get().getWorld()->getTimeStamp())
                    customData.mSpawn = true;
            }
        }
        else
            customData.mSpawn = true;
    }

    void CreatureLevList::insertObjectRendering(
        const MWWorld::Ptr& ptr, const std::string& model, MWRender::RenderingInterface& renderingInterface) const
    {
        ensureCustomData(ptr);

        CreatureLevListCustomData& customData = ptr.getRefData().getCustomData()->asCreatureLevListCustomData();
        if (!customData.mSpawn)
            return;

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        MWBase::World* world = MWBase::Environment::get().getWorld();

        // mp: ALREADY CLEARED BY THE PARTY, so do not roll it at all.
        //
        // HERE, and not anywhere later, because this is the only point at which
        // a spawn can be PREVENTED rather than undone: two lines down the
        // creature exists, and deleting it afterwards is a creature that
        // flickered into the cave on somebody's screen. It is also the only
        // check that works before a cell has ever loaded, which is what a
        // returning player needs.
        //
        // The session tells us which spawners are cleared (by content RefNum,
        // the only stable half of the pair -- the creature it spawned is a
        // runtime object). mSpawn is left false, so this machine treats the
        // point as already rolled and CellStore::respawn's own clock governs
        // when it may come back, exactly as it would have.
        const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
        if (refNum.hasContentFile() && world->isLevelledSpawnCleared(refNum))
        {
            customData.mSpawn = false;
            return;
        }

        // mp: THE levelled-spawn desync. Vanilla rolls this spawn point off the
        // shared world PRNG, whose stream POSITION diverges between two machines
        // within seconds of loading the same world (different cell-load order,
        // different AI ticks, different combat), so the same spawner materialises
        // a rat here and a bonelord there. With a big list that is hundreds of
        // spawn points and two players who simply do not see the same world.
        // A session hands us a campaign seed instead, and we roll from a stream
        // keyed to THIS spawn point's content RefNum - a pure function of the
        // plugin bytes and the content= order, and therefore identical on every
        // machine the campaign will admit.
        // mIndex and mContentFile are mixed SEPARATELY, never through
        // RefNum::toUint32(): that throws once mContentFile > 0xFE, and a big
        // modlist is well past 255 plugins, so it would raise here on the
        // RENDER path for every ref past the 255th.
        // A runtime-placed list has no content RefNum (its number is a
        // machine-local counter), so it stays on the vanilla path and on the
        // Lua-side arbitration that still backs all of this up.
        std::optional<uint64_t> spawnSeed;
        std::optional<int> spawnLevel;
        // refNum is declared above, at the cleared-spawn check.
        if (const auto& rule = world->getLevelledSpawnRule(); rule && rule->mLevel > 0 && refNum.hasContentFile())
        {
            uint64_t s = MWMechanics::mixSeed(rule->mSeed);
            s = MWMechanics::mixSeed(s ^ static_cast<uint64_t>(static_cast<uint32_t>(refNum.mContentFile)));
            s = MWMechanics::mixSeed(s ^ static_cast<uint64_t>(refNum.mIndex));
            // The respawn generation, so a cleared cave does not repopulate with
            // the identical lineup forever. It comes off the session's shared
            // calendar; local game time and mLastRespawn are per-client and
            // would put the divergence straight back.
            s = MWMechanics::mixSeed(s ^ static_cast<uint64_t>(rule->mEpoch));
            spawnSeed = s;
            // The PARTY level, not this machine's. getLevelledItem filters the
            // CANDIDATE SET by player level, so two players at different levels
            // pick different creatures from an identical seed - seeding alone
            // looks correct in a test where both characters are level 1 and
            // fails the moment they diverge.
            spawnLevel = rule->mLevel;
        }
        auto& prng = world->getPrng();
        const ESM::RefId& id = MWMechanics::getLevelledItem(
            store.get<ESM::CreatureLevList>().find(ptr.getCellRef().getRefId()), true, prng, spawnLevel, spawnSeed);

        if (!id.empty())
        {
            // Delete the previous creature
            MWWorld::Ptr previous = customData.getSpawnedPtr();
            if (!previous.isEmpty())
                MWBase::Environment::get().getWorld()->deleteObject(previous);

            MWWorld::ManualRef manualRef(store, id);
            manualRef.getPtr().getCellRef().setPosition(ptr.getCellRef().getPosition());
            manualRef.getPtr().getCellRef().setScale(ptr.getCellRef().getScale());
            MWWorld::Ptr placed = MWBase::Environment::get().getWorld()->placeObject(
                manualRef.getPtr(), ptr.getCell(), ptr.getRefData().getPosition());
            MWBase::Environment::get().getWorldModel()->registerPtr(placed);
            customData.mSpawnedActor = placed.getCellRef().getRefNum();
            customData.mSpawn = false;
        }
        else
            customData.mSpawn = false;
    }

    void CreatureLevList::ensureCustomData(const MWWorld::Ptr& ptr) const
    {
        if (!ptr.getRefData().getCustomData())
        {
            ptr.getRefData().setCustomData(std::make_unique<CreatureLevListCustomData>());
        }
    }

    void CreatureLevList::readAdditionalState(const MWWorld::Ptr& ptr, const ESM::ObjectState& state) const
    {
        if (!state.mHasCustomState)
            return;

        ensureCustomData(ptr);
        CreatureLevListCustomData& customData = ptr.getRefData().getCustomData()->asCreatureLevListCustomData();
        const ESM::CreatureLevListState& levListState = state.asCreatureLevListState();
        customData.mSpawnedActor = levListState.mSpawnedActor;
        customData.mSpawn = levListState.mSpawn;
        if (state.mActorIdConverter)
            state.mActorIdConverter->convert(customData.mSpawnedActor, customData.mSpawnedActor.mIndex);
    }

    void CreatureLevList::writeAdditionalState(const MWWorld::ConstPtr& ptr, ESM::ObjectState& state) const
    {
        if (!ptr.getRefData().getCustomData())
        {
            state.mHasCustomState = false;
            return;
        }

        const CreatureLevListCustomData& customData = ptr.getRefData().getCustomData()->asCreatureLevListCustomData();
        ESM::CreatureLevListState& levListState = state.asCreatureLevListState();
        levListState.mSpawnedActor = customData.mSpawnedActor;
        levListState.mSpawn = customData.mSpawn;
    }
}
