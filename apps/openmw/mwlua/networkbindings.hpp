#ifndef MWLUA_NETWORKBINDINGS_H
#define MWLUA_NETWORKBINDINGS_H

#include <sol/forward.hpp>

namespace MWLua
{
    struct Context;

    // Full openmw.network package for global scripts: session control, send/broadcast/receive.
    sol::table initNetworkPackage(const Context& context);

    // Control-only variant for menu scripts: host/connect/disconnect/state, no message access
    // (messages are consumed by the global scripts' receive loop).
    sol::table initNetworkControlPackage(const Context& context);
}

#endif // MWLUA_NETWORKBINDINGS_H
