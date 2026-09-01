#include "mapbindings.hpp"

#include <components/lua/luastate.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include "luamanagerimp.hpp"

namespace MWLua
{
    // openmw.map -- the fork's map surface. One function, deliberately: a
    // multiplayer session wants to say "these people are HERE" and nothing
    // else. The engine draws them on the HUD minimap, the local map and (for
    // markers carrying an exterior position) the world map, refreshed on the
    // detection-marker cadence.
    //
    // setMarkers(list) replaces the whole set. Each entry:
    //   x, y        world units on the local map
    //   worldspace  serialized worldspace id the position belongs to
    //   label       the tooltip -- who this is
    //   global      optional { x, y }: last known EXTERIOR position, for the
    //               world map. Absent = indoors somewhere, world map skips it.
    sol::table initMapPackage(const Context& context)
    {
        sol::state_view lua = context.sol();
        sol::table api(lua, sol::create);
        api["setMarkers"] = [context](const sol::table& list) {
            std::vector<MWBase::WindowManager::LiveMapMarker> markers;
            const size_t n = list.size();
            markers.reserve(n);
            for (size_t i = 1; i <= n; ++i)
            {
                sol::optional<sol::table> entry = list[i];
                if (!entry)
                    continue;
                MWBase::WindowManager::LiveMapMarker m;
                m.mWorldX = entry->get_or("x", 0.f);
                m.mWorldY = entry->get_or("y", 0.f);
                m.mWorldspace = entry->get_or<std::string>("worldspace", "");
                m.mLabel = entry->get_or<std::string>("label", "");
                sol::optional<sol::table> global = (*entry)["global"];
                if (global)
                {
                    m.mGlobal = true;
                    m.mGlobalX = global->get_or("x", 0.f);
                    m.mGlobalY = global->get_or("y", 0.f);
                }
                markers.push_back(std::move(m));
            }
            // Queued like every other engine mutation: the GUI is main-thread
            // property and Lua may be running off it.
            context.mLuaManager->addAction(
                [markers = std::move(markers)] {
                    MWBase::Environment::get().getWindowManager()->setLiveMapMarkers(markers);
                },
                "setLiveMapMarkers");
        };
        return LuaUtil::makeReadOnly(api);
    }
}
