#ifndef GAME_MWBASE_LUAMANAGER_H
#define GAME_MWBASE_LUAMANAGER_H

#include <filesystem>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include <SDL_events.h>
#include <osg/Quat>
#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>
#include <components/sdlutil/events.hpp>

#include "../mwmechanics/attacktype.hpp"
#include "../mwmechanics/damagesourcetype.hpp"
#include "../mwrender/animationpriority.hpp"

namespace MWWorld
{
    class CellStore;
    class Ptr;
}

namespace Loading
{
    class Listener;
}

namespace ESM
{
    class ESMReader;
    class ESMWriter;
    class RefId;
    struct LuaScripts;
    struct DialInfo;
    struct Dialogue;
}

namespace LuaUtil
{
    namespace InputAction
    {
        class Registry;
    }
}

namespace osg
{
    class Vec3f;
}

namespace MWBase
{
    // \brief LuaManager is the central interface through which the engine invokes lua scripts.
    //
    // The native side invokes functions on this interface, which queues events to be handled by the
    // scripts in the lua thread. Synchronous calls are not possible.
    //
    // The main implementation is in apps/openmw/mwlua/luamanagerimp.cpp.
    // Lua logic in general lives under apps/openmw/mwlua and this interface is
    // the main way for the rest of the engine to interact with the logic there.
    class LuaManager
    {
    public:
        virtual ~LuaManager() = default;

        virtual void contentFilesLoaded() = 0;
        virtual void newGameStarted() = 0;
        virtual void gameLoaded() = 0;
        virtual void gameEnded() = 0;
        virtual void noGame() = 0;
        virtual void objectAddedToScene(const MWWorld::Ptr& ptr) = 0;
        virtual void objectRemovedFromScene(const MWWorld::Ptr& ptr) = 0;
        virtual void objectTeleported(const MWWorld::Ptr& ptr) = 0;
        virtual void itemConsumed(const MWWorld::Ptr& consumable, const MWWorld::Ptr& actor) = 0;
        virtual void objectDropped(const MWWorld::Ptr& object, const MWWorld::Ptr& actor, const osg::Vec3f& position,
            const osg::Quat& rotation)
            = 0;
        virtual void objectPlaced(const MWWorld::Ptr& object, const MWWorld::Ptr& actor, const osg::Vec3f& position,
            const osg::Quat& rotation)
            = 0;
        virtual void objectActivated(const MWWorld::Ptr& object, const MWWorld::Ptr& actor) = 0;
        // MP: an mwscript asked for an object to be enabled or disabled. In a
        // net session the opcode does NOT apply it (see mwscript
        // miscextensions OpEnable/OpDisable); it becomes a request the session
        // arbitrates, so every machine ends up with the same answer instead of
        // each client's own copy of the script deciding for itself.
        virtual void objectStateRequest(const MWWorld::Ptr& object, bool enable) = 0;
        // MP: a script moved, rotated or rescaled a non-actor object. Unlike
        // enable/disable the engine DOES apply it first -- these are absolute
        // values that converge, and suppressing them would show the object in
        // the wrong place for a round trip -- so this reports the RESULT.
        virtual void objectTransformed(const MWWorld::Ptr& object) = 0;
        // MP: an mwscript AddItem/RemoveItem changed a WORLD CONTAINER's
        // contents and the engine HAS applied it -- the objectTransformed
        // choice, not the deferred one, because a script may read the store
        // back immediately. `delta` is signed; `script` names the running
        // script ("dialogue:<speaker>" for a result script, "unknown" from the
        // console). Nothing else in the engine reports a container change.
        virtual void containerScriptWrite(const MWWorld::Ptr& object, std::string_view script, const ESM::RefId& item,
            int delta, bool levelled)
            = 0;
        virtual void useItem(const MWWorld::Ptr& object, const MWWorld::Ptr& actor, bool force) = 0;
        virtual void animationTextKey(const MWWorld::Ptr& actor, const std::string& key) = 0;
        virtual void playAnimation(const MWWorld::Ptr& object, const std::string& groupname,
            const MWRender::AnimPriority& priority, int blendMask, bool autodisable, float speedmult,
            std::string_view start, std::string_view stop, float startpoint, uint32_t loops, bool loopfallback)
            = 0;
        virtual void animationEnded(const MWWorld::Ptr& actor, std::string_view groupname, float time, float completion,
            std::string_view startKey, std::string_view stopKey)
            = 0;
        virtual void jailTimeServed(const MWWorld::Ptr& actor, int days) = 0;
        virtual void skillLevelUp(const MWWorld::Ptr& actor, ESM::RefId skillId, std::string_view source) = 0;
        virtual void skillUse(const MWWorld::Ptr& actor, ESM::RefId skillId, int useType, float scale) = 0;
        virtual void onHit(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, const MWWorld::Ptr& weapon,
            const MWWorld::Ptr& ammo, int attackType, float attackStrength, float attackWindUp, float damage,
            bool isHealth, const osg::Vec3f& hitPos, bool successful, MWMechanics::DamageSourceType)
            = 0;
        virtual void exteriorCreated(MWWorld::CellStore& cell) = 0;
        virtual void actorDied(const MWWorld::Ptr& actor) = 0;
        virtual void onDialogueResponse(
            const MWWorld::Ptr& actor, const ESM::DialInfo& info, const ESM::Dialogue& record)
            = 0;
        // MP: the player chose a persuasion a script has claimed
        // (DialogueManager::setPersuasionClaimed). The engine did nothing with
        // it; the player's scripts get 'PersuasionClaimed' and answer it.
        virtual void onPersuasionClaimed(const MWWorld::Ptr& actor, int type) = 0;
        virtual void questUpdated(const ESM::RefId& questId, int stage) = 0;
        // `arg` is either forwarded from MWGui::pushGuiMode or empty
        virtual void uiModeChanged(const MWWorld::Ptr& arg) = 0;
        virtual void viewportResized(int width, int height) = 0;
        virtual void savePermanentStorage(const std::filesystem::path& userConfigPath) = 0;
        // MP: write the map memory once more, to the file the last
        // openmw.campaign.saveMap named, while the world is still whole:
        // at quit, and before a running game is cleared. `forget` drops the
        // name afterwards, so a later game never writes over it.
        virtual void saveMapMemoryAgain(bool forget) = 0;
        virtual void applyMagicEffects(ESM::RefId id, const MWWorld::Ptr& caster, ESM::RefNum item,
            const MWWorld::Ptr& target, const std::vector<int>& effects, bool ignoreReflect, bool ignoreSpellAbsorption,
            bool stackable, bool isReflect)
            = 0;
        virtual void magicProjectileHit(ESM::RefId spellId, const MWWorld::Ptr& caster, ESM::RefNum item,
            const MWWorld::Ptr& victim, const osg::Vec3f& position, const osg::Vec3f& normal)
            = 0;
        // TODO: notify LuaManager about other events
        // virtual void objectOnHit(const MWWorld::Ptr &ptr, float damage, bool ishealth, const MWWorld::Ptr &object,
        //                          const MWWorld::Ptr &attacker, const osg::Vec3f &hitPosition, bool successful,
        //                          DamageSourceType sourceType) = 0;

