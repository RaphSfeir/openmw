#ifndef MWLUA_GLOBALSCRIPTS_H
#define MWLUA_GLOBALSCRIPTS_H

#include <components/lua/luastate.hpp>
#include <components/lua/scriptscontainer.hpp>
#include <components/lua/utilpackage.hpp>

#include <osg/Vec3f>

#include "object.hpp"

namespace MWLua
{

    class GlobalScripts : public LuaUtil::ScriptsContainer
    {
    public:
        GlobalScripts(LuaUtil::LuaState* lua)
            : LuaUtil::ScriptsContainer(lua, "Global")
        {
            registerEngineHandlers({ &mObjectActiveHandlers, &mActorActiveHandlers, &mItemActiveHandlers,
                &mNewGameHandlers, &mPlayerAddedHandlers, &mOnActivateHandlers, &mOnUseItemHandlers,
                &mOnNewExteriorHandlers, &mOnDroppedHandlers, &mOnPlacedHandlers, &mOnProjectileHit,
                &mOnObjectStateRequestHandlers, &mOnObjectTransformHandlers, &mOnContainerScriptWriteHandlers });
        }

        void newGameStarted() { callEngineHandlers(mNewGameHandlers); }
        void objectActive(const GObject& obj) { callEngineHandlers(mObjectActiveHandlers, obj); }
        void actorActive(const GObject& obj) { callEngineHandlers(mActorActiveHandlers, obj); }
        void itemActive(const GObject& obj) { callEngineHandlers(mItemActiveHandlers, obj); }
        void playerAdded(const GObject& obj) { callEngineHandlers(mPlayerAddedHandlers, obj); }
        void onActivate(const GObject& obj, const GObject& actor)
        {
            callEngineHandlers(mOnActivateHandlers, obj, actor);
        }
        void onPlaced(
            const GObject& obj, const GObject& actor, const osg::Vec3f& position, const LuaUtil::TransformQ& rotation)
        {
            callEngineHandlers(mOnPlacedHandlers, obj, actor, position, rotation);
        }
        void onDropped(
            const GObject& obj, const GObject& actor, const osg::Vec3f& position, const LuaUtil::TransformQ& rotation)
        {
            callEngineHandlers(mOnDroppedHandlers, obj, actor, position, rotation);
        }
        void onUseItem(const GObject& obj, const GObject& actor, bool force)
        {
            callEngineHandlers(mOnUseItemHandlers, obj, actor, force);
        }
        // MP: an mwscript asked to enable/disable this object and the opcode
        // did NOT apply it, because a session is active. Whoever handles this
        // owns the decision and must do the applying.
        void onObjectStateRequest(const GObject& obj, bool enable)
        {
            callEngineHandlers(mOnObjectStateRequestHandlers, obj, enable);
        }
        // MP: a script moved/rotated/rescaled this object and the engine HAS
        // applied it; the handler's job is to tell everyone else.
        void onObjectTransform(const GObject& obj) { callEngineHandlers(mOnObjectTransformHandlers, obj); }
        // MP: a script wrote items into (or out of) this world container and the
        // engine HAS applied it, on this machine only. The handler's job is to
        // tell the session, which owns what every machine ends up holding.
        void onContainerScriptWrite(
            const GObject& obj, const std::string& script, const std::string& item, int delta, bool levelled)
        {
            callEngineHandlers(mOnContainerScriptWriteHandlers, obj, script, item, delta, levelled);
        }
        void onNewExterior(const GCell& cell) { callEngineHandlers(mOnNewExteriorHandlers, cell); }
        void onProjectileHit(const sol::table& projectile, const sol::table& hitResult)
        {
            callEngineHandlers(mOnProjectileHit, projectile, hitResult);
        }

    private:
        EngineHandlerList mObjectActiveHandlers{ "onObjectActive" };
        EngineHandlerList mActorActiveHandlers{ "onActorActive" };
        EngineHandlerList mItemActiveHandlers{ "onItemActive" };
        EngineHandlerList mNewGameHandlers{ "onNewGame" };
        EngineHandlerList mPlayerAddedHandlers{ "onPlayerAdded" };
        EngineHandlerList mOnActivateHandlers{ "onActivate" };
        EngineHandlerList mOnDroppedHandlers{ "onDropped" };
        EngineHandlerList mOnPlacedHandlers{ "onPlaced" };
        EngineHandlerList mOnObjectStateRequestHandlers{ "onObjectStateRequest" };
        EngineHandlerList mOnObjectTransformHandlers{ "onObjectTransform" };
        EngineHandlerList mOnContainerScriptWriteHandlers{ "onContainerScriptWrite" };
        EngineHandlerList mOnUseItemHandlers{ "_onUseItem" };
        EngineHandlerList mOnNewExteriorHandlers{ "onNewExterior" };
        EngineHandlerList mOnProjectileHit{ "_onProjectileHit" };
    };

}

#endif // MWLUA_GLOBALSCRIPTS_H
