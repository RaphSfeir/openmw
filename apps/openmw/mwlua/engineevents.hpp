#ifndef MWLUA_ENGINEEVENTS_H
#define MWLUA_ENGINEEVENTS_H

#include <variant>

#include <osg/Vec3f>

#include <components/esm3/cellref.hpp> // defines RefNum that is used as a unique id
#include <components/lua/utilpackage.hpp>

#include "../mwworld/cellstore.hpp"

namespace MWLua
{
    class GlobalScripts;

    class EngineEvents
    {
    public:
        explicit EngineEvents(GlobalScripts& globalScripts)
            : mGlobalScripts(globalScripts)
        {
        }

        struct OnActive
        {
            ESM::RefNum mObject;
        };
        struct OnInactive
        {
            ESM::RefNum mObject;
        };
        struct OnTeleported
        {
            ESM::RefNum mObject;
        };
        struct OnActivate
        {
            ESM::RefNum mActor;
            ESM::RefNum mObject;
        };
        struct OnUseItem
        {
            ESM::RefNum mActor;
            ESM::RefNum mObject;
            bool mForce;
        };
        struct OnConsume
        {
            ESM::RefNum mActor;
            ESM::RefNum mConsumable;
        };
        struct OnNewExterior
        {
            MWWorld::CellStore& mCell;
        };
        struct OnAnimationTextKey
        {
            ESM::RefNum mActor;
            std::string mGroupname;
            std::string mKey;
        };
        struct OnAnimationEnded
        {
            ESM::RefNum mActor;
            std::string mGroupname;
            std::string mStartKey;
            std::string mStopKey;
            float mTime;
            float mCompletion;
        };
        struct OnSkillUse
        {
            ESM::RefNum mActor;
            std::string mSkill;
            int useType;
            float scale;
        };
        struct OnSkillLevelUp
        {
            ESM::RefNum mActor;
            std::string mSkill;
            std::string mSource;
        };
        struct OnJailTimeServed
        {
            ESM::RefNum mActor;
            int mDays;
        };
        struct OnDropped
        {
            ESM::RefNum mObject;
            ESM::RefNum mActor;
            osg::Vec3f mPosition;
            LuaUtil::TransformQ mRotation;
        };
        struct OnPlaced
        {
            ESM::RefNum mObject;
            ESM::RefNum mActor;
            osg::Vec3f mPosition;
            LuaUtil::TransformQ mRotation;
        };
        // MP: an mwscript Enable/Disable, reported instead of applied while a
        // net session is active.
        struct OnStateRequest
        {
            ESM::RefNum mObject;
            bool mEnable;
        };
        // MP: a script's move/rotate/rescale of a non-actor object, reported
        // after the engine has applied it.
        struct OnTransformed
        {
            ESM::RefNum mObject;
        };
        using Event = std::variant<OnActive, OnInactive, OnConsume, OnActivate, OnUseItem, OnNewExterior, OnTeleported,
            OnAnimationTextKey, OnAnimationEnded, OnSkillUse, OnSkillLevelUp, OnJailTimeServed, OnDropped, OnPlaced,
            OnStateRequest, OnTransformed>;

        void clear() { mQueue.clear(); }
        void addToQueue(Event e) { mQueue.push_back(std::move(e)); }
        void callEngineHandlers();

    private:
        class Visitor;

        GlobalScripts& mGlobalScripts;
        std::vector<Event> mQueue;
    };

}

#endif // MWLUA_ENGINEEVENTS_H
