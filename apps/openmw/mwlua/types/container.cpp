#include "types.hpp"

#include "modelproperty.hpp"

#include <components/esm3/loadcont.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/finitevalues.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwbase/world.hpp"
#include "apps/openmw/mwrender/animation.hpp"
#include "apps/openmw/mwworld/class.hpp"

namespace sol
{
    template <>
    struct is_automagical<ESM::Container> : std::false_type
    {
    };
}

namespace
{
    ESM::Container tableToContainer(const sol::table& rec)
    {
        ESM::Container cont;

        // Start from template if provided
        if (rec["template"] != sol::nil)
            cont = LuaUtil::cast<ESM::Container>(rec["template"]);
        else
            cont.blank();

        // Basic fields
        if (rec["name"] != sol::nil)
            cont.mName = rec["name"];
        if (rec["model"] != sol::nil)
            cont.mModel = Misc::ResourceHelpers::meshPathForESM3(rec["model"].get<std::string_view>());
        if (rec["mwscript"] != sol::nil)
            cont.mScript = ESM::RefId::deserializeText(rec["mwscript"].get<std::string_view>());
        if (rec["weight"] != sol::nil)
            cont.mWeight = rec["weight"].get<Misc::FiniteFloat>();

        // Flags
        if (rec["isOrganic"] != sol::nil)
        {
            bool isOrganic = rec["isOrganic"];
            if (isOrganic)
                cont.mFlags |= ESM::Container::Organic;
            else
                cont.mFlags &= ~ESM::Container::Organic;
        }

        if (rec["isRespawning"] != sol::nil)
        {
            bool isRespawning = rec["isRespawning"];
            if (isRespawning)
                cont.mFlags |= ESM::Container::Respawn;
            else
                cont.mFlags &= ~ESM::Container::Respawn;
        }

        return cont;
    }
}

namespace MWLua
{

    static const MWWorld::Ptr& containerPtr(const Object& o)
    {
        return verifyType(ESM::REC_CONT, o.ptr());
    }

    void addContainerBindings(sol::table container, const Context& context)
    {
        container["content"] = sol::overload([](const LObject& o) { return Inventory<LObject>{ o }; },
            [](const GObject& o) { return Inventory<GObject>{ o }; });
        container["inventory"] = container["content"];
        container["getEncumbrance"] = [](const Object& obj) -> float {
            const MWWorld::Ptr& ptr = containerPtr(obj);
            return ptr.getClass().getEncumbrance(ptr);
        };
        container["createRecordDraft"] = tableToContainer;
        container["encumbrance"] = container["getEncumbrance"]; // for compatibility; should be removed later
        container["getCapacity"] = [](const Object& obj) -> float {
            const MWWorld::Ptr& ptr = containerPtr(obj);
            return ptr.getClass().getCapacity(ptr);
        };
        container["capacity"] = container["getCapacity"]; // for compatibility; should be removed later

        // mp: graphic herbalism, exposed for the multiplayer layer.
        //
        // Whether activating a plant harvests it (contents straight into the
        // inventory, mesh switched) or opens a window is decided from a setting
        // and a switch node in the MESH — neither visible to Lua, and a sync
        // layer that guesses would either swallow ordinary container windows or
        // claim plants the engine then refuses to harvest.
        container["canBeHarvested"] = [](const Object& obj) -> bool {
            const MWWorld::Ptr& ptr = containerPtr(obj);
            if (!Settings::game().mGraphicHerbalism)
                return false;
            const MWRender::Animation* anim = MWBase::Environment::get().getWorld()->getAnimation(ptr);
            return anim != nullptr && anim->canBeHarvested();
        };
        // And the other half: emptying a plant's store does not flip its mesh,
        // because the harvested visual is only derived when the object's
        // animation is first built. A machine told that somebody ELSE picked
        // this plant has to ask for that re-evaluation explicitly. Returns
        // false when the object is not rendered here — nothing to refresh, and
        // a freshly built animation derives the state on its own.
        container["refreshHarvested"] = [](const GObject& obj) -> bool {
            const MWWorld::Ptr& ptr = containerPtr(obj);
            MWRender::Animation* anim = MWBase::Environment::get().getWorld()->getAnimation(ptr);
            if (anim == nullptr)
                return false;
            // Self-guarding: harvest() switches nothing while the store still
            // holds visible items.
            anim->harvest(ptr);
            return true;
        };

        addRecordFunctionBinding<ESM::Container>(container, context);

        sol::usertype<ESM::Container> record = context.sol().new_usertype<ESM::Container>("ESM3_Container");
        record[sol::meta_function::to_string] = [](const ESM::Container& rec) -> std::string {
            return "ESM3_Container[" + rec.mId.toDebugString() + "]";
        };
        record["id"]
            = sol::readonly_property([](const ESM::Container& rec) -> std::string { return rec.mId.serializeText(); });
        record["name"] = sol::readonly_property([](const ESM::Container& rec) -> std::string { return rec.mName; });
        addModelProperty(record);
        record["mwscript"]
            = sol::readonly_property([](const ESM::Container& rec) -> ESM::RefId { return rec.mScript; });
        record["weight"] = sol::readonly_property([](const ESM::Container& rec) -> float { return rec.mWeight; });
        record["isOrganic"] = sol::readonly_property(
            [](const ESM::Container& rec) -> bool { return rec.mFlags & ESM::Container::Organic; });
        record["isRespawning"] = sol::readonly_property(
            [](const ESM::Container& rec) -> bool { return rec.mFlags & ESM::Container::Respawn; });
    }
}
