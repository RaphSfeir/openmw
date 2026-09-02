#include "apps/openmw/mwsound/networkvoicedecoder.hpp"
#include "apps/openmw/mwsound/voicecodec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace MWSound
{
    namespace
    {
        using namespace testing;

        // Speech-like rather than a pure tone: SILK only emits redundancy for
        // material its voice detector believes in, so a flat sine would leave
        // the FEC paths untested.
        std::vector<std::int16_t> makeSpeechLike(int frames)
        {
            std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * sVoiceFrameSamples);
            for (std::size_t i = 0; i < pcm.size(); ++i)
            {
                const double t = static_cast<double>(i) / sVoiceSampleRate;
                const double envelope = 0.6 + 0.4 * std::sin(2.0 * 3.14159265358979 * 4.0 * t);
                const double tone = std::sin(2.0 * 3.14159265358979 * 220.0 * t)
                    + 0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 * t);
                pcm[i] = static_cast<std::int16_t>(9000.0 * envelope * tone);
            }
            return pcm;
        }

        std::vector<std::vector<unsigned char>> encodeFrames(const std::vector<std::int16_t>& pcm)
        {
            VoiceEncoder encoder;
            std::vector<std::vector<unsigned char>> packets;
            const std::size_t frames = pcm.size() / sVoiceFrameSamples;
            for (std::size_t f = 0; f < frames; ++f)
            {
                std::vector<unsigned char> packet(sVoiceMaxPacketBytes);
                const std::size_t size
                    = encoder.encode(pcm.data() + f * sVoiceFrameSamples, packet.data(), packet.size());
                packet.resize(size);
                packets.push_back(std::move(packet));
            }
            return packets;
        }

        double rms(const std::int16_t* pcm, std::size_t count)
        {
            double sum = 0.0;
            for (std::size_t i = 0; i < count; ++i)
                sum += static_cast<double>(pcm[i]) * pcm[i];
            return std::sqrt(sum / static_cast<double>(count));
        }

        // Every test asserts on this: the sound system treats a short read as
        // the end of the stream and destroys the speaker's attachment, so the
        // byte count is the contract, not the audio.
        ::testing::AssertionResult readsFully(NetworkVoiceDecoder& decoder, std::vector<char>& out, std::size_t bytes)
        {
            out.assign(bytes, char{ 0x7f });
            const std::size_t got = decoder.read(out.data(), bytes);
            if (got != bytes)
                return ::testing::AssertionFailure() << "read returned " << got << " of " << bytes << " bytes";
            return ::testing::AssertionSuccess();
        }

        constexpr std::size_t sFrameBytes = static_cast<std::size_t>(sVoiceFrameSamples) * sizeof(std::int16_t);

        TEST(MWSoundNetworkVoiceDecoderTest, shouldReportFortyEightKilohertzMonoSixteenBit)
        {
            NetworkVoiceDecoder decoder;
            int rate = 0;
            ChannelConfig chans = ChannelConfig_Stereo;
            SampleType type = SampleType_Float32;
            decoder.getInfo(&rate, &chans, &type);
            EXPECT_EQ(rate, sVoiceSampleRate);
            EXPECT_EQ(chans, ChannelConfig_Mono);
            EXPECT_EQ(type, SampleType_Int16);
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldFillSilenceWhenNothingHasEverArrived)
        {
            NetworkVoiceDecoder decoder;
            std::vector<char> out;
            ASSERT_TRUE(readsFully(decoder, out, sFrameBytes * 4));
            EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldNeverShortReadAcrossAnArbitraryByteCount)
        {
            NetworkVoiceDecoder decoder;
            const std::vector<std::int16_t> pcm = makeSpeechLike(30);
            const auto packets = encodeFrames(pcm);
            for (std::size_t i = 0; i < packets.size(); ++i)
                decoder.pushPacket(static_cast<std::uint16_t>(i), packets[i].data(), packets[i].size(), false);

            // Sizes that deliberately do not divide into whole frames, because
            // the sound system picks the read size and it has no reason to.
            std::vector<char> out;
            for (const std::size_t bytes : { std::size_t{ 1 }, std::size_t{ 333 }, sFrameBytes - 2, sFrameBytes,
                     sFrameBytes + 7, sFrameBytes * 3 + 1 })
                ASSERT_TRUE(readsFully(decoder, out, bytes)) << "at read size " << bytes;
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldKeepReadingLongAfterThePacketsRunOut)
        {
            NetworkVoiceDecoder decoder;
            const std::vector<std::int16_t> pcm = makeSpeechLike(5);
            const auto packets = encodeFrames(pcm);
            for (std::size_t i = 0; i < packets.size(); ++i)
                decoder.pushPacket(static_cast<std::uint16_t>(i), packets[i].data(), packets[i].size(), false);

            // Two hundred frames is four seconds of a speaker who stopped
            // talking. Every one of them must still answer in full.
            std::vector<char> out;
            for (int i = 0; i < 200; ++i)
                ASSERT_TRUE(readsFully(decoder, out, sFrameBytes)) << "at frame " << i;
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldPlayAudibleAudioForArrivedPackets)
        {
            NetworkVoiceDecoder decoder;
            const std::vector<std::int16_t> pcm = makeSpeechLike(40);
            const auto packets = encodeFrames(pcm);
            for (std::size_t i = 0; i < packets.size(); ++i)
                decoder.pushPacket(static_cast<std::uint16_t>(i), packets[i].data(), packets[i].size(), false);

            std::vector<char> out;
            double loudest = 0.0;
            for (int i = 0; i < 40; ++i)
            {
                ASSERT_TRUE(readsFully(decoder, out, sFrameBytes));
                loudest = std::max(
                    loudest, rms(reinterpret_cast<const std::int16_t*>(out.data()), sVoiceFrameSamples));
            }
            EXPECT_GT(loudest, 500.0) << "decoded stream never became audible";
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldFadeOutRatherThanConcealForever)
        {
            NetworkVoiceDecoder decoder;
            const std::vector<std::int16_t> pcm = makeSpeechLike(20);
            const auto packets = encodeFrames(pcm);
            for (std::size_t i = 0; i < packets.size(); ++i)
                decoder.pushPacket(static_cast<std::uint16_t>(i), packets[i].data(), packets[i].size(), false);

            std::vector<char> out;
            for (int i = 0; i < 20; ++i)
                ASSERT_TRUE(readsFully(decoder, out, sFrameBytes));

            // The speaker has gone quiet without saying so. Concealment carries
            // the first frames and then has to give up: what must never happen
            // is an indefinite metallic tail.
            std::vector<double> tail;
            for (int i = 0; i < 12; ++i)
            {
                ASSERT_TRUE(readsFully(decoder, out, sFrameBytes));
                tail.push_back(rms(reinterpret_cast<const std::int16_t*>(out.data()), sVoiceFrameSamples));
            }
            EXPECT_DOUBLE_EQ(tail.back(), 0.0) << "concealment never faded to silence";
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldRecoverALostFrameFromItsSuccessor)
        {
            NetworkVoiceDecoder decoder;
            const std::vector<std::int16_t> pcm = makeSpeechLike(24);
            const auto packets = encodeFrames(pcm);

            // Frame 8 never arrives. Its redundancy rides in packet 9, which
            // does, so the buffer should ask for a FEC rebuild rather than a
            // concealment - and either way the read must be whole.
            //
            // Pushes are interleaved with reads rather than dumped in at once:
            // the ring holds sixteen frames, and filling it before consuming
            // anything is not a jitter scenario, it is a speaker who has run
            // ahead far enough for the buffer to give up and resync.
            std::vector<char> out;
            for (std::size_t i = 0; i < packets.size(); ++i)
            {
                if (i != 8)
                    decoder.pushPacket(static_cast<std::uint16_t>(i), packets[i].data(), packets[i].size(), false);
                if (i >= 4)
                    ASSERT_TRUE(readsFully(decoder, out, sFrameBytes)) << "at frame " << i;
            }
            for (int i = 0; i < 6; ++i)
                ASSERT_TRUE(readsFully(decoder, out, sFrameBytes));

            const Voip::JitterBufferStats stats = decoder.stats();
            EXPECT_GT(stats.mFecRecovered, 0u) << "the gap was concealed rather than rebuilt from FEC";
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldAdvanceTheSampleOffsetMonotonically)
        {
            NetworkVoiceDecoder decoder;
            std::vector<char> out;
            std::size_t previous = decoder.getSampleOffset();
            for (int i = 0; i < 10; ++i)
            {
                ASSERT_TRUE(readsFully(decoder, out, sFrameBytes));
                const std::size_t now = decoder.getSampleOffset();
                // The loudness analyser and the stream offset are both indexed
                // by this, so it going backwards desynchronises lip sync.
                EXPECT_GT(now, previous);
                previous = now;
            }
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldSurviveAGarbagePacket)
        {
            NetworkVoiceDecoder decoder;
            std::vector<unsigned char> garbage(80, 0xa5);
            decoder.pushPacket(0, garbage.data(), garbage.size(), false);

            std::vector<char> out;
            for (int i = 0; i < 5; ++i)
                ASSERT_TRUE(readsFully(decoder, out, sFrameBytes));
        }

        TEST(MWSoundNetworkVoiceDecoderTest, shouldStayUsableAfterAReset)
        {
            NetworkVoiceDecoder decoder;
            const std::vector<std::int16_t> pcm = makeSpeechLike(10);
            const auto packets = encodeFrames(pcm);
            for (std::size_t i = 0; i < packets.size(); ++i)
                decoder.pushPacket(static_cast<std::uint16_t>(i), packets[i].data(), packets[i].size(), false);

            std::vector<char> out;
            ASSERT_TRUE(readsFully(decoder, out, sFrameBytes * 2));
            decoder.resetBuffer();
            ASSERT_TRUE(readsFully(decoder, out, sFrameBytes * 2));

            for (std::size_t i = 0; i < packets.size(); ++i)
                decoder.pushPacket(static_cast<std::uint16_t>(100 + i), packets[i].data(), packets[i].size(), false);
            for (int i = 0; i < 10; ++i)
                ASSERT_TRUE(readsFully(decoder, out, sFrameBytes));
        }
    }
}
