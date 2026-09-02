#include "voipbindings.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include <sol/sol.hpp>

#include <components/lua/luastate.hpp>

#include "../mwbase/environment.hpp"
#include "../mwsound/voipmanager.hpp"
#include "../mwworld/ptr.hpp"

#include "context.hpp"
#include "objectvariant.hpp"

namespace MWLua
{
    namespace
    {
        // Absent on a --no-sound or headless client. That is an ordinary state
        // for a machine to be in rather than an error, so the package is still
        // registered and every entry point below answers nil or does nothing
        // instead of throwing - a script should not have to guard each call to
        // find out whether this build has a voice subsystem.
        MWSound::VoipManager* voip()
        {
            return MWBase::Environment::get().getVoipManager();
        }
    }

    sol::table initVoipPlayerPackage(const Context& context)
    {
        sol::table api(context.sol(), sol::create);

        // Every lambda here can land on either the main thread or the Lua worker
        // thread: a player script's onKeyPress and onFrame are dispatched from
        // synchronizedUpdate while its onUpdate, timers and event handlers run on
        // the worker. So nothing below may touch the sound manager or the scene
        // graph directly - the manager's setters only move atomics and queued
        // requests, and VoipManager::update() does the rest on the main thread.

        // Which body a speaker is talking through. The mod owns this mapping --
        // a peer id means nothing to the engine, and which puppet belongs to
        // whom is knowledge only the multiplayer scripts have.
        //
        // Both only queue: a script may be on the Lua worker thread, and a
        // stream may only be moved on the main thread.
        api["attach"] = [](unsigned int speakerId, const sol::object& object) {
            MWSound::VoipManager* manager = voip();
            if (manager == nullptr)
                return;
            MWWorld::Ptr ptr = ObjectVariant(object).ptr();
            if (ptr.isEmpty())
                throw std::runtime_error("voip.attach: invalid object");
            manager->requestAttach(static_cast<std::uint32_t>(speakerId), ptr);
        };

        api["detach"] = [](unsigned int speakerId) {
            if (MWSound::VoipManager* manager = voip())
                manager->requestDetach(static_cast<std::uint32_t>(speakerId));
        };

        api["setLoopback"] = [](bool enabled) {
            if (MWSound::VoipManager* manager = voip())
                manager->setLoopback(enabled);
        };

        api["setTransmit"] = [](bool transmit) {
            if (MWSound::VoipManager* manager = voip())
                manager->setTransmit(transmit);
        };

        api["transmitting"] = []() -> sol::optional<bool> {
            if (MWSound::VoipManager* manager = voip())
                return manager->transmitting();
            return sol::nullopt;
        };

        // nil selects the system default, which on a machine whose default is a
        // virtual routing endpoint is exactly how a working build looks broken.
        api["setCaptureDevice"] = [](sol::optional<std::string> name) {
            if (MWSound::VoipManager* manager = voip())
                manager->setCaptureDevice(name ? *name : std::string());
        };

        // Answers even without a manager: enumeration is a property of the ALC
        // library, not of the voice subsystem, and a device picker that goes
        // blank on a --no-sound client is worse than one that lists what is there.
        api["captureDevices"] = [](sol::this_state s) {
            sol::state_view lua(s);
            sol::table result(lua, sol::create);
            int index = 1;
            for (const std::string& name : MWSound::VoipManager::captureDevices())
                result[index++] = name;
            return result;
        };

        api["micLevel"] = []() -> sol::optional<float> {
            if (MWSound::VoipManager* manager = voip())
                return manager->micLevel();
            return sol::nullopt;
        };

        api["micStatus"] = []() -> sol::optional<std::string> {
            if (MWSound::VoipManager* manager = voip())
                return std::string(manager->micStatusName());
            return sol::nullopt;
        };

        api["setInputGain"] = [](float gain) {
            if (MWSound::VoipManager* manager = voip())
                manager->setInputGain(gain);
        };

        api["stats"] = [](sol::this_state s) -> sol::optional<sol::table> {
            MWSound::VoipManager* manager = voip();
            if (manager == nullptr)
                return sol::nullopt;

            const MWSound::VoipStats stats = manager->stats();
            sol::state_view lua(s);
            sol::table result(lua, sol::create);
            result["jitterMs"] = stats.mJitterMs;
            result["streamDelayMs"] = stats.mStreamDelayMs;
            result["totalMs"] = stats.mTotalMs;
            result["targetFrames"] = stats.mTargetFrames;
            // Counters go over as doubles rather than integers: this Lua has no
            // 64-bit integer type, and a silent wrap in a statistic is worse than
            // the precision a session will never reach.
            result["sentPackets"] = static_cast<double>(stats.mSentPackets);
            result["played"] = static_cast<double>(stats.mPlayed);
            result["fecRecovered"] = static_cast<double>(stats.mFecRecovered);
            result["concealed"] = static_cast<double>(stats.mConcealed);
            result["underruns"] = static_cast<double>(stats.mUnderruns);

            // The network half. sentPackets above counts frames the encoder
            // produced - the figure the loopback drill grades itself on, left
            // alone so its probe keeps meaning what it meant - while netPackets
            // counts the datagrams that reached the transport. A two-client
            // drill compares one machine's netPackets against the other's
            // receivedPackets; both should climb at fifty a second.
            result["netPackets"] = static_cast<double>(stats.mNetPackets);
            result["droppedSends"] = static_cast<double>(stats.mDroppedSends);
            result["oversizeFrames"] = static_cast<double>(stats.mOversizeFrames);
            result["receivedPackets"] = static_cast<double>(stats.mReceivedPackets);
            result["droppedMalformed"] = static_cast<double>(stats.mDroppedMalformed);
            result["droppedVersion"] = static_cast<double>(stats.mDroppedVersion);
            result["droppedOversize"] = static_cast<double>(stats.mDroppedOversize);
            result["refusedSpeakers"] = static_cast<double>(stats.mRefusedSpeakers);
            // Only a listen host can count these, and only against a client
            // that wrote a speaker id into a field the wire format reserves for
            // the relay. The frames were played under the id the transport
            // supplied, so this is a report about the sender, not about lost
            // audio: anything but zero here names a client to look at.
            result["spoofedOrigin"] = static_cast<double>(stats.mSpoofedOrigin);
            // nil until something has actually been heard, so that "nobody" and
            // "peer 0", which is the host, cannot be confused for each other.
            if (stats.mHeardAnyone)
                result["lastSpeaker"] = stats.mLastSpeaker;

            sol::table speakers(lua, sol::create);
            int index = 1;
            for (const MWSound::VoipSpeakerStats& speaker : stats.mRemote)
            {
                sol::table entry(lua, sol::create);
                entry["id"] = speaker.mId;
                entry["jitterMs"] = speaker.mJitterMs;
                entry["streamDelayMs"] = speaker.mStreamDelayMs;
                entry["totalMs"] = speaker.mJitterMs + speaker.mStreamDelayMs;
                entry["targetFrames"] = speaker.mTargetFrames;
                entry["attached"] = speaker.mAttached;
                entry["received"] = static_cast<double>(speaker.mReceived);
                entry["late"] = static_cast<double>(speaker.mLate);
                entry["played"] = static_cast<double>(speaker.mPlayed);
                entry["fecRecovered"] = static_cast<double>(speaker.mFecRecovered);
                entry["concealed"] = static_cast<double>(speaker.mConcealed);
                entry["underruns"] = static_cast<double>(speaker.mUnderruns);
                speakers[index++] = entry;
            }
            result["speakers"] = speakers;
            return result;
        };

        // An empty table without a manager rather than nil: an indicator that
        // iterates this every frame should find nobody talking, not an error.
        api["speakers"] = [](sol::this_state s) {
            sol::state_view lua(s);
            sol::table result(lua, sol::create);
            MWSound::VoipManager* manager = voip();
            if (manager == nullptr)
                return result;
            int index = 1;
            for (const std::uint32_t id : manager->speakerIds())
                result[index++] = id;
            return result;
        };

        return LuaUtil::makeReadOnly(api);
    }
}
