#ifndef GAME_LUA_VOIPBINDINGS_H
#define GAME_LUA_VOIPBINDINGS_H

#include <sol/forward.hpp>

namespace MWLua
{
    struct Context;

    // openmw.voip for player scripts: the microphone half of voice - loopback,
    // push to talk, device selection and the latency figures the drill reads.
    // Player-scoped rather than global because everything in it is a fact about
    // this machine's hardware, not about the session.
    sol::table initVoipPlayerPackage(const Context& context);
}

#endif // GAME_LUA_VOIPBINDINGS_H
