#include "difficultyscaling.hpp"

#include <atomic>
#include <limits>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/ptr.hpp"

#include "actorutil.hpp"

namespace
{
    // mp: INT_MIN means "no override". Atomic because the order comes from a
    // global Lua script, which may run on the Lua worker thread, while the
    // reader below runs on the main thread inside the mechanics pass.
    constexpr int sNoOverride = std::numeric_limits<int>::min();
    std::atomic<int> sDifficultyOverride{ sNoOverride };
}

int getEffectiveDifficulty()
{
    const int ruled = sDifficultyOverride.load(std::memory_order_relaxed);
    if (ruled != sNoOverride)
        return ruled;
    return Settings::game().mDifficulty.get();
}

void setDifficultyOverride(std::optional<int> value)
{
    sDifficultyOverride.store(value.value_or(sNoOverride), std::memory_order_relaxed);
}

float scaleDamage(float damage, const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim)
{
    const MWWorld::Ptr& player = MWMechanics::getPlayer();

    static const float fDifficultyMult
        = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>().find("fDifficultyMult")->mValue.getFloat();

    const float difficultyTerm = 0.01f * getEffectiveDifficulty();

    float x = 0;
    if (victim == player)
    {
        if (difficultyTerm > 0)
            x = fDifficultyMult * difficultyTerm;
        else
            x = difficultyTerm / fDifficultyMult;
    }
    else if (attacker == player)
    {
        if (difficultyTerm > 0)
            x = -difficultyTerm / fDifficultyMult;
        else
            x = fDifficultyMult * (-difficultyTerm);
    }

    damage *= 1 + x;
    return damage;
}
