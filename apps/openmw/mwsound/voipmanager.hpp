#ifndef GAME_SOUND_VOIPMANAGER_H
#define GAME_SOUND_VOIPMANAGER_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../mwworld/ptr.hpp"

#include "voicecapture.hpp"
#include "voicecodec.hpp"
#include "voicestream.hpp"

namespace Net
{
    class Session;
}

namespace MWSound
{
    class NetworkVoiceDecoder;
    class SoundManager;
    class Stream;

    // One remote speaker's row in the stats snapshot. Republished with the rest
    // of it once per frame, so a script iterating speakers never trips over one
    // the main thread is retiring underneath it.
    struct VoipSpeakerStats
    {
        std::uint32_t mId = 0;
        int mJitterMs = 0;
        int mStreamDelayMs = 0;
        int mTargetFrames = 0;
        std::uint64_t mReceived = 0;
        std::uint64_t mLate = 0;
        std::uint64_t mPlayed = 0;
        std::uint64_t mFecRecovered = 0;
        std::uint64_t mConcealed = 0;
        std::uint64_t mUnderruns = 0;
        bool mAttached = false;
    };

    // What openmw.voip.stats() hands to Lua, republished once per frame by
    // update(). Written on the main thread and read on whichever thread the
    // script happens to be on, so it is copied whole under one lock rather than
    // read a field at a time: a reader that saw a fresh jitter figure against a
    // stale stream delay would report a total that never existed.
    struct VoipStats
    {
        int mJitterMs = 0;
        int mStreamDelayMs = 0;
        int mTotalMs = 0;
        int mTargetFrames = 0;
        std::uint64_t mSentPackets = 0;
        std::uint64_t mPlayed = 0;
        std::uint64_t mFecRecovered = 0;
        std::uint64_t mConcealed = 0;
        std::uint64_t mUnderruns = 0;

        // Send side of the network path. mSentPackets counts frames the encoder
        // produced, which is what the loopback drill grades itself on and is
        // therefore left alone; mNetPackets counts the datagrams that reached
        // the transport, which is the figure the two-client drill compares
        // against the far end's mReceivedPackets. They differ whenever transmit
        // is on but the session is not live.
        std::uint64_t mNetPackets = 0;
        std::uint64_t mDroppedSends = 0; // Session's own count for the voice channel
        std::uint64_t mOversizeFrames = 0; // encoder output too big for the wire clamp

        // Receive side. Every rejection lands in exactly one of these, because
        // at 50 pps per speaker a log line per dropped packet is a denial of
        // service against the log rather than a diagnostic.
        std::uint64_t mReceivedPackets = 0;
        std::uint64_t mDroppedMalformed = 0;
        std::uint64_t mDroppedVersion = 0;
        std::uint64_t mDroppedOversize = 0;
        std::uint64_t mRefusedSpeakers = 0;
        // Frames that reached a HOST carrying a speaker id the sender had no
        // business writing. They are played under the id the transport gave,
        // so this is evidence rather than a loss count: it is how an operator
        // sees a client lying about who it is.
        std::uint64_t mSpoofedOrigin = 0;

        std::uint32_t mLastSpeaker = 0;
        bool mHeardAnyone = false; // whether mLastSpeaker means anything yet
        std::vector<VoipSpeakerStats> mRemote;
    };

