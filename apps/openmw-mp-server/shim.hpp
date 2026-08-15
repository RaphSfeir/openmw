#ifndef OPENMW_MP_SERVER_SHIM_H
#define OPENMW_MP_SERVER_SHIM_H

#include <sol/sol.hpp>

namespace Net
{
    class Session;
}

namespace MPServer
{
    struct Config;

    // Puts the openmw.* packages the mod requires into package.loaded, so its
    // modules load here exactly as they load in the engine.
    //
    // Two of them are real — openmw.network over the same ENet wrapper the
    // client uses, and openmw.campaign over the same file format — and the rest
    // are the smallest honest stubs: a server has no world, and the arbitration
    // was rewritten not to need one. A stub that silently invented data would
    // hide exactly the dependencies M8.0 removed, so they return nothing rather
    // than something plausible.
    void installShim(sol::state& lua, Net::Session& session, const Config& config);
}

#endif // OPENMW_MP_SERVER_SHIM_H
