#ifndef OPENMW_ESM_CELLREF_H
#define OPENMW_ESM_CELLREF_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>

#include "refnum.hpp"

namespace ESM
{
    class ESMWriter;
    class ESMReader;

    /* Cell reference. This represents ONE object (of many) inside the
    cell. The cell references are not loaded as part of the normal
    loading process, but are rather loaded later on demand when we are
    setting up a specific cell.
    */

    class CellRef
    {
    public:
        static constexpr std::string_view getRecordType() { return "CellRef"; }

        // Reference number
        // Note: Currently unused for items in containers
        RefNum mRefNum;

        ESM::RefId mRefID; // ID of object being referenced

        float mScale; // Scale applied to mesh

        // The NPC that owns this object (and will get angry if you steal it)
        ESM::RefId mOwner;

        // Name of a global variable. If the global variable is set to '1', using the object is temporarily allowed
        // even if it has an Owner field.
        // Used by bed rent scripts to allow the player to use the bed for the duration of the rent.
        std::string mGlobalVariable;

        // ID of creature trapped in this soul gem
        ESM::RefId mSoul;

        // MULTIPLAYER IDENTITY. A number the session puts on this reference so
        // every machine can name the same object, assigned once and carried for
        // the reference's whole life. 0 = never assigned.
        //
        // Needed because a RUNTIME object has no portable identity of its own:
        // mRefNum is meaningful only in the cell the content files placed it
        // in, and entering a container unsets it outright (ContainerStore)
        // along with any Lua script state attached to it. So an item picked up
        // and dropped again comes back as a brand new reference with a
        // process-local number that means nothing to anyone else. Without a
        // field like this one a multiplayer layer has to GUESS which object is
        // which -- by record and position, which stops working the moment the
        // object moves, settles or is re-created in a different order -- and
        // every guess is a chance to lose an object or to duplicate it.
        //
        // Deliberately NOT cleared by unsetRefNum: that erases a cell-local
        // address, while this is meant to outlive the cell. A stack that merges
        // keeps one id, which is correct -- a pile of five lockpicks is one
        // object in the world.
        uint32_t mMpId;

        // The faction that owns this object (and will get angry if
        // you take it and are not a faction member)
        ESM::RefId mFaction;

        // PC faction rank required to use the item. Sometimes is -1, which means "any rank".
        int32_t mFactionRank;

        // For weapon or armor, this is the remaining item health.
        // For tools (lockpicks, probes, repair hammer) it is the remaining uses.
        // For lights it is remaining time.
        // This could be -1 if the charge was not touched yet (i.e. full).
        union
        {
            int32_t mChargeInt; // Used by everything except lights
            float mChargeFloat; // Used only by lights
        };
        float mChargeIntRemainder; // Fractional part of mChargeInt

        // Remaining enchantment charge. This could be -1 if the charge was not touched yet (i.e. full).
        float mEnchantmentCharge;

        int32_t mCount;

        // For doors - true if this door teleports to somewhere else, false
        // if it should open through animation.
        bool mTeleport;

        // Teleport location for the door, if this is a teleporting door.
        Position mDoorDest;

        // Destination cell for doors (optional)
        std::string mDestCell;

        // Lock level for doors and containers
        int32_t mLockLevel;
        bool mIsLocked{};
        ESM::RefId mKey, mTrap; // Key and trap ID names, if any

        // This corresponds to the "Reference Blocked" checkbox in the construction set,
        // which prevents editing that reference.
        // -1 is not blocked, otherwise it is blocked.
        signed char mReferenceBlocked;

        // Position and rotation of this object within the cell
        Position mPos;

        /// Calls loadId and loadData
        void load(ESMReader& esm, bool& isDeleted, bool wideRefNum = false);

        void loadId(ESMReader& esm, bool wideRefNum = false);

        /// Implicitly called by load
        void loadData(ESMReader& esm, bool& isDeleted);

        void save(ESMWriter& esm, bool wideRefNum = false, bool inInventory = false, bool isDeleted = false) const;

        void blank();
    };

    void skipLoadCellRef(ESMReader& esm, bool wideRefNum = false);

    CellRef makeBlankCellRef();
}

#endif
