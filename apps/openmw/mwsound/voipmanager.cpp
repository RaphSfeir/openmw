#include "voipmanager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/net/session.hpp>
#include <components/voip/wire.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/world.hpp"

#include "networkvoicedecoder.hpp"
#include "soundmanagerimp.hpp"

namespace MWSound
{
    namespace
    {
        // Anything past this is a broken slider rather than a loud room: the
        // encoder is handed clipped samples long before it buys any loudness.
        constexpr float sMaxInputGain = 4.0f;

        constexpr float sLogIntervalSeconds = 1.0f;

        // Half a second of speech at the 50 packets a second voice runs at.
        // Past it the transport drops the OLDEST message queued on the voice
        // channel: a pump stalled behind a loading screen has to resume the
        // conversation, not replay it from where it stopped.
        constexpr std::size_t sVoiceSendLimit = 25;

        // A client cannot learn that a speaker left. The transport attributes
        // every message it receives to peer 0 and keeps no peer table at all
        // unless it is hosting, so silence is the only departure signal there
        // is. Long enough not to mistake a pause for a disconnect.
        constexpr std::chrono::milliseconds sSpeakerTimeout{ 3000 };

        // How long after a speaker's last packet their mouth stays open.
        //
        // Packets arrive every 20 ms while somebody holds the key and stop
        // dead when they let go -- DTX is off, so there is no comfort-noise
        // trickle to confuse this. Long enough to ride out ordinary loss and
        // jitter, short enough that the mouth shuts promptly. It may safely be
        // a little generous at both ends: sayActive only says WHETHER to run
        // the talk animation, and the loudness the animation is scaled by is
        // zero through the silence either side, so an early or late edge costs
        // nothing visible.
        constexpr std::chrono::milliseconds sSpeakerSpurtGap{ 200 };

        // Ids the packet handler may remember before the main thread has had a
        // chance to decide about them. A remote peer picks the ids, so this is
        // the bound on what it can make this machine remember for one frame.
        constexpr std::size_t sMaxPendingSpeakers = 8;

        // Enough for a party several times over. Speakers are keyed by their own
        // id now rather than by an in-world reference, so this is a bound on
        // resources - a decoder and an AL source each - and not, as it briefly
        // was, a bound of one imposed by two speakers colliding on the same key.
        constexpr std::size_t sMaxRemoteSpeakers = 8;

        std::string_view describeStatus(VoiceCaptureStatus status)
        {
            switch (status)
            {
                case VoiceCaptureStatus::Closed:
                    return "closed";
                case VoiceCaptureStatus::Ok:
                    return "ok";
                case VoiceCaptureStatus::Silent:
                    return "silent";
                case VoiceCaptureStatus::Failed:
                    return "failed";
                case VoiceCaptureStatus::Disconnected:
                    return "disconnected";
            }
            return "closed";
        }

        std::string_view describeDevice(const std::string& name)
        {
            return name.empty() ? std::string_view("(default)") : std::string_view(name);
        }

