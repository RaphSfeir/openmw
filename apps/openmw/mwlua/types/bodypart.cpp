#include "types.hpp"

#include "modelproperty.hpp"

#include <components/esm3/loadbody.hpp>
#include <components/lua/luastate.hpp>

namespace sol
{
    template <>
    struct is_automagical<ESM::BodyPart> : std::false_type
    {
    };
}

namespace MWLua
{
    void addBodyPartBindings(sol::table bodypart, const Context& context)
    {
        addRecordFunctionBinding<ESM::BodyPart>(bodypart, context);

        sol::state_view lua = context.sol();
        sol::usertype<ESM::BodyPart> record = lua.new_usertype<ESM::BodyPart>("ESM3_BodyPart");
        record[sol::meta_function::to_string]
            = [](const ESM::BodyPart& rec) { return "ESM3_BodyPart[" + rec.mId.toDebugString() + "]"; };
        record["id"] = sol::readonly_property([](const ESM::BodyPart& rec) -> ESM::RefId { return rec.mId; });
        record["race"] = sol::readonly_property([](const ESM::BodyPart& rec) -> ESM::RefId { return rec.mRace; });
        addModelProperty(record);
        record["isFemale"] = sol::readonly_property(
            [](const ESM::BodyPart& rec) -> bool { return rec.mData.mFlags & ESM::BodyPart::BPF_Female; });
        record["isPlayable"] = sol::readonly_property(
            [](const ESM::BodyPart& rec) -> bool { return !(rec.mData.mFlags & ESM::BodyPart::BPF_NotPlayable); });
        record["isVampire"]
            = sol::readonly_property([](const ESM::BodyPart& rec) -> bool { return rec.mData.mVampire; });
        // WHICH PART OF THE BODY THIS IS -- head, hair, chest and so on.
        //
        // "type" next to it answers skin/clothing/armor, which is a different
        // question and the only one that was exposed. Without this a script
        // cannot tell a face from a hairstyle, so it cannot offer either: the
        // whole point of enumerating body parts is to list the heads for a
        // race and then the hairs, and mPart is the field that separates them.
        // Needed by a character creation screen written in Lua; the engine's
        // own chargen reads mData.mPart directly (mwgui/race.cpp getBodyParts).
        record["part"] = sol::readonly_property([](const ESM::BodyPart& rec) -> std::string_view {
            static constexpr std::string_view names[ESM::BodyPart::MP_Count] = { "head", "hair", "neck",
                "chest", "groin", "hand", "wrist", "forearm", "upperarm", "foot", "ankle", "knee", "upperleg",
                "clavicle", "tail" };
            if (rec.mData.mPart < ESM::BodyPart::MP_Count)
                return names[rec.mData.mPart];
            return "unknown";
        });
        record["type"] = sol::readonly_property([](const ESM::BodyPart& rec) -> std::optional<std::string_view> {
            if (rec.mData.mType == ESM::BodyPart::MT_Skin)
                return "skin";
            else if (rec.mData.mType == ESM::BodyPart::MT_Clothing)
                return "clothing";
            else if (rec.mData.mType == ESM::BodyPart::MT_Armor)
                return "armor";
            return {};
        });
    }
}