    // Joins capture, codec and playback into one voice path, and is the only
    // thing openmw.voip talks to.
    //
    // Four threads meet in here and the split between them is the whole design:
    //
    //  - Lua player scripts call the setters and the getters. They have no fixed
    //    thread: onKeyPress and onFrame are dispatched from synchronizedUpdate on
    //    the main thread, while onUpdate, timers and event handlers run on the
    //    Lua worker thread. Nothing below may assume either one, so every
    //    Lua-reachable member is an atomic or is taken under a mutex, and none of
    //    them touch the sound manager.
    //  - update() is main thread only, called from Engine::frame. It is the only
    //    place a stream is attached or detached, because the sound manager's
    //    stream containers are plain maps that its own main-thread update
    //    iterates and erases from.
    //  - The transport's channel handler runs on the pump thread, which IS the
    //    main thread: Session::pump() is main-thread-only by contract and
    //    Engine::frame calls it through LuaManager::synchronizedUpdate a dozen
    //    lines before it calls update(). That is what lets the speaker table
    //    below be plain main-thread state with no lock: the packet handler and
    //    the attach/retire pass are the same thread, in the same frame, in that
    //    order. Lua never sees the table, only the stats snapshot.
    //  - The capture poll thread delivers whole 20 ms frames and, in loopback,
    //    carries them all the way to the jitter buffer without going near the
    //    main thread or the sound manager - wakeStreamThread aside, which takes
    //    no lock by design. That is what makes voice survive a loading screen,
    //    during which Engine::frame does not run at all.
    //  - The OpenAL stream thread pulls the other end of the jitter buffer
    //    through NetworkVoiceDecoder::read.
    class VoipManager
    {
    public:
        // Takes the concrete sound manager rather than MWBase::SoundManager:
        // playVoiceStream and wakeStreamThread are deliberately absent from the
        // abstract interface, because nothing outside voice has any use for them.
        explicit VoipManager(SoundManager& sounds);
        ~VoipManager();

        VoipManager(const VoipManager&) = delete;
        VoipManager& operator=(const VoipManager&) = delete;

        // The transport, borrowed. Main thread only, and only outside pump():
        // this is where the voice channel's handler is claimed and released, and
        // a handler may only be released from the thread that runs it.
        //
        // A raw pointer is safe rather than merely tolerable, because the
        // session outlives this object: it is a by-value member of the Lua
        // manager, which Engine destroys after the voip manager. Null is an
        // ordinary state - single player - in which everything below still
        // works, minus the network.
        //
        // Stops capture before it swaps the pointer, so the poll thread is
        // joined and provably not mid-send while it changes.
        void setSession(Net::Session* session);

        // Everything from here to update() is callable from any thread.

        // Say which body a speaker is talking through, or that they no longer
        // have one. Callable from a Lua script on either thread: both only
        // record the wish, and update() on the main thread is what actually
        // moves a stream. Attaching a speaker who already has a body is the
        // ordinary case, not an error.
        void requestAttach(std::uint32_t speakerId, const MWWorld::Ptr& body, const VoiceStreamParams& params = {});
        void requestDetach(std::uint32_t speakerId);

        // Route captured audio to a stream on the player's own head. The drill
        // this milestone exists for: it exercises capture, encode, jitter buffer,
        // decode and 3D playback with no network in the way.
        void setLoopback(bool enabled);
        bool loopback() const { return mLoopbackWanted.load(std::memory_order_relaxed); }

        void setTransmit(bool transmit);
        bool transmitting() const { return mTransmitWanted.load(std::memory_order_relaxed); }

        // Empty selects the system default. Applied by update(), because opening
        // a capture endpoint is slow enough that a script thread should not be
        // made to wait on it.
        void setCaptureDevice(const std::string& name);
        std::string captureDevice() const;

        // Re-enumerated on every call: capture endpoints appear and disappear
        // while the game runs, and openal-soft does not track that for us.
        static std::vector<std::string> captureDevices();

        float micLevel() const { return mCapture.level(); }
        std::string_view micStatusName() const;

        void setInputGain(float gain);
        float inputGain() const { return mInputGain.load(std::memory_order_relaxed); }

        VoipStats stats() const;

        // Ids of the speakers currently being heard, from the same snapshot as
        // stats() and therefore at most one frame old. Live enough for an
        // indicator, and it costs no lock on the packet path.
        std::vector<std::uint32_t> speakerIds() const;

        // Main thread only, from Engine::frame. Services what the script threads
        // asked for, republishes the stats snapshot and prints the latency line.
        // Never on the audio data path.
        void update(float dt);

        // Main thread only. Must run while the sound manager is still alive.
        void clear();