        // Empty while there is no game, and that is all it is good for. It is
        // NOT a save-load detector: the reference it returns is a by-value
        // member of MWWorld::Player, constructed once per process, so its
        // address is the same before and after a load - and a successful load
        // finishes inside one StateManager::update, so no frame ever sees the
        // State_NoGame it passes through. Only the sound manager's clear
        // counter says an attachment was thrown away.
        MWWorld::Ptr currentPlayer()
        {
            if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame)
                return {};
            return MWBase::Environment::get().getWorld()->getPlayerPtr();
        }
    }

    VoipManager::VoipManager(SoundManager& sounds)
        : mSounds(sounds)
    {
        // Where the counter already stands, not zero: a clear that happened
        // before this object existed took no attachment of ours with it, and
        // starting behind it would announce a reset on the very first frame.
        mSoundsGeneration = mSounds.clearGeneration();
    }

    VoipManager::~VoipManager()
    {
        clear();
    }

    void VoipManager::setSession(Net::Session* session)
    {
        // MAIN THREAD, which is also the pump thread, and outside pump() itself.
        // Both halves matter: a channel handler may only be claimed and released
        // from the thread that runs it, and releasing it from inside the pump
        // that is running it would be destroying the callable mid-call.
        if (mSession == session)
            return;

        // The capture poll thread reads mSession with no lock. That is only
        // sound because this joins it before the pointer moves, so there is
        // never a send in flight through a pointer being reassigned.
        stopCapture();

        if (mSession != nullptr)
        {
            mSession->setChannelHandler(Voip::sVoiceChannel, nullptr);
            dropSpeakers();
        }

        mSession = session;
        mSessionLive.store(false, std::memory_order_relaxed);
        // Until the first serviceSpeakers pass says otherwise. Guessing Host
        // here would credit a client's first frames to peer 0 - the id a listen
        // host's own voice would carry - and the two are not the same speaker.
        mIsHost = false;

        if (mSession == nullptr)
            return;

        mSession->setChannelSendLimit(Voip::sVoiceChannel, sVoiceSendLimit);
        mSession->setChannelHandler(
            Voip::sVoiceChannel, [this](std::uint32_t peer, std::string_view data) { onVoicePacket(peer, data); });
    }

    void VoipManager::setLoopback(bool enabled)
    {
        mLoopbackWanted.store(enabled, std::memory_order_relaxed);
    }

    void VoipManager::setTransmit(bool transmit)
    {
        mTransmitWanted.store(transmit, std::memory_order_relaxed);
    }

    void VoipManager::setCaptureDevice(const std::string& name)
    {
        const std::lock_guard<std::mutex> lock(mRequestMutex);
        mRequestedDevice = name;
        mDeviceRequestPending = true;
    }

    void VoipManager::requestAttach(std::uint32_t speakerId, const MWWorld::Ptr& body, const VoiceStreamParams& params)
    {
        const std::lock_guard<std::mutex> lock(mRequestMutex);
        mAttachRequests[speakerId] = AttachRequest{ body, params };
    }

    void VoipManager::requestDetach(std::uint32_t speakerId)
    {
        // An empty Ptr is the detach: the request map holds one entry per
        // speaker, so a detach must be able to overwrite a queued attach that
        // has not been applied yet rather than sit behind it.
        const std::lock_guard<std::mutex> lock(mRequestMutex);
        mAttachRequests[speakerId] = AttachRequest{};
    }

    std::string VoipManager::captureDevice() const
    {
        const std::lock_guard<std::mutex> lock(mRequestMutex);
        return mRequestedDevice;
    }

    std::vector<std::string> VoipManager::captureDevices()
    {
        return VoiceCapture::enumerateDevices();
    }

    std::string_view VoipManager::micStatusName() const
    {
        return describeStatus(mCapture.status());
    }

    void VoipManager::setInputGain(float gain)
    {
        mInputGain.store(std::clamp(gain, 0.0f, sMaxInputGain), std::memory_order_relaxed);
    }

    VoipStats VoipManager::stats() const
    {
        const std::lock_guard<std::mutex> lock(mStatsMutex);
        return mStats;
    }

    bool VoipManager::ensureCodec()
    {
        if (mEncoder != nullptr && mDecoder != nullptr)
            return true;

        try
        {
            if (mEncoder == nullptr)
                mEncoder = std::make_unique<VoiceEncoder>();
            if (mDecoder == nullptr)
                mDecoder = std::make_shared<NetworkVoiceDecoder>();
        }
        catch (const std::exception& e)
        {
            // A machine whose Opus refuses to start is a machine without voice,
            // not a machine that fails to run, so this is reported and dropped.
            Log(Debug::Error) << "[voip] codec unavailable: " << e.what();
            mEncoder = nullptr;
            mDecoder = nullptr;
            return false;
        }

        return true;
    }

    void VoipManager::onCaptureFrame(const std::int16_t* pcm, std::size_t samples)
    {
        // Capture poll thread. Everything here has to stay off the main thread
        // and out of the sound manager, because Engine::frame stops for the whole
        // of a loading screen and this path must not stop with it. The one call
        // out is wakeStreamThread, which takes no lock on purpose.
        if (samples != static_cast<std::size_t>(sVoiceFrameSamples))
            return;

        const std::int16_t* input = pcm;
        const float gain = mInputGain.load(std::memory_order_relaxed);
        if (gain != 1.0f)
        {
            for (std::size_t i = 0; i < samples; ++i)
            {
                const float scaled = static_cast<float>(pcm[i]) * gain;
                mGainScratch[i] = static_cast<std::int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
            }
            input = mGainScratch.data();
        }

        // Both codec objects are read here without a lock. startCapture builds
        // them before it starts this thread and ensureCodec only ever assigns
        // them while they are null, so nothing reassigns them underneath a
        // running poll thread; clear() drops them only after joining it.
        const std::size_t bytes = mEncoder->encode(input, mPacket.data(), mPacket.size());
        if (bytes == 0)
            return;

        // The sequence number is not spent on a frame Opus declined to encode:
        // a gap in the numbering is indistinguishable downstream from a packet
        // that was lost, and would be counted as one.
        const std::uint16_t seq = mSeq++;
        mSentPackets.fetch_add(1, std::memory_order_relaxed);

        // The wire first: the far end is the one with a deadline, and the
        // loopback push below is a drill.
        sendVoiceFrame(mPacket.data(), bytes, seq);

        if (!mLoopbackLive.load(std::memory_order_relaxed))
            return;

        mDecoder->pushPacket(seq, mPacket.data(), bytes, false);
        mSounds.wakeStreamThread();
    }

    void VoipManager::sendVoiceFrame(const unsigned char* payload, std::size_t bytes, std::uint16_t seq)
    {
        // CAPTURE POLL THREAD, deliberately and not as an optimisation.
        // Session::send takes nothing but a queue mutex - every method except
        // pump() is callable off the main thread - so speech keeps leaving this
        // machine through a loading screen, during which Engine::frame and
        // therefore the whole main-thread half of this class does not run.
        Net::Session* session = mSession;
        if (session == nullptr || !mSessionLive.load(std::memory_order_relaxed))
            return;
        if (!mTransmitWanted.load(std::memory_order_relaxed))
            return;

        if (bytes == 0 || bytes > Voip::sMaxPacketBytes - Voip::sHeaderBytes)
        {
            // The clamp is a correctness bound, not a bandwidth one: ENet reads
            // the payload length before it looks at the flags, and an
            // unsequenced datagram past its fragment threshold silently becomes
            // a reliable, ordered, retransmitted fragment set - the one
            // delivery class voice picked Unsequenced to get away from.
            mOversizeFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // One allocation per 20 ms frame, and the header is written straight
        // into it rather than into scratch and copied. The speaker id stays 0:
        // only the relay may say who spoke, which is the whole of the anti-spoof
        // design and costs nothing here.
        std::string datagram(Voip::sHeaderBytes + bytes, '\0');
        unsigned char* raw = reinterpret_cast<unsigned char*>(datagram.data());
        Voip::Header header;
        header.mSeq = seq;
        Voip::writeHeader(raw, header);
        std::memcpy(raw + Voip::sHeaderBytes, payload, bytes);

        // Addressed to the server, which as a client means the destination is
        // ignored entirely. As a HOST there is no peer 0 - ids are handed out
        // from 1 - so a listen host's own voice is dropped by the transport,
        // which is the honest outcome while the host relays nothing.
        session->send(
            Net::Session::sServerPeerId, Voip::sVoiceChannel, Net::Delivery::Unsequenced, std::move(datagram));
        mNetPackets.fetch_add(1, std::memory_order_relaxed);
    }

    void VoipManager::onVoicePacket(std::uint32_t peer, std::string_view data)
    {
        // PUMP THREAD, which is the main thread, from inside
        // LuaManager::synchronizedUpdate and therefore from inside sol's
        // protected call: an exception escaping here would unwind through Lua's
        // own machinery, so nothing may leave this function. Nothing here
        // allocates per packet or reaches into the sound manager either -
        // pushPacket copies into a ring the attach pass already allocated, and
        // wakeStreamThread takes no lock by design.
        try
        {
            ++mReceivedPackets;

            const unsigned char* raw = reinterpret_cast<const unsigned char*>(data.data());
            Voip::Header header;
            if (!Voip::readHeader(raw, data.size(), header))
            {
                ++mDroppedMalformed;
                return;
            }
            if (header.mVersion != Voip::sWireVersion)
            {
                // Unknown FLAG bits are passed through and ignored, per the
                // convention that a newer peer talking about things this build
                // has never heard of is harmless. An unknown VERSION is the one
                // thing that makes the rest of the bytes unreadable.
                ++mDroppedVersion;
                return;
            }
            if (data.size() > Voip::sMaxPacketBytes)
            {
                ++mDroppedOversize;
                return;
            }

            // Who spoke, taken from whichever end is entitled to say so. A
            // CLIENT has no peer information at all - the transport attributes
            // every message it receives to peer 0 and keeps no peer table - so
            // the only origin available there is the one the server's relay
            // stamped into the header. A HOST is handed the true id by the
            // transport, straight off the connection the bytes arrived on, and
            // ignores the header entirely: no relay runs on this end yet, so
            // every honest sender leaves that field at 0, and anything else in
            // it is a claim to an origin the sender does not own. The M3
            // listen-host relay will stamp the same id this picks, so nothing
            // here changes when it lands.
            const std::uint32_t origin = mIsHost ? peer : header.mSpeaker;
            if (mIsHost && header.mSpeaker != 0)
            {
                // Counted, not dropped. The transport's id is authoritative, so
                // the frame is playable under it; refusing it would only teach
                // a future protocol revision that this build is incompatible.
                ++mSpoofedOrigin;
            }
            mLastSpeaker = origin;
            mHeardAnyone = true;

            const auto it = mSpeakers.find(origin);
            if (it == mSpeakers.end())
            {
                // No decoder is built here. opus_decoder_create plus the ring is
                // tens of kilobytes, at a rate and a count a remote peer
                // chooses; the id is written down instead and serviceSpeakers
                // decides about it later in this same frame.
                if (mPending.size() < sMaxPendingSpeakers
                    && std::find(mPending.begin(), mPending.end(), origin) == mPending.end())
                    mPending.push_back(origin);
                return;
            }

            Speaker& speaker = it->second;
            ++speaker.mReceived;
            speaker.mLastPacket = std::chrono::steady_clock::now();
            speaker.mDecoder->pushPacket(header.mSeq, raw + Voip::sHeaderBytes, data.size() - Voip::sHeaderBytes,
                (header.mFlags & Voip::FlagEndOfSpurt) != 0);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[voip] dropped a voice packet: " << e.what();
            return;
        }

        mSounds.wakeStreamThread();
    }

    bool VoipManager::captureWanted() const
    {
        // Two independent consumers, and either alone is reason enough to hold
        // the endpoint open: the loopback drill needs a stream to feed, the
        // network path needs only a live session. Transmit is deliberately NOT
        // part of this - VoiceCapture keeps draining the device while its gate
        // is shut, which is what stops the driver's ring overrunning and stale
        // audio surfacing on the next press, and what keeps the first syllable
        // of a push-to-talk press from being spent opening a device.
        //
        // The cost is that the Windows in-use indicator stays lit for a
        // multiplayer session. Closing it for a player who does not want voice
        // at all is what voip.setMode('off') is for, and that is M6.
        if (mLoopbackWanted.load(std::memory_order_relaxed) && mAttached)
            return true;
        return mSession != nullptr && mSessionLive.load(std::memory_order_relaxed);
    }

    void VoipManager::serviceCapture()
    {
        const bool wanted = captureWanted();
        if (wanted != mCaptureServiced)
        {
            mCaptureServiced = wanted;
            // Wanting it again is as close as this gets to being told the
            // reason it failed last time may have gone away.
            mCaptureFailed = false;
        }

        if (wanted && !mCaptureRunning && !mCaptureFailed)
        {
            if (startCapture())
                Log(Debug::Info) << "[voip] capture running on \"" << describeDevice(mOpenDevice) << "\", status "
                                 << micStatusName();
            else
            {
                // Latched, because retrying an endpoint another application is
                // holding would charge the rest of the session a device open
                // per frame for a condition that does not fix itself.
                mCaptureFailed = true;
                Log(Debug::Warning) << "[voip] the microphone did not start, status " << micStatusName();
            }
        }
        else if (!wanted && mCaptureRunning)
            stopCapture();

        mCapture.setTransmitting(mCaptureRunning && mTransmitWanted.load(std::memory_order_relaxed));
    }

    bool VoipManager::startCapture()
    {
        // Before the thread and never after: the poll thread reads both codec
        // pointers with no lock, and this is where they are made to exist for
        // as long as it runs.
        if (!ensureCodec())
            return false;

        std::string wanted;
        {
            const std::lock_guard<std::mutex> lock(mRequestMutex);
            wanted = mRequestedDevice;
            mDeviceRequestPending = false;
        }

        if (!mCapture.open(wanted))
            return false;

        mOpenDevice = wanted;
        mCapture.setTransmitting(mTransmitWanted.load(std::memory_order_relaxed));
        mCapture.start([this](const std::int16_t* pcm, std::size_t samples) { onCaptureFrame(pcm, samples); });
        if (mCapture.status() == VoiceCaptureStatus::Failed)
        {
            // Left open rather than closed: closing resets the status to Closed
            // and would throw away the only evidence the player has that another
            // application is holding the endpoint.
            return false;
        }

        mCaptureRunning = true;
        return true;
    }

    void VoipManager::stopCapture()
    {
        // Joins the poll thread, so once this returns nothing is pushing packets
        // any more and the codec objects belong to the main thread again.
        mCapture.stop();
        mCapture.close();
        mCaptureRunning = false;
    }

    void VoipManager::serviceDeviceRequest()
    {
        std::string wanted;
        {
            const std::lock_guard<std::mutex> lock(mRequestMutex);
            if (!mDeviceRequestPending)
                return;
            mDeviceRequestPending = false;
            wanted = mRequestedDevice;
        }

        // A device nobody has opened yet needs no reopening: whenever capture
        // does start, it reads the name for itself.
        mCaptureFailed = false;
        if (!mCaptureRunning || wanted == mOpenDevice)
            return;

        // openal-soft does not migrate an open capture endpoint and its reopen
        // extension is playback-only, so changing device is a close and a fresh
        // open and there is no gentler way to do it.
        Log(Debug::Info) << "[voip] switching capture device to \"" << describeDevice(wanted) << "\"";
        stopCapture();
        if (!startCapture())
        {
            mCaptureFailed = true;
            Log(Debug::Warning) << "[voip] could not open capture device \"" << describeDevice(wanted) << "\"";
        }
    }

    void VoipManager::attach(const MWWorld::Ptr& player)
    {
        if (!ensureCodec())
        {
            mAttachFailed = true;
            return;
        }

        // A fresh attachment starts from an empty buffer: whatever is left in it
        // belongs to a spurt nobody is listening to any more.
        mDecoder->resetBuffer();

        mStream = mSounds.playVoiceStream(player, mDecoder, VoiceStreamParams{});
        if (mStream == nullptr)
        {
            // No output device, or no free source. Retrying every frame would
            // charge the rest of the session a stream allocation attempt per
            // frame for a condition that does not fix itself.
            Log(Debug::Warning) << "[voip] could not attach the loopback voice stream";
            mAttachFailed = true;
            return;
        }

        mAttachedPtr = MWWorld::ConstPtr(player);
        mAttached = true;
        mLoopbackLive.store(true, std::memory_order_relaxed);

        Log(Debug::Info) << "[voip] loopback attached to the player";
    }

    void VoipManager::detach()
    {
        // Before the stream goes, so the capture thread has already stopped
        // pushing into a decoder nothing is reading by the time it does.
        mLoopbackLive.store(false, std::memory_order_relaxed);

        if (mAttached)
        {
            // Keyed by reference, and the sound manager only ever compares the
            // pointer, so a reference that has since been replaced finds nothing
            // rather than being dereferenced.
            mSounds.setVoiceSpeaking(mAttachedPtr, false);
            mSounds.stopVoiceStream(mAttachedPtr);
            Log(Debug::Info) << "[voip] loopback detached";
        }

        forget();
    }

    void VoipManager::forget()
    {
        mStream = nullptr;
        mAttachedPtr = MWWorld::ConstPtr();
        mAttached = false;
        mSpeaking = false;
    }

    void VoipManager::serviceLoopback(bool cleared)
    {
        const bool wanted = mLoopbackWanted.load(std::memory_order_relaxed);
        if (wanted != mLoopbackServiced)
        {
            mLoopbackServiced = wanted;
            // Asking again is the player's way of saying the reason it failed
            // last time may have gone away.
            mAttachFailed = false;
        }

        const MWWorld::Ptr player = currentPlayer();

        if (mAttached && (cleared || player.isEmpty()))
        {
            // A new game or a loaded save takes every voice attachment with it
            // (SoundManager::clear), and the handle held here went with it.
            // Dropping it without calling back into the sound manager is the
            // only correct move, because the entry is already gone. The empty
            // player is a second case and not the same one: a load that failed
            // leaves State_NoGame standing across frames, with nothing to
            // attach to and no clear left to notice.
            mLoopbackLive.store(false, std::memory_order_relaxed);
            forget();
            mAttachFailed = false;
        }

        if (wanted && !mAttached && !mAttachFailed && !player.isEmpty())
            attach(player);
        else if (!wanted && mAttached)
            detach();

        if (!mAttached)
            return;

        const bool transmit = mTransmitWanted.load(std::memory_order_relaxed);
        if (transmit != mSpeaking)
        {
            // What drives the mouth. The attachment is persistent and mostly
            // quiet, so the sound manager has to be told about the spurt rather
            // than infer it from the stream existing.
            mSpeaking = transmit;
            mSounds.setVoiceSpeaking(mAttachedPtr, transmit);
        }
    }

    void VoipManager::detachSpeaker(std::uint32_t id, Speaker& speaker)
    {
        if (speaker.mStream == nullptr)
        {
            speaker.mBody = MWWorld::Ptr();
            return;
        }

        // The mouth belongs to the stream, so a stream that is going takes the
        // flag with it: a rebuilt one starts closed and the next pass reopens
        // it if the speaker is still talking.
        speaker.mSpeaking = false;

        // Whichever kind of stream this speaker has. The two are different
        // containers in the sound manager, keyed differently, and stopping the
        // wrong one silently leaves the real stream running.
        if (speaker.mBody.isEmpty())
            mSounds.stopVoiceTrack(id);
        else
            mSounds.stopVoiceStream(speaker.mBody);

        // Only now may the last reference to the decoder be dropped: finishStream
        // waits on the mutex the stream thread holds across a whole decode pass,
        // so its return is the proof that read() is not running.
        speaker.mStream = nullptr;
        speaker.mBody = MWWorld::Ptr();
    }

    void VoipManager::applyAttachRequests()
    {
        // MAIN THREAD. Scripts only ever queue; this is the one place a stream
        // moves, because attaching touches the sound manager.
        {
            std::map<std::uint32_t, AttachRequest> requests;
            {
                const std::lock_guard<std::mutex> lock(mRequestMutex);
                requests.swap(mAttachRequests);
            }
            for (const auto& [id, request] : requests)
            {
                if (request.mBody.isEmpty())
                    mSpeakerBodies.erase(id);
                else
                    mSpeakerBodies[id] = request;
            }
        }

        // Sync every speaker against what is currently known, rather than only
        // reacting to what just arrived: a body remembered before its speaker
        // was ever heard has to take effect when that speaker finally appears,
        // which is the ordinary order of events.
        for (auto& [id, speaker] : mSpeakers)
        {
            const auto known = mSpeakerBodies.find(id);
            const MWWorld::Ptr wanted = known == mSpeakerBodies.end() ? MWWorld::Ptr() : known->second.mBody;
            const VoiceStreamParams wantedParams
                = known == mSpeakerBodies.end() ? VoiceStreamParams{} : known->second.mParams;

            // Volume, unlike geometry, is live. updateStream feeds the sound's
            // own volume to AL_GAIN every frame, so turning one person down
            // takes effect mid-sentence and costs nothing -- no rebuild, no
            // gap, no reallocated source. Done before the geometry test so it
            // still happens on the overwhelmingly common path where nothing
            // else about the speaker has changed.
            if (speaker.mStream != nullptr && wantedParams.mGain != speaker.mStreamParams.mGain)
            {
                mSounds.setVoiceGain(speaker.mStream, wantedParams.mGain);
                speaker.mStreamParams.mGain = wantedParams.mGain;
            }

            // A live stream whose GEOMETRY has changed has to be rebuilt even
            // though its body has not moved: reference distance, max distance
            // and rolloff are written to the AL source only by initCommon3D,
            // and nothing refreshes them afterwards. Without this, a player
            // dragging the hearing sliders in the settings menu changes what
            // the next stream will sound like and not what they are listening
            // to, which is precisely backwards from what a slider is for.
            const bool geometryStale = speaker.mStream != nullptr && !wantedParams.sameGeometry(speaker.mStreamParams);

            if (wanted.isEmpty() && speaker.mBody.isEmpty() && !geometryStale)
                continue;
            if (!wanted.isEmpty() && !speaker.mBody.isEmpty() && wanted == speaker.mBody && !geometryStale)
                continue;

            // Stop first, start second, and never the other way round. The
            // decoder is shared, and it is what carries the Opus state and the
            // jitter buffer across the move: finishStream's return is the proof
            // that the stream thread has let go of it, so starting the new
            // stream before stopping the old one would have two streams pulling
            // one decoder. The decoder itself is never reset here -- that is
            // the whole point of moving the stream rather than the speaker.
            detachSpeaker(id, speaker);
            speaker.mAttachFailed = false;
            speaker.mBody = wanted;
        }
    }

    void VoipManager::dropSpeakers()
    {
        for (auto& [id, speaker] : mSpeakers)
            detachSpeaker(id, speaker);
        mSpeakers.clear();
        mPending.clear();
        // The bodies go too: they belong to a session's puppets, and holding
        // references to objects from a world that has been torn down is how a
        // reconnect ends up attaching a voice to nothing.
        mSpeakerBodies.clear();
        {
            const std::lock_guard<std::mutex> lock(mRequestMutex);
            mAttachRequests.clear();
        }
    }

    void VoipManager::serviceSpeakers(bool cleared)
    {
        // MAIN THREAD - and the same thread onVoicePacket ran on a dozen lines
        // earlier this frame, which is why none of this needs a lock and why a
        // packet cannot arrive in the middle of the pass. It also means a
        // speaker retired here cannot be resurrected by a packet already in
        // flight: anything that arrived this frame has already refreshed the
        // timestamp the retirement is judged on.
        const bool live = mSession != nullptr && mSession->isConnected();
        // The role is read here and nowhere else, on the same session snapshot
        // isConnected has just taken the lock for. The packet handler must not
        // ask for it itself: getRole takes the mutex Session::send takes from
        // the capture poll thread, fifty times a second.
        const bool host = live && mSession->getRole() == Net::Role::Host;
        mSessionLive.store(live, std::memory_order_relaxed);

        if (!live || host != mIsHost)
        {
            // The two roles number speakers out of different keyspaces - a
            // client credits whatever the relay stamped, a host credits the
            // transport's peer id, and id 0 means "the server" to one and
            // "unstamped" to the other - so nothing minted under the old role
            // may survive the flip into the new one.
            mIsHost = host;
            dropSpeakers();
            if (!live)
                return;
        }

        if (cleared)
        {
            // SoundManager::clear went through. Every stream was finished and
            // handed back to the pool, so the borrowed handles are stale and the
            // map entries they were keyed by are gone; they are dropped and
            // attached again below rather than stopped. The ring goes with them,
            // exactly as a fresh attach() would reset it: what accumulated in it
            // while nobody was reading belongs to a spurt already over.
            for (auto& [id, speaker] : mSpeakers)
            {
                speaker.mStream = nullptr;
                speaker.mAttachFailed = false;
                speaker.mDecoder->resetBuffer();
            }
        }

        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        for (auto it = mSpeakers.begin(); it != mSpeakers.end();)
        {
            if (now - it->second.mLastPacket < sSpeakerTimeout)
            {
                ++it;
                continue;
            }
            Log(Debug::Info) << "[voip] speaker " << it->first << " went quiet, releasing the stream";
            detachSpeaker(it->first, it->second);
            it = mSpeakers.erase(it);
        }

        for (const std::uint32_t id : mPending)
        {
            if (mSpeakers.find(id) != mSpeakers.end())
                continue;
            if (mSpeakers.size() >= sMaxRemoteSpeakers)
            {
                if (mRefusedSpeakers == 0)
                    Log(Debug::Warning) << "[voip] speaker " << id << " refused: only " << sMaxRemoteSpeakers
                                        << " remote speaker(s) can be attached until voice has a puppet to sit on";
                ++mRefusedSpeakers;
                continue;
            }

            Speaker speaker;
            try
            {
                speaker.mDecoder = std::make_shared<NetworkVoiceDecoder>();
            }
            catch (const std::exception& e)
            {
                // A machine whose Opus refuses to start is a machine without
                // voice, not one that fails to run.
                Log(Debug::Error) << "[voip] no decoder for speaker " << id << ": " << e.what();
                ++mRefusedSpeakers;
                continue;
            }
            speaker.mLastPacket = now;
            mSpeakers.emplace(id, std::move(speaker));
            Log(Debug::Info) << "[voip] hearing speaker " << id;
        }
        mPending.clear();

        // After the newly heard speakers exist, so a body assigned in the same
        // frame as a speaker's first packet lands on them rather than being
        // dropped for want of a speaker; and before the attach loop, so a
        // speaker whose body just changed is re-attached in this pass instead
        // of spending a frame silent.
        applyAttachRequests();

        for (auto& [id, speaker] : mSpeakers)
        {
            if (speaker.mStream != nullptr || speaker.mAttachFailed)
                continue;

            // With a body, the voice comes out of the body: head tracking,
            // distance attenuation, the min-gain floor and lip sync all follow
            // from playVoiceStream. Without one it stays flat, which is what a
            // speaker sounds like before the mod has said which puppet is
            // theirs, and what they fall back to if that body goes away.
            // Recorded whichever branch runs, because the sync loop compares
            // against it to decide whether a live stream owes a rebuild. A
            // bodyless track has no geometry to go stale, so the default is the
            // honest answer there.
            speaker.mStreamParams = VoiceStreamParams{};

            if (speaker.mBody.isEmpty())
            {
                speaker.mStream = mSounds.playVoiceTrack(id, speaker.mDecoder, VoiceStreamParams{});
            }
            else
            {
                // The distances the authority asked for, not the built-in ones:
                // how far a voice carries is a world rule, and a session where
                // each client decided it for itself would have people audible
                // to some of the party and not to others.
                const auto known = mSpeakerBodies.find(id);
                const VoiceStreamParams params
                    = known == mSpeakerBodies.end() ? VoiceStreamParams{} : known->second.mParams;
                speaker.mStreamParams = params;
                speaker.mStream = mSounds.playVoiceStream(speaker.mBody, speaker.mDecoder, params);
            }

            if (speaker.mStream == nullptr)
            {
                // No output device - a --no-sound client relays and receives but
                // cannot play - or no free source. Either way retrying every
                // frame buys nothing.
                speaker.mAttachFailed = true;
                Log(Debug::Warning) << "[voip] could not attach a stream for speaker " << id;
            }
            else if (!speaker.mBody.isEmpty())
            {
                Log(Debug::Info) << "[voip] speaker " << id << " now sounds from their body";
            }
        }

        // WHOSE MOUTH MOVES, and why it is here rather than beside the loopback
        // flag above: that one is driven by mTransmitWanted, which is THIS
        // machine's push-to-talk, and applies to the local player's own
        // attachment. A remote speaker has no such signal here -- their key is
        // on their keyboard -- so the only honest evidence that they are
        // talking is that their packets are still arriving.
        //
        // Without this, a remote speaker's mSpeaking stayed false for the life
        // of the attachment, sayActive answered no, and HeadAnimationTime took
        // the BLINK branch forever. Voice worked; nobody's mouth ever moved.
        const std::chrono::steady_clock::time_point mouthNow = std::chrono::steady_clock::now();
        for (auto& [id, speaker] : mSpeakers)
        {
            if (speaker.mStream == nullptr || speaker.mBody.isEmpty())
                continue;
            const bool speaking = mouthNow - speaker.mLastPacket < sSpeakerSpurtGap;
            if (speaking != speaker.mSpeaking)
            {
                speaker.mSpeaking = speaking;
                mSounds.setVoiceSpeaking(speaker.mBody, speaking);
            }
        }
    }

    void VoipManager::refreshStats()
    {
        VoipStats fresh;
        fresh.mSentPackets = mSentPackets.load(std::memory_order_relaxed);

        if (mDecoder != nullptr)
        {
            const Voip::JitterBufferStats buffer = mDecoder->stats();
            fresh.mJitterMs = mDecoder->bufferedDelayMs();
            fresh.mTargetFrames = buffer.mTargetFrames;
            fresh.mPlayed = buffer.mPlayed;
            fresh.mFecRecovered = buffer.mFecRecovered;
            fresh.mConcealed = buffer.mConcealed;
            fresh.mUnderruns = buffer.mUnderruns;
        }

        if (mAttached && mStream != nullptr)
        {
            // The AL side of mouth-to-ear: how much audio is queued ahead of the
            // play cursor. Read here and nowhere else, because this is the only
            // thread that can be sure nothing is detaching the stream.
            const double delay = mSounds.getTrackTimeDelay(mStream);
            fresh.mStreamDelayMs = static_cast<int>(std::lround(delay * 1000.0));
        }

        // The 20 ms the microphone spends filling a frame, and the device period
        // behind it, are invisible to both ends, so this total is the buffered
        // part of mouth-to-ear rather than the whole of it.
        fresh.mTotalMs = fresh.mJitterMs + fresh.mStreamDelayMs;

        fresh.mNetPackets = mNetPackets.load(std::memory_order_relaxed);
        fresh.mOversizeFrames = mOversizeFrames.load(std::memory_order_relaxed);
        fresh.mReceivedPackets = mReceivedPackets;
        fresh.mDroppedMalformed = mDroppedMalformed;
        fresh.mDroppedVersion = mDroppedVersion;
        fresh.mDroppedOversize = mDroppedOversize;
        fresh.mRefusedSpeakers = mRefusedSpeakers;
        fresh.mSpoofedOrigin = mSpoofedOrigin;
        fresh.mLastSpeaker = mLastSpeaker;
        fresh.mHeardAnyone = mHeardAnyone;
        if (mSession != nullptr)
            fresh.mDroppedSends = mSession->droppedSends(Voip::sVoiceChannel);

        fresh.mRemote.reserve(mSpeakers.size());
        for (const auto& [id, speaker] : mSpeakers)
        {
            const Voip::JitterBufferStats buffer = speaker.mDecoder->stats();
            VoipSpeakerStats row;
            row.mId = id;
            row.mJitterMs = speaker.mDecoder->bufferedDelayMs();
            row.mTargetFrames = buffer.mTargetFrames;
            row.mReceived = speaker.mReceived;
            row.mLate = buffer.mLate;
            row.mPlayed = buffer.mPlayed;
            row.mFecRecovered = buffer.mFecRecovered;
            row.mConcealed = buffer.mConcealed;
            row.mUnderruns = buffer.mUnderruns;
            row.mAttached = speaker.mStream != nullptr;
            if (speaker.mStream != nullptr)
                row.mStreamDelayMs = static_cast<int>(std::lround(mSounds.getTrackTimeDelay(speaker.mStream) * 1000.0));
            fresh.mRemote.push_back(row);
        }

        const std::lock_guard<std::mutex> lock(mStatsMutex);
        mStats = std::move(fresh);
    }

    std::vector<std::uint32_t> VoipManager::speakerIds() const
    {
        const std::lock_guard<std::mutex> lock(mStatsMutex);
        std::vector<std::uint32_t> ids;
        ids.reserve(mStats.mRemote.size());
        for (const VoipSpeakerStats& speaker : mStats.mRemote)
            ids.push_back(speaker.mId);
        return ids;
    }

    void VoipManager::logLatency()
    {
        const VoipStats snapshot = stats();
        Log(Debug::Info) << "[voip] mic=" << mCapture.level() << " status=" << micStatusName()
                         << " jitter=" << snapshot.mJitterMs << "ms stream=" << snapshot.mStreamDelayMs
                         << "ms total=" << snapshot.mTotalMs << "ms sent=" << snapshot.mSentPackets
                         << " played=" << snapshot.mPlayed << " fec=" << snapshot.mFecRecovered
                         << " plc=" << snapshot.mConcealed << " underruns=" << snapshot.mUnderruns
                         << " target=" << snapshot.mTargetFrames;

        if (!mSessionLive.load(std::memory_order_relaxed))
            return;

        Log(Debug::Info) << "[voip] net sent=" << snapshot.mNetPackets << " dropped=" << snapshot.mDroppedSends
                         << " recv=" << snapshot.mReceivedPackets << " bad=" << snapshot.mDroppedMalformed << "/"
                         << snapshot.mDroppedVersion << "/" << snapshot.mDroppedOversize
                         << " refused=" << snapshot.mRefusedSpeakers << " spoofed=" << snapshot.mSpoofedOrigin
                         << " speakers=" << snapshot.mRemote.size();
        for (const VoipSpeakerStats& speaker : snapshot.mRemote)
            Log(Debug::Info) << "[voip] speaker " << speaker.mId << " recv=" << speaker.mReceived
                             << " jitter=" << speaker.mJitterMs << "ms stream=" << speaker.mStreamDelayMs
                             << "ms played=" << speaker.mPlayed << " fec=" << speaker.mFecRecovered
                             << " plc=" << speaker.mConcealed << " late=" << speaker.mLate
                             << " underruns=" << speaker.mUnderruns << " target=" << speaker.mTargetFrames
                             << " attached=" << speaker.mAttached;
    }

    void VoipManager::update(float dt)
    {
        serviceDeviceRequest();

        // Latched once and handed to both passes below. Reading it inside each
        // of them instead would let whichever ran first consume the change, and
        // the other would keep the very handles this is here to drop.
        const std::uint64_t generation = mSounds.clearGeneration();
        const bool cleared = generation != mSoundsGeneration;
        mSoundsGeneration = generation;
        if (cleared && (mAttached || !mSpeakers.empty()))
        {
            // Said out loud, because the failure this replaces was silent: the
            // player stopped hearing anyone and nothing in the log connected it
            // to the save load that had just happened. Only when there was
            // something to lose - a new game logs once per new game otherwise,
            // at every player who has never touched voice.
            Log(Debug::Info) << "[voip] the sound manager was cleared, re-attaching " << mSpeakers.size()
                             << " speaker(s)";
        }

        serviceLoopback(cleared);
        // After the loopback pass, because it decides whether a stream exists to
        // hold the microphone open for, and before the capture pass, which acts
        // on the answer. Both are before the stats snapshot they feed.
        serviceSpeakers(cleared);
        serviceCapture();
        refreshStats();

        // Only while there is a voice path to measure. A line a second for every
        // session that never touches voice would bury the one that does.
        if (!mLoopbackServiced && mSpeakers.empty() && !mTransmitWanted.load(std::memory_order_relaxed))
        {
            mLogTimer = 0.0f;
            return;
        }

        mLogTimer += dt;
        if (mLogTimer < sLogIntervalSeconds)
            return;

        mLogTimer = 0.0f;
        logLatency();
    }

    void VoipManager::clear()
    {
        mLoopbackWanted.store(false, std::memory_order_relaxed);
        mLoopbackServiced = false;
        mSessionLive.store(false, std::memory_order_relaxed);

        // First, and everything else depends on it: this joins the poll thread,
        // so once it returns nothing is encoding, sending or pushing packets.
        stopCapture();
        mCaptureServiced = false;

        detach();
        dropSpeakers();
        // setSession below only reaches this when there is a session to release,
        // and the role has to be forgotten either way: whatever comes next is
        // not the session whose peer ids these were.
        mIsHost = false;

        // Releases the channel handler, which may only be released from the pump
        // thread - this is it - and only once nothing can still be sending
        // through the pointer, which the stopCapture above guarantees.
        setSession(nullptr);

        mDecoder = nullptr;
        mEncoder = nullptr;
    }
}
