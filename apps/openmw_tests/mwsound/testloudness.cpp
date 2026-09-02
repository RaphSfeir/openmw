#include "apps/openmw/mwsound/loudness.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace MWSound
{
    namespace
    {
        using namespace testing;

        constexpr int sRate = 48000;
        constexpr float sPerSecond = 20.0f;
        constexpr std::size_t sSamplesPerSegment = sRate / static_cast<int>(sPerSecond);

        // One segment's worth of a constant amplitude, as 16-bit mono bytes.
        std::vector<char> segmentAt(float amplitude)
        {
            std::vector<char> data(sSamplesPerSegment * sizeof(std::int16_t));
            const std::int16_t value = static_cast<std::int16_t>(amplitude * 32767.0f);
            for (std::size_t i = 0; i < sSamplesPerSegment; ++i)
                std::memcpy(data.data() + i * sizeof(std::int16_t), &value, sizeof(value));
            return data;
        }

        Sound_Loudness makeAnalyser()
        {
            return Sound_Loudness(sPerSecond, sRate, ChannelConfig_Mono, SampleType_Int16);
        }

        TEST(MWSoundLoudnessTest, shouldReportTheLoudnessOfEachSegment)
        {
            Sound_Loudness loudness = makeAnalyser();
            loudness.analyzeLoudness(segmentAt(0.5f));
            loudness.analyzeLoudness(segmentAt(0.25f));

            EXPECT_NEAR(loudness.getLoudnessAtTime(0.0f), 0.5f, 0.01f);
            EXPECT_NEAR(loudness.getLoudnessAtTime(1.0f / sPerSecond), 0.25f, 0.01f);
        }

        TEST(MWSoundLoudnessTest, shouldClampAQueryPastTheEndToTheLastSegment)
        {
            Sound_Loudness loudness = makeAnalyser();
            loudness.analyzeLoudness(segmentAt(0.5f));
            // Behaviour the vanilla callers rely on: a say sound is read at an
            // offset that can run just past what has been analysed.
            EXPECT_NEAR(loudness.getLoudnessAtTime(60.0f), 0.5f, 0.01f);
        }

        TEST(MWSoundLoudnessTest, shouldAnswerZeroBeforeAnythingIsAnalysed)
        {
            Sound_Loudness loudness = makeAnalyser();
            EXPECT_FLOAT_EQ(loudness.getLoudnessAtTime(0.0f), 0.0f);
            EXPECT_FLOAT_EQ(loudness.getLoudnessAtTime(-1.0f), 0.0f);
        }

        TEST(MWSoundLoudnessTest, shouldKeepTheRecentPastAfterFarMoreThanTheHistoryLength)
        {
            Sound_Loudness loudness = makeAnalyser();

            // Two hours of audio at 20 segments a second. Before the history was
            // bounded this allocated a float per segment for the whole run,
            // which is what an attached voice stream does to a long session.
            constexpr int sSegments = 20 * 60 * 60 * 2;
            for (int i = 0; i < sSegments; ++i)
                loudness.analyzeLoudness(segmentAt(0.5f));
            loudness.analyzeLoudness(segmentAt(0.125f));

            // The most recent segment is what lip sync actually reads.
            const float last = static_cast<float>(sSegments) / sPerSecond;
            EXPECT_NEAR(loudness.getLoudnessAtTime(last), 0.125f, 0.01f);
            EXPECT_NEAR(loudness.getLoudnessAtTime(last - 1.0f), 0.5f, 0.01f);
        }

        TEST(MWSoundLoudnessTest, shouldTreatEvictedHistoryAsSilenceRatherThanAsSomethingElse)
        {
            Sound_Loudness loudness = makeAnalyser();

            // A quiet opening segment, then far more than the history can hold
            // at a different level. Reading the opening back must not return the
            // later level just because the ring wrapped onto its slot.
            loudness.analyzeLoudness(segmentAt(0.125f));
            for (int i = 0; i < 4000; ++i)
                loudness.analyzeLoudness(segmentAt(0.75f));

            EXPECT_FLOAT_EQ(loudness.getLoudnessAtTime(0.0f), 0.0f);
        }
    }
}