    private:
        // A remote speaker, from the first packet bearing its id until it falls
        // silent or the session ends. Main thread only, in the pump-is-main
        // sense described above.
        struct Speaker
        {
            std::shared_ptr<NetworkVoiceDecoder> mDecoder;
            // Borrowed from the sound manager, null while unattached. The same
            // caveat as mStream below: a save load throws the attachment away
            // without telling us, which is what mSoundsGeneration detects.
            Stream* mStream = nullptr;
            // The body this speaker is talking through, empty while their voice
            // is still coming out flat. Held so the stream can be stopped
            // against the same reference it was started against, and replaced
            // whenever the mod says the body has changed -- which is often:
            // core rebuilds a puppet on any identity or level change, so within
            // seconds of every join.
            MWWorld::Ptr mBody;
            // What mStream was actually CREATED with, which is not the same
            // question as what the mod last asked for. Reference distance, max
            // distance and rolloff are init-only on an AL source, so a settings
            // slider that moves while somebody is mid-sentence changes the
            // request and nothing else until the stream is rebuilt. Keeping the
            // built-with copy is how the sync loop knows a rebuild is owed.
            VoiceStreamParams mStreamParams;
            std::chrono::steady_clock::time_point mLastPacket{};
            std::uint64_t mReceived = 0;
            // What the sound manager was last told about this speaker's mouth. A
            // voice stream is attached for the whole session and mostly quiet,
            // so the spurt has to be pushed rather than inferred from the
            // stream existing -- see the comment on sayActive.
            bool mSpeaking = false;
            // One failed attach is a condition that does not fix itself within a
            // frame, so it is not retried until something changes.
            bool mAttachFailed = false;
        };

        void onCaptureFrame(const std::int16_t* pcm, std::size_t samples);
        // Capture poll thread. Wraps one encoded frame in the wire header and
        // hands it to the transport.
        void sendVoiceFrame(const unsigned char* payload, std::size_t bytes, std::uint16_t seq);
        // Pump thread, which is the main thread. Nothing here may allocate per
        // packet, reach into the sound manager, or throw.
        void onVoicePacket(std::uint32_t peer, std::string_view data);

        bool ensureCodec();
        void serviceDeviceRequest();
        // Both take the clear signal as an argument rather than reading it for
        // themselves, because a shared member consumed twice is only seen by
        // whichever pass runs first - and the one that missed it would go on
        // holding the stale handles this exists to drop.
        void serviceLoopback(bool cleared);
        void serviceCapture();
        // Main thread. Admits speakers the handler discovered, attaches what has
        // no stream, retires what has gone quiet.
        void serviceSpeakers(bool cleared);
        // Main thread. Drains what scripts asked for and moves streams to match.
        void applyAttachRequests();
        void detachSpeaker(std::uint32_t id, Speaker& speaker);
        void dropSpeakers();
        bool captureWanted() const;
        bool startCapture();
        void stopCapture();
        void attach(const MWWorld::Ptr& player);
        void detach();
        // Give up the handle without touching the sound manager, for the cases
        // where it has already thrown the attachment away underneath us.
        void forget();
        void refreshStats();
        void logLatency();

        SoundManager& mSounds;
        // Only ever reassigned by setSession, which joins the capture poll
        // thread first, so the plain read on that thread cannot race the write.
        Net::Session* mSession = nullptr;

        // Declared before mCapture so that the poll thread, which reads both from
        // onCaptureFrame, is joined by ~VoiceCapture before either goes away.
        std::unique_ptr<VoiceEncoder> mEncoder;
        std::shared_ptr<NetworkVoiceDecoder> mDecoder;
        // A value rather than a pointer, so that a script thread asking for the
        // level or the status is never reading a pointer the main thread is
        // reassigning. It holds no device until open() and is cheap to keep.
        VoiceCapture mCapture;

        std::atomic<bool> mLoopbackWanted{ false };
        std::atomic<bool> mTransmitWanted{ false };
        std::atomic<float> mInputGain{ 1.0f };
        // Published by update(), read by the capture poll thread, so that the
        // send path never has to ask the session anything: the poll thread is
        // the one thread whose deadline is a hardware ring filling up.
        std::atomic<bool> mLoopbackLive{ false };
        std::atomic<bool> mSessionLive{ false };

