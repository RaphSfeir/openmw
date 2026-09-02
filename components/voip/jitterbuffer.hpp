#ifndef OPENMW_COMPONENTS_VOIP_JITTERBUFFER_H
#define OPENMW_COMPONENTS_VOIP_JITTERBUFFER_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Voip
{
    // 20 ms frames everywhere. Mirrors MWSound::sVoiceFrameSamples at 48 kHz;
    // networkvoicedecoder.cpp static_asserts the two agree, because this header
    // must not reach into apps/ and must not know that Opus exists.
    inline constexpr int sFrameMs = 20;

    // Mirrors MWSound::sVoiceMaxPacketBytes, same reason.
    inline constexpr std::size_t sMaxPayloadBytes = 400;

    // 16 frames = 320 ms of reordering tolerance, and the modulus the ring is
    // indexed by. Fixed, so a speaker's whole buffer is one ~11 KB allocation
    // made at attach time: nothing on the audio thread ever allocates.
    inline constexpr std::size_t sCapacityFrames = 16;

    // seq % sCapacityFrames must name the same slot on both sides of the u16
    // wrap, which it only does when the capacity divides 65536 exactly.
    static_assert(sCapacityFrames > 0 && 65536 % sCapacityFrames == 0);

    // What the audio thread must do with this frame. There is deliberately no
    // "nothing available": refillQueue treats a short read as end of stream and
    // marks the stream finished permanently (openaloutput.cpp:633-637), so a
    // decoder that cannot answer kills the voice stream for good.
    enum class PopOutcome
    {
        // mData/mSize are this frame's own packet: VoiceDecoder::decode.
        Frame,
        // The frame never arrived but its successor is in hand. mData/mSize are
        // the SUCCESSOR's packet and mSeq is the missing frame's number:
        // VoiceDecoder::decodeFec rebuilds it from the redundancy that packet
        // carries.
        Fec,
        // Neither the frame nor its successor arrived, inside a live spurt.
        // VoiceDecoder::conceal. mData is null.
        Conceal,
        // Not a loss: the speaker is silent, or a spurt has begun and the buffer
        // is still filling to its target. Write zeroes. Concealing here is what
        // makes silence sound metallic, and it would charge quiet time to the
        // loss statistic.
        Silence,
    };

    struct PopResult
    {
        PopOutcome mOutcome = PopOutcome::Silence;
        // Borrowed from the buffer's own storage. Valid until the next push(),
        // pop() or reset() on this buffer - the slot is recycled then, not at
        // the moment it is handed out, because the audio thread's natural loop
        // is pop-then-decode-then-pop.
        const unsigned char* mData = nullptr;
        std::size_t mSize = 0;
        // The frame this decision is about, which for Fec is the frame that is
        // MISSING, not the packet sitting in mData.
        std::uint16_t mSeq = 0;
    };

    struct VoicePacket
    {
        std::uint16_t mSeq = 0;
        bool mEndOfSpurt = false;
        // Borrowed: push() copies these bytes into the ring, so the caller may
        // reuse its receive scratch immediately.
        const unsigned char* mData = nullptr;
        std::size_t mSize = 0;
    };

    struct JitterBufferSettings
    {
        int mMinTargetFrames = 2; // 40 ms
        int mMaxTargetFrames = 6; // 120 ms
        int mInitialTargetFrames = 3;
        // One frame beyond what measured jitter needs, so a missing frame's
        // successor is already in hand and its FEC copy is reachable. Without
        // it, in-band FEC is dead weight (voicecodec.hpp:85-88).
        int mFecFrames = 1;
        std::chrono::milliseconds mDeltaWindow{ 3000 };
        int mPercentile = 95;
        // Shrinking is slow and only between spurts: a target that tracks
        // improvement as eagerly as it tracks damage oscillates. Counted in
        // frames, not time, so pop() needs no clock.
        int mShrinkAfterSilentFrames = 250; // 5 s
        // After this many concealed frames in a row the caller has faded out,
        // so a spurt that is still starved at that point is treated as over
        // rather than concealed indefinitely.
        int mMaxConcealFrames = 5;
    };

    struct JitterBufferStats
    {
        int mTargetFrames = 0;
        // Playout cushion: the distance from the play cursor to the newest
        // packet held, so a hole inside the span still counts as depth.
        int mBufferedFrames = 0;
        int mJitterMs = 0; // the p95 the target was computed from
        // Every packet handed to push(), including those dropped below as late,
        // duplicate or malformed.
        std::uint64_t mReceived = 0;
        std::uint64_t mLate = 0; // arrived after their slot had played
        std::uint64_t mDuplicate = 0;
        std::uint64_t mPlayed = 0;
        std::uint64_t mFecRecovered = 0;
        std::uint64_t mConcealed = 0;
        std::uint64_t mSilence = 0;
        std::uint64_t mUnderruns = 0; // starvation episodes, not starved frames
        std::uint64_t mResyncs = 0;
    };

    // One speaker's worth of reordering and playout depth.
    //
    // NOT thread-safe, deliberately: push() runs on whichever thread takes
    // packets off the wire and pop() on the OpenAL stream thread, but the owner
    // (VoipManager) already holds a per-speaker lock across bigger regions than
    // this - mute, detach, decoder state - so a second lock in here would only
    // be a second chance to get the ordering wrong.
    //
    // Holds no clock. Time enters at exactly one place, the arrival stamp on
    // push(), and is used for exactly one thing, the inter-arrival statistic the
    // playout target is computed from. Everything the audio thread does is
    // counted in frames, so pop() takes no time argument: it can neither read
    // the clock nor be made to disagree with the AL queue that is actually
    // pacing it, and a test drives the whole state machine with integers.
    class JitterBuffer
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;

        explicit JitterBuffer(const JitterBufferSettings& settings = {});

        // Copies the payload into the ring. Late, duplicate, empty and oversized
        // packets are counted and dropped; a packet far enough ahead is treated
        // as the stream having jumped, and resyncs.
        void push(const VoicePacket& packet, TimePoint arrival);

        // Always returns a decision. Advances by exactly one frame, except on a
        // starved Conceal - see the note on that in the implementation.
        PopResult pop();

        // Detach, reconnect, speaker gone: returns the buffer to its constructed
        // state, statistics included. Does NOT imply resetting the Opus decoder:
        // VoiceDecoder is kept alive across spurts on purpose
        // (voicecodec.hpp:72-75) and throwing its state away is audible.
        void reset();

        int targetFrames() const { return mTargetFrames; }
        int bufferedFrames() const;
        // Consecutive Conceal outcomes so far, so the caller's fade-out does not
        // have to keep its own copy of this state.
        int concealRun() const { return mConcealRun; }
        // What this buffer contributes to mouth-to-ear right now. The Lua
        // streamDelayMs is this plus the AL queue.
        int delayMs() const { return bufferedFrames() * sFrameMs; }
        const JitterBufferStats& stats() const { return mStats; }

    private:
        enum class State
        {
            Idle, // no spurt in progress; pop() yields Silence
            Prebuffering, // spurt started, filling to mTargetFrames
            Playing,
        };

        struct Slot
        {
            bool mValid = false;
            bool mEndOfSpurt = false;
            std::uint16_t mSeq = 0;
            std::uint16_t mSize = 0;
            std::array<unsigned char, sMaxPayloadBytes> mData;
        };

        struct DeltaSample
        {
            TimePoint mArrival;
            std::uint16_t mDeviationMs;
        };

        struct RingScan
        {
            int mSpan = 0; // cursor to newest held packet, inclusive
            int mOldestAhead = -1; // -1 when nothing at or after the cursor
            bool mHaveEndOfSpurt = false;
        };

        // 5.1 s of arrival history at 50 pps, comfortably more than the widest
        // mDeltaWindow anyone would set.
        static constexpr std::size_t sDeltaSamples = 256;

        // Sequence numbers are 16 bit and wrap every 21.8 minutes at 50 packets
        // a second, which is inside a normal session. Nothing may compare them
        // with <. RFC 1982 serial arithmetic.
        static bool isNewer(std::uint16_t a, std::uint16_t b) { return static_cast<std::int16_t>(a - b) > 0; }

        Slot& slot(std::uint16_t seq) { return mRing[seq % sCapacityFrames]; }
        const Slot& slot(std::uint16_t seq) const { return mRing[seq % sCapacityFrames]; }

        RingScan scanRing() const;
        PopResult decide();
        PopResult emitSilence();
        void beginSpurt(std::uint16_t seq);
        void startSpurtFromRing();
        void endSpurt();
        void recordArrival(TimePoint arrival);
        // target = clamp(1 + ceil(p95(max(0, delta - sFrameMs)) / sFrameMs)
        //                  + mFecFrames, min, max)
        void recomputeTarget();
        void resync(std::uint16_t seq);
        void refreshGauges();

        JitterBufferSettings mSettings;
        std::array<Slot, sCapacityFrames> mRing;
        State mState = State::Idle;
        std::uint16_t mNextSeq = 0; // the frame pop() will decide about
        bool mHaveSeq = false;
        int mTargetFrames;
        int mConcealRun = 0;
        bool mInUnderrun = false;
        int mSilentFrames = 0;
        int mPushesSinceTarget = 0;
        bool mHaveArrival = false;
        TimePoint mLastArrival;
        std::array<DeltaSample, sDeltaSamples> mDeltas;
        std::size_t mDeltaCount = 0;
        std::size_t mDeltaNext = 0;
        JitterBufferStats mStats;
    };
}

#endif // OPENMW_COMPONENTS_VOIP_JITTERBUFFER_H
