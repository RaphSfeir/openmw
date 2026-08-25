#include <components/debug/debuglog.hpp>
#include <components/esm3/loadlevlist.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/ptr.hpp"

#include "../mwbase/environment.hpp"

#include "actorutil.hpp"
#include "creaturestats.hpp"
#include "levelledlist.hpp"

namespace MWMechanics
{

    ESM::RefId getLevelledItem(const ESM::LevelledListBase* levItem, bool creature, Misc::Rng::Generator& prng,
        std::optional<int> level, std::optional<uint64_t> seed)
    {
        const std::vector<ESM::LevelledListBase::LevelItem>& items = levItem->mList;

        // mp: the deterministic draw stream. Handing both machines the same
        // seeded Misc::Rng::Generator is NOT enough to make them agree: both
        // draws below go through Misc::Rng::rollDice, which is
        // std::uniform_int_distribution, and the standard leaves that
        // algorithm unspecified - libstdc++ and the MSVC STL return DIFFERENT
        // indices from identical generator state. That would desync a Linux
        // player against a Windows player in exactly the way this whole change
        // exists to prevent, and only ever in production, never in a
        // same-platform test. So the deterministic path skips the distribution
        // and reduces a splitmix64 counter itself; every byte of the result is
        // fixed by this file.
        uint64_t detState = seed.value_or(0);
        const bool deterministic = seed.has_value();
        const auto detRoll = [&detState](uint64_t bound) -> uint64_t {
            detState += 0x9E3779B97F4A7C15ULL;
            // The modulo bias over a 64-bit draw with a bound this small is
            // about 2^-57. Portability is worth more than that here.
            return bound == 0 ? 0 : mixSeed(detState) % bound;
        };

        int playerLevel;
        if (level.has_value())
            playerLevel = *level;
        else
        {
            const MWWorld::Ptr& player = getPlayer();
            playerLevel = player.getClass().getCreatureStats(player).getLevel();
            level = playerLevel;
        }

        const int noneRoll = deterministic ? static_cast<int>(detRoll(100)) : Misc::Rng::roll0to99(prng);
        if (noneRoll < levItem->mChanceNone)
            return ESM::RefId();

        std::vector<const ESM::RefId*> candidates;
        int highestLevel = 0;
        for (const auto& levelledItem : items)
        {
            if (levelledItem.mLevel > highestLevel && levelledItem.mLevel <= playerLevel)
                highestLevel = levelledItem.mLevel;
        }

        // For levelled creatures, the flags are swapped. This file format just makes so much sense.
        bool allLevels = (levItem->mFlags & ESM::ItemLevList::AllLevels) != 0;
        if (creature)
            allLevels = levItem->mFlags & ESM::CreatureLevList::AllLevels;

        for (const auto& levelledItem : items)
        {
            if (playerLevel >= levelledItem.mLevel && (allLevels || levelledItem.mLevel == highestLevel))
                candidates.push_back(&levelledItem.mId);
        }
        if (candidates.empty())
            return ESM::RefId();
        const size_t pick = deterministic ? static_cast<size_t>(detRoll(candidates.size()))
                                          : Misc::Rng::rollDice(candidates.size(), prng);
        const ESM::RefId& item = *candidates[pick];

        // Vanilla doesn't fail on nonexistent items in levelled lists
        if (!MWBase::Environment::get().getESMStore()->find(item))
        {
            Log(Debug::Warning) << "Warning: ignoring nonexistent item " << item << " in levelled list "
                                << levItem->mId;
            return ESM::RefId();
        }

        // Is this another levelled item or a real item?
        MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), item, 1);
        if (ref.getPtr().getType() != ESM::ItemLevList::sRecordId
            && ref.getPtr().getType() != ESM::CreatureLevList::sRecordId)
        {
            return item;
        }
        else
        {
            // mp: the child inherits the counter as this level's draws left it,
            // so a nested list draws fresh numbers while staying a pure
            // function of the seed. Both machines walk the same tree, so the
            // counter stands at the same value at every node.
            std::optional<uint64_t> childSeed;
            if (deterministic)
                childSeed = detState;
            if (ref.getPtr().getType() == ESM::ItemLevList::sRecordId)
                return getLevelledItem(ref.getPtr().get<ESM::ItemLevList>()->mBase, false, prng, level, childSeed);
            else
                return getLevelledItem(ref.getPtr().get<ESM::CreatureLevList>()->mBase, true, prng, level, childSeed);
        }
    }
}
