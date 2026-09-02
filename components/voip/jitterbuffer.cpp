#include "jitterbuffer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Voip
{
    namespace
    {
        // Sorting 256 samples per packet would put the statistic on the hot path
        // for no benefit: the target only ever moves in whole 20 ms steps.
        constexpr int sRecomputeEveryPushes = 10;
    }

    JitterBuffer::JitterBuffer(const JitterBufferSettings& settings)
        : mSettings(settings)
        , mTargetFrames(std::clamp(settings.mInitialTargetFrames, settings.mMinTargetFrames, settings.mMaxTargetFrames))
    {
        refreshGauges();
    }

    void JitterBuffer::push(const VoicePacket& packet, TimePoint arrival)
    {
        ++mStats.mReceived;
        if (packet.mData == nullptr || packet.mSize == 0 || packet.mSize > sMaxPayloadBytes)
            return;

        if (!mHaveSeq)
        {
            mHaveSeq = true;
            beginSpurt(packet.mSeq);
        }
        else if (isNewer(mNextSeq, packet.mSeq))
        {
            ++mStats.mLate;
            return;
        }
        else if (mState == State::Idle)
        {
            // A new word after quiet. However far the sender's counter has moved
            // meanwhile, the gap is not a stall, so this is not a resync.
            beginSpurt(packet.mSeq);
        }
        else if (static_cast<std::int16_t>(packet.mSeq - mNextSeq) >= static_cast<int>(sCapacityFrames))
        {
            resync(packet.mSeq);
        }

        Slot& target = slot(packet.mSeq);
        if (target.mValid && target.mSeq == packet.mSeq)
        {
            ++mStats.mDuplicate;
            return;
        }

        target.mValid = true;
        target.mEndOfSpurt = packet.mEndOfSpurt;
        target.mSeq = packet.mSeq;
        target.mSize = static_cast<std::uint16_t>(packet.mSize);
        std::memcpy(target.mData.data(), packet.mData, packet.mSize);

        recordArrival(arrival);
        refreshGauges();
    }

    PopResult JitterBuffer::pop()
    {
        const PopResult result = decide();
        refreshGauges();
        return result;
    }

    void JitterBuffer::reset()
    {
        for (Slot& s : mRing)
            s.mValid = false;
        mState = State::Idle;
        mNextSeq = 0;
        mHaveSeq = false;
        mTargetFrames
            = std::clamp(mSettings.mInitialTargetFrames, mSettings.mMinTargetFrames, mSettings.mMaxTargetFrames);
        mConcealRun = 0;
        mInUnderrun = false;
        mSilentFrames = 0;
        mPushesSinceTarget = 0;
        mHaveArrival = false;
        mDeltaCount = 0;
        mDeltaNext = 0;
        mStats = JitterBufferStats{};
        refreshGauges();
    }

    int JitterBuffer::bufferedFrames() const
    {
        return scanRing().mSpan;
    }

    JitterBuffer::RingScan JitterBuffer::scanRing() const
    {
        RingScan scan;
        for (const Slot& s : mRing)
        {
            if (!s.mValid)
                continue;
            // Slots left behind by a cursor that jumped are stale, not buffered.
            const int ahead = static_cast<std::int16_t>(s.mSeq - mNextSeq);
            if (ahead < 0)
                continue;
            scan.mSpan = std::max(scan.mSpan, ahead + 1);
            if (scan.mOldestAhead < 0 || ahead < scan.mOldestAhead)
                scan.mOldestAhead = ahead;
            if (s.mEndOfSpurt)
                scan.mHaveEndOfSpurt = true;
        }
        return scan;
    }

    PopResult JitterBuffer::decide()
    {
        if (mState == State::Idle)
        {
            startSpurtFromRing();
            if (mState == State::Idle)
                return emitSilence();
        }

        if (mState == State::Prebuffering)
        {
            const RingScan scan = scanRing();
            // A spurt shorter than the target is entirely in hand once its last
            // packet is, and waiting for depth that will never arrive would
            // strand it forever.
            if (scan.mSpan < mTargetFrames && !scan.mHaveEndOfSpurt)
                return emitSilence();
            if (scan.mOldestAhead > 0)
                mNextSeq = static_cast<std::uint16_t>(mNextSeq + scan.mOldestAhead);
            mState = State::Playing;
            mConcealRun = 0;
            mInUnderrun = false;
        }

        PopResult result;
        result.mSeq = mNextSeq;

        Slot& current = slot(mNextSeq);
        if (current.mValid && current.mSeq == mNextSeq)
        {
            result.mOutcome = PopOutcome::Frame;
            result.mData = current.mData.data();
            result.mSize = current.mSize;
            // Only the occupancy flag is cleared here; the bytes stay put so the
            // pointer just handed out survives until something overwrites them.
            current.mValid = false;
            const bool endOfSpurt = current.mEndOfSpurt;
            ++mStats.mPlayed;
            mConcealRun = 0;
            mInUnderrun = false;
            mNextSeq = static_cast<std::uint16_t>(mNextSeq + 1);
            if (endOfSpurt)
                endSpurt();
            return result;
        }

        const std::uint16_t successorSeq = static_cast<std::uint16_t>(mNextSeq + 1);
        Slot& successor = slot(successorSeq);
        if (successor.mValid && successor.mSeq == successorSeq)
        {
            result.mOutcome = PopOutcome::Fec;
            result.mData = successor.mData.data();
            result.mSize = successor.mSize;
            // The successor stays buffered: the next pop plays it for real.
            ++mStats.mFecRecovered;
            mConcealRun = 0;
            mInUnderrun = false;
            mNextSeq = successorSeq;
            return result;
        }

        const RingScan scan = scanRing();
        if (scan.mSpan == 0)
        {
            if (mConcealRun >= mSettings.mMaxConcealFrames)
            {
                // The caller has finished fading out by now, and a speaker who
                // has sent nothing for that long is not mid-word. Calling the
                // rest of the quiet loss would both sound wrong and inflate the
                // number voip.stats() reports as packet loss.
                endSpurt();
                return emitSilence();
            }
            if (!mInUnderrun)
            {
                mInUnderrun = true;
                ++mStats.mUnderruns;
                mTargetFrames = std::min(mTargetFrames + 1, mSettings.mMaxTargetFrames);
            }
            ++mConcealRun;
            ++mStats.mConcealed;
            result.mOutcome = PopOutcome::Conceal;
            // The cursor deliberately does not move. Raising the target adds no
            // depth by itself - the audio thread keeps asking at the same rate -
            // so the depth has to come from this inserted frame: staying put
            // spends a frame of time without spending a packet, and the frame
            // that was merely late gets played instead of discarded.
            return result;
        }

        // Something newer is already here, so the missing frame is genuinely
        // gone rather than late, and holding the cursor would stall the stream.
        ++mConcealRun;
        ++mStats.mConcealed;
        result.mOutcome = PopOutcome::Conceal;
        mNextSeq = static_cast<std::uint16_t>(mNextSeq + 1);
        return result;
    }

    PopResult JitterBuffer::emitSilence()
    {
        ++mStats.mSilence;
        if (mState == State::Idle)
        {
            ++mSilentFrames;
            if (mSilentFrames >= mSettings.mShrinkAfterSilentFrames)
            {
                mSilentFrames = 0;
                if (mTargetFrames > mSettings.mMinTargetFrames)
                    --mTargetFrames;
            }
        }

        PopResult result;
        result.mOutcome = PopOutcome::Silence;
        result.mSeq = mNextSeq;
        return result;
    }

    void JitterBuffer::beginSpurt(std::uint16_t seq)
    {
        mNextSeq = seq;
        mState = State::Prebuffering;
        mConcealRun = 0;
        mInUnderrun = false;
        mSilentFrames = 0;
        // The wait for the next word is not jitter. Kept as a baseline it would
        // enter the statistic as a multi-second spike and peg the target.
        mHaveArrival = false;
    }

    void JitterBuffer::startSpurtFromRing()
    {
        const RingScan scan = scanRing();
        if (scan.mOldestAhead < 0)
            return;
        beginSpurt(static_cast<std::uint16_t>(mNextSeq + scan.mOldestAhead));
    }

    void JitterBuffer::endSpurt()
    {
        mState = State::Idle;
        mSilentFrames = 0;
        mConcealRun = 0;
        mInUnderrun = false;
    }

    void JitterBuffer::recordArrival(TimePoint arrival)
    {
        if (mHaveArrival)
        {
            const std::int64_t deltaMs
                = std::chrono::duration_cast<std::chrono::milliseconds>(arrival - mLastArrival).count();
            // The p95 of raw deltas is ~20 ms on any network and would pin the
            // target to its minimum forever. What matters is how far a packet
            // ran late against the frame clock, not that it arrived at all.
            const std::int64_t deviationMs = std::min<std::int64_t>(
                std::max<std::int64_t>(0, deltaMs - sFrameMs), std::numeric_limits<std::uint16_t>::max());
            mDeltas[mDeltaNext] = DeltaSample{ arrival, static_cast<std::uint16_t>(deviationMs) };
            mDeltaNext = (mDeltaNext + 1) % mDeltas.size();
            if (mDeltaCount < mDeltas.size())
                ++mDeltaCount;
        }

        mLastArrival = arrival;
        mHaveArrival = true;

        if (++mPushesSinceTarget >= sRecomputeEveryPushes)
            recomputeTarget();
    }

    void JitterBuffer::recomputeTarget()
    {
        mPushesSinceTarget = 0;

        std::array<std::uint16_t, sDeltaSamples> window;
        std::size_t count = 0;
        const TimePoint cutoff = mLastArrival - mSettings.mDeltaWindow;
        for (std::size_t i = 0; i < mDeltaCount; ++i)
        {
            if (mDeltas[i].mArrival >= cutoff)
                window[count++] = mDeltas[i].mDeviationMs;
        }
        if (count == 0)
            return;

        std::sort(window.begin(), window.begin() + count);
        const int percentile = std::clamp(mSettings.mPercentile, 0, 100);
        std::size_t rank = (count * static_cast<std::size_t>(percentile) + 99) / 100;
        if (rank > 0)
            --rank;
        const int jitterMs = window[rank];
        mStats.mJitterMs = jitterMs;

        const int needed = 1 + (jitterMs + sFrameMs - 1) / sFrameMs + mSettings.mFecFrames;
        const int clamped = std::clamp(needed, mSettings.mMinTargetFrames, mSettings.mMaxTargetFrames);
        // Growth only. Giving back depth as eagerly as it is taken makes the
        // target oscillate, so shrinking is left to the silence hysteresis.
        mTargetFrames = std::max(mTargetFrames, clamped);
    }

    void JitterBuffer::resync(std::uint16_t seq)
    {
        for (Slot& s : mRing)
            s.mValid = false;
        ++mStats.mResyncs;
        beginSpurt(seq);
    }

    void JitterBuffer::refreshGauges()
    {
        mStats.mTargetFrames = mTargetFrames;
        mStats.mBufferedFrames = bufferedFrames();
    }
}