        mutable std::mutex mRequestMutex;
        std::string mRequestedDevice;
        bool mDeviceRequestPending = false;
        // Body assignments a script has asked for but the main thread has not
        // applied yet. An empty Ptr means detach. Keyed by speaker, so a mod
        // that re-points the same speaker twice before a frame runs only causes
        // the last one to happen, which is the intent.
        struct AttachRequest
        {
            MWWorld::Ptr mBody; // empty means detach
            VoiceStreamParams mParams;
        };
        std::map<std::uint32_t, AttachRequest> mAttachRequests;

        mutable std::mutex mStatsMutex;
        VoipStats mStats;
        std::atomic<std::uint64_t> mSentPackets{ 0 };
        std::atomic<std::uint64_t> mNetPackets{ 0 };
        // Counted on the capture thread, so an encoder that suddenly produced
        // something too big for the wire is visible instead of silent.
        std::atomic<std::uint64_t> mOversizeFrames{ 0 };

        // Capture poll thread only, so no lock: the thread is started after they
        // are set up and joined before they are torn down.
        std::array<std::int16_t, sVoiceFrameSamples> mGainScratch{};
        std::array<unsigned char, sVoiceMaxPacketBytes> mPacket{};
        std::uint16_t mSeq = 0;

        // Pump thread, which is the main thread: written by onVoicePacket and
        // read by the pass that runs later in the same frame.
        std::map<std::uint32_t, Speaker> mSpeakers;

        // Which body belongs to which speaker, remembered whether or not that
        // speaker has ever been heard. It has to outlive the speaker table
        // because the two arrive in either order and usually in this one: the
        // mod knows a peer's puppet from their first position update, which
        // precedes their first spoken word by however long they stay quiet.
        // Applying a body only to a speaker that already exists would drop
        // exactly the common case on the floor.
        std::map<std::uint32_t, AttachRequest> mSpeakerBodies;
        // Ids the handler heard and had nowhere to put. Creating a decoder there
        // would be ~30 KB of allocation at a rate a remote peer chooses, so the
        // handler only writes the id down and serviceSpeakers() decides.
        std::vector<std::uint32_t> mPending;
        std::uint64_t mReceivedPackets = 0;
        std::uint64_t mDroppedMalformed = 0;
        std::uint64_t mDroppedVersion = 0;
        std::uint64_t mDroppedOversize = 0;
        std::uint64_t mRefusedSpeakers = 0;
        std::uint64_t mSpoofedOrigin = 0;
        std::uint32_t mLastSpeaker = 0;
        bool mHeardAnyone = false;
        // Which end of the wire this machine is, refreshed once a frame by
        // serviceSpeakers from the snapshot it already asks the session for. It
        // decides whose word the packet handler takes for a speaker's identity,
        // and it is plain state rather than an atomic because the handler runs
        // on the main thread too, later in the same frame that wrote it.
        bool mIsHost = false;

        // Main thread only.
        std::string mOpenDevice;
        MWWorld::ConstPtr mAttachedPtr;
        // The sound manager's clear counter as of the last frame. A new game or
        // a save load finishes every voice stream and hands the Stream objects
        // back to the pool without saying so, and nothing else about it is
        // observable - the pool recycles rather than frees, and the player
        // reference a stream was keyed by is a by-value member whose address
        // never changes - so this counter is the whole of the signal.
        std::uint64_t mSoundsGeneration = 0;
        // Borrowed from the sound manager. Only ever read on the main thread and
        // only for getTrackTimeDelay. It has to be dropped the moment the
        // counter above moves: the pool hands the Stream straight to the next
        // caller, so a stale handle does not read as a zero delay, it reads as
        // some other sound's queue depth.
        Stream* mStream = nullptr;
        bool mAttached = false;
        bool mAttachFailed = false;
        bool mCaptureRunning = false;
        bool mCaptureServiced = false;
        bool mCaptureFailed = false;
        bool mLoopbackServiced = false;
        bool mSpeaking = false;
        float mLogTimer = 0.0f;
    };
}

#endif // GAME_SOUND_VOIPMANAGER_H
