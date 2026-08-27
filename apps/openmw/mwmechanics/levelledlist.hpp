#ifndef OPENMW_MECHANICS_LEVELLEDLIST_H
#define OPENMW_MECHANICS_LEVELLEDLIST_H

#include <components/misc/rng.hpp>

#include <cstdint>
#include <optional>

namespace ESM
{
    struct LevelledListBase;
    class RefId;
}

namespace MWMechanics
{

    // mp: splitmix64's finalizer. Spawn points that sit next to each other carry
    // adjacent RefNum indices, so a weak mix (a raw XOR, or std::hash, which is
    // the identity function on 64-bit integers in libstdc++) would put visibly
    // patterned creatures beside one another. Written out here rather than
    // pulled from a library so its bytes are fixed by this header on every
    // platform - two machines must agree exactly, not approximately.
    constexpr uint64_t mixSeed(uint64_t z)
    {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // mp: the per-actor seed for resolving that actor's levelled INVENTORY.
    //
    // Derived exactly like the levelled-CREATURE seed in creaturelevlist.cpp -
    // campaign seed, the RefNum's two halves mixed separately, then the respawn
    // epoch - because it answers the same question: what would every machine in
    // this session agree on for this particular object?
    //
    // The domain separator is not decoration. Without it an actor's inventory
    // draw would begin from the identical value its own creature draw used, so
    // the two would correlate - the same list positions coming up together
    // across the whole world, which reads as a pattern rather than a roll.
    constexpr uint64_t sDomainInventory = 0x9E3779B97F4A7C15ULL;

    constexpr uint64_t actorInventorySeed(
        uint64_t campaignSeed, int contentFile, uint32_t index, uint32_t epoch)
    {
        uint64_t s = mixSeed(campaignSeed ^ sDomainInventory);
        s = mixSeed(s ^ static_cast<uint64_t>(static_cast<uint32_t>(contentFile)));
        s = mixSeed(s ^ static_cast<uint64_t>(index));
        s = mixSeed(s ^ static_cast<uint64_t>(epoch));
        return s;
    }

    /// @return ID of resulting item, or empty if none
    // mp: `seed`, when set, replaces `prng` with a deterministic draw stream so
    // that two machines resolve the same list to the same record. It is nullopt
    // at every vanilla call site, which is what keeps single-player identical to
    // upstream.
    ESM::RefId getLevelledItem(const ESM::LevelledListBase* levItem, bool creature, Misc::Rng::Generator& prng,
        std::optional<int> level = {}, std::optional<uint64_t> seed = {});

}

#endif
