#ifndef MWLUA_MAPBINDINGS_H
#define MWLUA_MAPBINDINGS_H

#include <sol/forward.hpp>

#include "context.hpp"

namespace MWLua
{
    sol::table initMapPackage(const Context& context);
}

#endif // MWLUA_MAPBINDINGS_H
