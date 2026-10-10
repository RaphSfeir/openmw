#ifndef OPENMW_MWMECHANICS_DIFFICULTYSCALING_H
#define OPENMW_MWMECHANICS_DIFFICULTYSCALING_H

#include <optional>

namespace MWWorld
{
    class Ptr;
}

/// Scales damage dealt to an actor based on difficulty setting
float scaleDamage(float damage, const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim);

/// mp: the difficulty in force, which is the session's override when one is set
/// and the user's [Game] difficulty setting otherwise. Everything that scales
/// damage reads this, so a session rule and the options menu cannot disagree.
int getEffectiveDifficulty();

/// mp: order a difficulty for this process (a multiplayer session rule), or lift
/// it with std::nullopt. Never written to the user's settings: the rule is the
/// session's, and the player's own value is back the moment it is lifted.
void setDifficultyOverride(std::optional<int> value);

#endif
