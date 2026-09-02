#include <components/voip/jitterbuffer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace
{
    using namespace testing;
    using namespace Voip;

    constexpr std::size_t sPayloadSize = 60;

    // Payload bytes are derived from the sequence number so a test can tell
    // which packet came back, which is the whole point of the Fec case.
    std::array<unsigned char, sPayloadSize> makePayload(int seq)
    {
        std::array<unsigned char, sPayloadSize> payload{};
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<unsigned char>(seq + static_cast<int>(i));
        return payload;
    }

    JitterBuffer::TimePoint at(int milliseconds)
    {
        return JitterBuffer::TimePoint(std::chrono::milliseconds(milliseconds));
    }

    void pushFrame(JitterBuffer& buffer, int seq, int arrivalMs, bool endOfSpurt = false)
    {
        const std::array<unsigned char, sPayloadSize> payload = makePayload(seq);
        VoicePacket packet;
        packet.mSeq = static_cast<std::uint16_t>(seq);
        packet.mEndOfSpurt = endOfSpurt;
        packet.mData = payload.data();
        packet.mSize = payload.size();
        buffer.push(packet, at(arrivalMs));
    }

    char outcomeLetter(PopOutcome outcome)
    {
        switch (outcome)
        {
            case PopOutcome::Frame:
                return 'F';
            case PopOutcome::Fec:
                return 'E';
            case PopOutcome::Conceal:
                return 'C';
            case PopOutcome::Silence:
                return 'S';
        }
        return '?';
    }

    // A whole playout run reads better as one string than as a column of
    // per-call expectations, and a mismatch shows the shape of the divergence.
    std::string popPattern(JitterBuffer& buffer, int frames)
    {
        std::string pattern;
        for (int i = 0; i < frames; ++i)
            pattern.push_back(outcomeLetter(buffer.pop().mOutcome));
        return pattern;
    }

    // Reported rather than asserted: a fatal assertion inside a helper only
    // leaves the helper.
    AssertionResult isFrame(const PopResult& result, int seq)
    {
        if (result.mOutcome != PopOutcome::Frame)
            return AssertionFailure() << "expected Frame for seq " << seq << ", got " << outcomeLetter(result.mOutcome);
        if (result.mSeq != static_cast<std::uint16_t>(seq))
            return AssertionFailure() << "expected seq " << seq << ", got " << result.mSeq;
        const std::array<unsigned char, sPayloadSize> expected = makePayload(seq);
        if (result.mSize != expected.size())
            return AssertionFailure() << "expected " << expected.size() << " bytes, got " << result.mSize;
        if (result.mData == nullptr || !std::equal(expected.begin(), expected.end(), result.mData))
            return AssertionFailure() << "payload does not belong to seq " << seq;
        return AssertionSuccess();
    }

    AssertionResult isFec(const PopResult& result, int missingSeq, int successorSeq)
    {
        if (result.mOutcome != PopOutcome::Fec)
            return AssertionFailure() << "expected Fec for seq " << missingSeq << ", got "
                                      << outcomeLetter(result.mOutcome);
        if (result.mSeq != static_cast<std::uint16_t>(missingSeq))
            return AssertionFailure() << "expected the MISSING seq " << missingSeq << ", got " << result.mSeq;
        const std::array<unsigned char, sPayloadSize> expected = makePayload(successorSeq);
        if (result.mSize != expected.size())
            return AssertionFailure() << "expected " << expected.size() << " bytes, got " << result.mSize;
        if (result.mData == nullptr || !std::equal(expected.begin(), expected.end(), result.mData))
            return AssertionFailure() << "payload is not the successor packet " << successorSeq;
        return AssertionSuccess();
    }

    void pushSpurtHead(JitterBuffer& buffer)
    {
        pushFrame(buffer, 0, 0);
        pushFrame(buffer, 1, 20);
        pushFrame(buffer, 2, 40);
    }

    TEST(VoipJitterBufferTest, shouldReturnSilenceWhenNothingHasBeenPushed)
    {
        JitterBuffer buffer;
        const PopResult result = buffer.pop();
        EXPECT_EQ(outcomeLetter(result.mOutcome), 'S');
        EXPECT_EQ(result.mData, nullptr);
        EXPECT_EQ(result.mSize, 0u);
        EXPECT_EQ(popPattern(buffer, 20), std::string(20, 'S'));
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
        EXPECT_EQ(buffer.stats().mSilence, 21u);
    }

    TEST(VoipJitterBufferTest, shouldPlayAnInOrderStreamWithoutConcealment)
    {
        JitterBuffer buffer;
        std::string pattern;
        for (int i = 0; i < 20; ++i)
        {
            pushFrame(buffer, i, i * 20);
            if (i >= 2)
                pattern += popPattern(buffer, 1);
        }
        pattern += popPattern(buffer, 2);
        EXPECT_EQ(pattern, std::string(20, 'F'));
        EXPECT_EQ(buffer.stats().mPlayed, 20u);
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
        EXPECT_EQ(buffer.stats().mUnderruns, 0u);
    }

    TEST(VoipJitterBufferTest, shouldEmitSilenceUntilTheBufferReachesItsTarget)
    {
        const JitterBufferSettings settings;
        JitterBuffer buffer(settings);
        EXPECT_EQ(popPattern(buffer, 1), "S");
        pushFrame(buffer, 0, 0);
        EXPECT_EQ(popPattern(buffer, 1), "S");
        pushFrame(buffer, 1, 20);
        EXPECT_EQ(popPattern(buffer, 1), "S");
        pushFrame(buffer, 2, 40);
        EXPECT_TRUE(isFrame(buffer.pop(), 0));
        EXPECT_EQ(buffer.stats().mSilence, static_cast<std::uint64_t>(settings.mInitialTargetFrames));
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
    }

    TEST(VoipJitterBufferTest, shouldReleasePrebufferingEarlyWhenTheSpurtIsShorterThanTheTarget)
    {
        JitterBuffer buffer;
        pushFrame(buffer, 0, 0);
        pushFrame(buffer, 1, 20, true);
        EXPECT_EQ(popPattern(buffer, 4), "FFSS");
        EXPECT_EQ(buffer.stats().mPlayed, 2u);
    }

    TEST(VoipJitterBufferTest, shouldReorderAPacketThatArrivesOutOfOrder)
    {
        JitterBuffer buffer;
        pushFrame(buffer, 3, 0);
        pushFrame(buffer, 5, 20);
        pushFrame(buffer, 4, 40);
        EXPECT_TRUE(isFrame(buffer.pop(), 3));
        EXPECT_TRUE(isFrame(buffer.pop(), 4));
        EXPECT_TRUE(isFrame(buffer.pop(), 5));
        EXPECT_EQ(buffer.stats().mLate, 0u);
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
    }

    TEST(VoipJitterBufferTest, shouldRecoverAMissingFrameFromItsSuccessorWithFec)
    {
        JitterBuffer buffer;
        pushFrame(buffer, 0, 0);
        pushFrame(buffer, 1, 20);
        pushFrame(buffer, 3, 40);
        EXPECT_TRUE(isFrame(buffer.pop(), 0));
        EXPECT_TRUE(isFrame(buffer.pop(), 1));
        EXPECT_TRUE(isFec(buffer.pop(), 2, 3));
        EXPECT_TRUE(isFrame(buffer.pop(), 3));
        EXPECT_EQ(buffer.stats().mFecRecovered, 1u);
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
    }

    TEST(VoipJitterBufferTest, shouldConcealAMissingFrameWithNothingBehindIt)
    {
        JitterBuffer buffer;
        pushSpurtHead(buffer);
        EXPECT_EQ(popPattern(buffer, 3), "FFF");
        const PopResult result = buffer.pop();
        EXPECT_EQ(outcomeLetter(result.mOutcome), 'C');
        EXPECT_EQ(result.mSeq, 3);
        EXPECT_EQ(result.mData, nullptr);
        EXPECT_EQ(result.mSize, 0u);
        EXPECT_EQ(buffer.stats().mConcealed, 1u);
        EXPECT_EQ(buffer.stats().mSilence, 0u);
    }

    TEST(VoipJitterBufferTest, shouldKeepAnsweringThroughALongLossRun)
    {
        JitterBuffer buffer;
        pushSpurtHead(buffer);
        pushFrame(buffer, 13, 60);
        EXPECT_EQ(popPattern(buffer, 14), "FFFCCCCCCCCCEF");
        EXPECT_EQ(buffer.stats().mConcealed, 9u);
        EXPECT_EQ(buffer.stats().mFecRecovered, 1u);
        EXPECT_EQ(buffer.stats().mUnderruns, 0u);
    }

    TEST(VoipJitterBufferTest, shouldEndTheSpurtAfterProlongedStarvation)
    {
        const JitterBufferSettings settings;
        JitterBuffer buffer(settings);
        pushSpurtHead(buffer);
        EXPECT_EQ(popPattern(buffer, 3), "FFF");
        EXPECT_EQ(popPattern(buffer, 10), "CCCCCSSSSS");
        EXPECT_EQ(buffer.stats().mConcealed, static_cast<std::uint64_t>(settings.mMaxConcealFrames));
        EXPECT_EQ(buffer.stats().mSilence, 5u);
    }

    TEST(VoipJitterBufferTest, shouldReturnSilenceNotConcealAfterEndOfSpurt)
    {
        JitterBuffer buffer;
        pushFrame(buffer, 0, 0);
        pushFrame(buffer, 1, 20);
        pushFrame(buffer, 2, 40, true);
        EXPECT_EQ(popPattern(buffer, 12), "FFFSSSSSSSSS");
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
        EXPECT_EQ(buffer.stats().mSilence, 9u);
    }

    TEST(VoipJitterBufferTest, shouldCountAndDropDuplicates)
    {
        JitterBuffer buffer;
        pushFrame(buffer, 0, 0);
        pushFrame(buffer, 0, 20);
        pushFrame(buffer, 1, 40);
        pushFrame(buffer, 2, 60);
        EXPECT_EQ(buffer.stats().mReceived, 4u);
        EXPECT_EQ(buffer.stats().mDuplicate, 1u);
        EXPECT_EQ(popPattern(buffer, 4), "FFFC");
        EXPECT_EQ(buffer.stats().mPlayed, 3u);
    }

    TEST(VoipJitterBufferTest, shouldCountAndDropLatePackets)
    {
        JitterBuffer buffer;
        pushSpurtHead(buffer);
        EXPECT_TRUE(isFrame(buffer.pop(), 0));
        pushFrame(buffer, 0, 60);
        EXPECT_EQ(buffer.stats().mLate, 1u);
        EXPECT_TRUE(isFrame(buffer.pop(), 1));
        EXPECT_TRUE(isFrame(buffer.pop(), 2));
        EXPECT_EQ(buffer.stats().mPlayed, 3u);
    }

    TEST(VoipJitterBufferTest, shouldPlayInOrderAcrossASequenceNumberWraparound)
    {
        JitterBuffer buffer;
        pushFrame(buffer, 65534, 0);
        pushFrame(buffer, 65535, 20);
        pushFrame(buffer, 0, 40);
        pushFrame(buffer, 1, 60);
        EXPECT_TRUE(isFrame(buffer.pop(), 65534));
        EXPECT_TRUE(isFrame(buffer.pop(), 65535));
        EXPECT_TRUE(isFrame(buffer.pop(), 0));
        EXPECT_TRUE(isFrame(buffer.pop(), 1));
        EXPECT_EQ(buffer.stats().mLate, 0u);
        EXPECT_EQ(buffer.stats().mResyncs, 0u);
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
    }

    TEST(VoipJitterBufferTest, shouldResyncOnceOnAFarFutureSequenceNumber)
    {
        JitterBuffer buffer;
        pushSpurtHead(buffer);
        EXPECT_EQ(popPattern(buffer, 3), "FFF");
        pushFrame(buffer, 1003, 60);
        EXPECT_EQ(buffer.stats().mResyncs, 1u);
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
        EXPECT_EQ(popPattern(buffer, 1), "S");
        pushFrame(buffer, 1004, 80);
        EXPECT_EQ(popPattern(buffer, 1), "S");
        pushFrame(buffer, 1005, 100);
        EXPECT_TRUE(isFrame(buffer.pop(), 1003));
        EXPECT_EQ(buffer.stats().mResyncs, 1u);
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
    }

    TEST(VoipJitterBufferTest, shouldGrowTheTargetWhenArrivalsAreSpiky)
    {
        JitterBuffer buffer;
        for (int i = 0; i < 120; ++i)
        {
            pushFrame(buffer, i, i * 20 + (i % 10 == 5 ? 60 : 0));
            if (i >= 2)
                buffer.pop();
        }
        EXPECT_EQ(buffer.stats().mJitterMs, 60);
        EXPECT_EQ(buffer.targetFrames(), 5);
        // Growth has to come from the statistic, not from starvation.
        EXPECT_EQ(buffer.stats().mUnderruns, 0u);
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
    }

    TEST(VoipJitterBufferTest, shouldPinTheTargetToTheMinimumOnACleanStream)
    {
        const JitterBufferSettings settings;
        JitterBuffer buffer(settings);
        for (int i = 0; i < 100; ++i)
        {
            pushFrame(buffer, i, i * 20, i == 99);
            if (i >= 2)
                buffer.pop();
        }
        while (buffer.bufferedFrames() > 0)
            buffer.pop();
        EXPECT_EQ(buffer.stats().mJitterMs, 0);
        EXPECT_EQ(buffer.stats().mUnderruns, 0u);
        EXPECT_EQ(buffer.targetFrames(), settings.mInitialTargetFrames);
        popPattern(buffer, settings.mShrinkAfterSilentFrames - 1);
        EXPECT_EQ(buffer.targetFrames(), settings.mInitialTargetFrames);
        buffer.pop();
        EXPECT_EQ(buffer.targetFrames(), settings.mMinTargetFrames);
    }

    TEST(VoipJitterBufferTest, shouldClampTheTargetToTheMaximumUnderPathologicalJitter)
    {
        const JitterBufferSettings settings;
        JitterBuffer buffer(settings);
        for (int i = 0; i < 120; ++i)
        {
            pushFrame(buffer, i, i * 20 + (i % 3 == 0 ? 500 : 0));
            if (i >= 2)
                buffer.pop();
        }
        EXPECT_EQ(buffer.stats().mJitterMs, 500);
        EXPECT_EQ(buffer.targetFrames(), settings.mMaxTargetFrames);
    }

    TEST(VoipJitterBufferTest, shouldRaiseTheTargetImmediatelyOnAnUnderrun)
    {
        const JitterBufferSettings settings;
        JitterBuffer buffer(settings);
        pushSpurtHead(buffer);
        EXPECT_EQ(popPattern(buffer, 3), "FFF");
        EXPECT_EQ(buffer.targetFrames(), settings.mInitialTargetFrames);
        buffer.pop();
        EXPECT_EQ(buffer.targetFrames(), settings.mInitialTargetFrames + 1);
        EXPECT_EQ(buffer.stats().mUnderruns, 1u);
        // One starvation episode, however many frames it lasts.
        buffer.pop();
        EXPECT_EQ(buffer.targetFrames(), settings.mInitialTargetFrames + 1);
        EXPECT_EQ(buffer.stats().mUnderruns, 1u);
    }

    TEST(VoipJitterBufferTest, shouldNotAdvanceThePlayCursorWhileStarved)
    {
        JitterBuffer buffer;
        pushSpurtHead(buffer);
        EXPECT_EQ(popPattern(buffer, 3), "FFF");
        const PopResult first = buffer.pop();
        const PopResult second = buffer.pop();
        EXPECT_EQ(outcomeLetter(first.mOutcome), 'C');
        EXPECT_EQ(outcomeLetter(second.mOutcome), 'C');
        EXPECT_EQ(first.mSeq, 3);
        EXPECT_EQ(second.mSeq, 3);
        // The stretch is what buys the depth: the frame that was merely late is
        // still playable rather than counted as arriving after its slot.
        pushFrame(buffer, 3, 200);
        EXPECT_EQ(buffer.stats().mLate, 0u);
        EXPECT_TRUE(isFrame(buffer.pop(), 3));
    }

    TEST(VoipJitterBufferTest, shouldShrinkTheTargetOnlyAfterSilentFramesAndOnlyByOneStep)
    {
        JitterBufferSettings settings;
        settings.mInitialTargetFrames = 5;
        settings.mShrinkAfterSilentFrames = 4;
        JitterBuffer buffer(settings);
        pushFrame(buffer, 0, 0);
        pushFrame(buffer, 1, 20);
        pushFrame(buffer, 2, 40, true);
        EXPECT_EQ(popPattern(buffer, 3), "FFF");
        EXPECT_EQ(buffer.targetFrames(), 5);
        EXPECT_EQ(popPattern(buffer, 3), "SSS");
        EXPECT_EQ(buffer.targetFrames(), 5);
        EXPECT_EQ(popPattern(buffer, 1), "S");
        EXPECT_EQ(buffer.targetFrames(), 4);
        popPattern(buffer, 4);
        EXPECT_EQ(buffer.targetFrames(), 3);
        popPattern(buffer, 4);
        EXPECT_EQ(buffer.targetFrames(), settings.mMinTargetFrames);
        popPattern(buffer, 100);
        EXPECT_EQ(buffer.targetFrames(), settings.mMinTargetFrames);
    }

    TEST(VoipJitterBufferTest, shouldRejectAnOversizedPayload)
    {
        JitterBuffer buffer;
        const std::array<unsigned char, sMaxPayloadBytes + 1> payload{};
        VoicePacket packet;
        packet.mSeq = 0;
        packet.mData = payload.data();
        packet.mSize = payload.size();
        buffer.push(packet, at(0));
        EXPECT_EQ(buffer.stats().mReceived, 1u);
        EXPECT_EQ(buffer.bufferedFrames(), 0);
        EXPECT_EQ(popPattern(buffer, 1), "S");

        packet.mSize = sMaxPayloadBytes;
        buffer.push(packet, at(20));
        EXPECT_EQ(buffer.bufferedFrames(), 1);
    }

    TEST(VoipJitterBufferTest, shouldReportDelayFromBufferedFrames)
    {
        JitterBuffer buffer;
        EXPECT_EQ(buffer.bufferedFrames(), 0);
        EXPECT_EQ(buffer.delayMs(), 0);
        pushSpurtHead(buffer);
        EXPECT_EQ(buffer.bufferedFrames(), 3);
        EXPECT_EQ(buffer.delayMs(), 3 * sFrameMs);
        EXPECT_EQ(buffer.stats().mBufferedFrames, 3);
        buffer.pop();
        EXPECT_EQ(buffer.bufferedFrames(), 2);
        EXPECT_EQ(buffer.delayMs(), 2 * sFrameMs);
        EXPECT_EQ(buffer.stats().mBufferedFrames, 2);
    }

    TEST(VoipJitterBufferTest, shouldRestoreConstructedStateOnReset)
    {
        const JitterBufferSettings settings;
        JitterBuffer buffer(settings);
        pushSpurtHead(buffer);
        popPattern(buffer, 5);
        ASSERT_NE(buffer.stats().mPlayed, 0u);
        ASSERT_NE(buffer.targetFrames(), settings.mInitialTargetFrames);

        buffer.reset();
        EXPECT_EQ(buffer.stats().mReceived, 0u);
        EXPECT_EQ(buffer.stats().mPlayed, 0u);
        EXPECT_EQ(buffer.stats().mConcealed, 0u);
        EXPECT_EQ(buffer.targetFrames(), settings.mInitialTargetFrames);
        EXPECT_EQ(buffer.bufferedFrames(), 0);
        EXPECT_EQ(popPattern(buffer, 1), "S");

        // A reattached speaker anchors wherever its counter happens to be.
        pushFrame(buffer, 500, 1000);
        EXPECT_EQ(buffer.stats().mResyncs, 0u);
        EXPECT_EQ(buffer.bufferedFrames(), 1);
    }
}