        struct InputEvent
        {
            struct WheelChange
            {
                int x;
                int y;
            };

            enum
            {
                KeyPressed,
                KeyReleased,
                ControllerPressed,
                ControllerReleased,
                Action,
                TouchPressed,
                TouchReleased,
                TouchMoved,
                MouseButtonPressed,
                MouseButtonReleased,
                MouseWheel,
            } mType;
            std::variant<SDL_Keysym, int, SDLUtil::TouchEvent, WheelChange> mValue;
        };
        virtual void inputEvent(const InputEvent& event) = 0;

        /// Fork addition: a GUI sound was played locally (potion success or
        /// fail, enchanting, repairs...). Reported to the player's Lua as an
        /// 'MP_UiSound' event so multiplayer can share the ones worth
        /// hearing; Lua curates, this only reports. The engine's own sound
        /// is not affected.
        virtual void uiSoundPlayed(std::string_view soundId) = 0;

        struct ActorControls
        {
            bool mDisableAI = false;
            // mp: this actor stands in for another PLAYER in a session. The
            // local player's hits still land on it, but they are not a crime
            // and must not make the body or any witness hostile. Session-
            // scoped by design (not serialized); set from the stand-in's own
            // local script.
            bool mIsAlly = false;
            bool mChanged = false;

            bool mJump = false;
            bool mRun = false;
            bool mSneak = false;
            float mMovement = 0;
            float mSideMovement = 0;
            float mPitchChange = 0;
            float mYawChange = 0;
            MWMechanics::AttackType mUse = MWMechanics::AttackType::NoAttack;
        };

        virtual ActorControls* getActorControls(const MWWorld::Ptr&) const = 0;

        virtual void clear() = 0;
        virtual void setupPlayer(const MWWorld::Ptr&) = 0;

        // Saving
        size_t countSavedGameRecords() const { return 1; }
        virtual void write(ESM::ESMWriter& writer, Loading::Listener& progress) = 0;
        virtual void saveLocalScripts(const MWWorld::Ptr& ptr, ESM::LuaScripts& data) = 0;

        // Must be called before save, otherwise the world can be saved in an inconsistent state.
        virtual void applyDelayedActions() = 0;

        // Loading from a save
        virtual void readRecord(ESM::ESMReader& reader, uint32_t type) = 0;
        virtual void loadLocalScripts(const MWWorld::Ptr& ptr, const ESM::LuaScripts& data) = 0;

        // Should be called before loading. The map is used to fix refnums if the order of content files was changed.
        virtual void setContentFileMapping(const std::map<int, int>&) = 0;

        // Drops script cache and reloads all scripts. Calls `onSave` and `onLoad` for every script.
        virtual void reloadAllScripts() = 0;

        virtual void handleConsoleCommand(
            const std::string& consoleMode, const std::string& command, const MWWorld::Ptr& selectedPtr)
            = 0;

        virtual std::string formatResourceUsageStats() const = 0;

        // True while a multiplayer session is running. Engine behaviours that assume a
        // single local player owning the game state (ending the game when that player
        // dies) defer to the multiplayer layer instead.
        virtual bool isNetSessionActive() const { return false; }
    };

}

#endif // GAME_MWBASE_LUAMANAGER_H
