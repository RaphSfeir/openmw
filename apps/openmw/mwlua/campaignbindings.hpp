#ifndef MWLUA_CAMPAIGNBINDINGS_H
#define MWLUA_CAMPAIGNBINDINGS_H

#include <sol/forward.hpp>

namespace MWLua
{
    struct Context;

    // openmw.campaign, global scripts only: a small persistent file store for the
    // multiplayer campaign registry (one campaign = one file under
    // <userConfigPath>/mp-campaigns/). This is the deployed server's database API,
    // temporarily living on the host machine: when a dedicated server exists it
    // implements the same surface on its own disk and the Lua side is unchanged.
    sol::table initCampaignPackage(const Context& context);
}

#endif // MWLUA_CAMPAIGNBINDINGS_H
