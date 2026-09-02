#include "apps/openmw/mwsound/voicecodec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <opus.h>

namespace MWSound
{
    namespace
    {
        // Opus spends the first frames of a stream choosing a mode and settling
        // its noise shaping, so a run has to be long enough that the part these
        // tests measure is past that.
        constexpr int sFrames = 25;
        constexpr int sSkippedFrames = 4;

        // Well inside the run: the frame the FEC test throws away has to have
        // both a converged encoder behind it and a successor to be recovered
        // from.
        constexpr int sLostFrame = 12;

        constexpr double sTwoPi = 6.283185307179586;
        constexpr double sToneHz = 440.0;
        constexpr double sToneAmplitude = 0.5;
        constexpr double sSyllableHz = 4.0;

        // The encoder is built with in-band FEC on and a non-zero expected
        // loss, and Opus spends bits on redundancy only while both hold. Zero
        // expected loss is therefore the one lever the public voice API gives a
        // test for turning redundancy off, and it is what the FEC-off arm of
        // the comparisons below is made of. Nothing here re-asserts the FEC-on
        // side: leaving that to the constructor is what makes these tests fail
        // if the shipped default ever stops carrying redundancy.
        constexpr int sNoExpectedLoss = 0;

        // Half scale is the loudest anything here gets, so a sample still
        // holding this after a call is a sample the decoder never wrote.
        constexpr std::int16_t sSentinel = std::numeric_limits<std::int16_t>::max();

        // Opus delays the signal by its lookahead, which is a fraction of a
        // frame; searching half a frame is well past it without ever reaching
        // the neighbouring pitch periods that would make the search ambiguous.
        // The delay this build actually shows is 307 samples.
        constexpr std::size_t sMaxCodecDelaySamples = sVoiceFrameSamples / 2;

        // How much better a frame rebuilt from in-band redundancy has to be
        // than the same frame concealed. Measured on this signal: the FEC
        // reconstruction lands at a mean squared error of 1.6e5 against the
        // original, concealment at 4.1e6, a factor of 25.9. Requiring five
        // leaves the assertion five times clear of the number it was set from,
        // which is the room a different Opus build or a different machine's
        // rounding gets to move in.
        constexpr double sFecErrorRatio = 5.0;

        // The ratio alone would still pass if both arms degraded together, so
        // the reconstruction is also held to an absolute standard: its error
        // against the original, as a fraction of the frame's own energy. The
        // measured value is 0.0070, so the bound sits seven times above it.
        constexpr double sFecErrorFraction = 0.05;

        std::size_t frameOffset(int frame)
        {
            return static_cast<std::size_t>(frame) * static_cast<std::size_t>(sVoiceFrameSamples);
        }

        // Energy per frame is constant, which is the only thing the round trip
        // can compare: the codec delays the signal by about 6.5 ms, so the two
        // runs never line up sample by sample.
        std::vector<std::int16_t> makeTone(int frames)
        {
            std::vector<std::int16_t> pcm(frameOffset(frames));
            const double scale = sToneAmplitude * std::numeric_limits<std::int16_t>::max();
            for (std::size_t i = 0; i < pcm.size(); ++i)
            {
                const double t = static_cast<double>(i) / sVoiceSampleRate;
                pcm[i] = static_cast<std::int16_t>(scale * std::sin(sTwoPi * sToneHz * t));
            }
            return pcm;
        }

        // SILK only spends bits on FEC for frames its own voice detector calls
        // active, so the loss tests feed a syllable rhythm and a harmonic
        // instead of a flat sine, and keep the envelope off silence throughout.
        std::vector<std::int16_t> makeSpeechLike(int frames)
        {
            std::vector<std::int16_t> pcm(frameOffset(frames));
            const double scale = sToneAmplitude * std::numeric_limits<std::int16_t>::max();
            for (std::size_t i = 0; i < pcm.size(); ++i)
            {
                const double t = static_cast<double>(i) / sVoiceSampleRate;
                const double envelope = 0.6 + 0.4 * std::sin(sTwoPi * sSyllableHz * t);
                const double wave
                    = 0.7 * std::sin(sTwoPi * sToneHz * t) + 0.3 * std::sin(2.0 * sTwoPi * sToneHz * t);
                pcm[i] = static_cast<std::int16_t>(scale * envelope * wave);
            }
            return pcm;
        }

        double rms(const std::int16_t* pcm, std::size_t count)
        {
            double sum = 0.0;
            for (std::size_t i = 0; i < count; ++i)
                sum += static_cast<double>(pcm[i]) * static_cast<double>(pcm[i]);
            return std::sqrt(sum / static_cast<double>(count));
        }

        double meanSquaredError(const std::int16_t* left, const std::int16_t* right, std::size_t count)
        {
            double sum = 0.0;
            for (std::size_t i = 0; i < count; ++i)
            {
                const double difference = static_cast<double>(left[i]) - static_cast<double>(right[i]);
                sum += difference * difference;
            }
            return sum / static_cast<double>(count);
        }

        // Opus's own algorithmic delay is not reachable through the voice API,
        // so the tests recover it rather than hard coding it: decoded audio is
        // the input pushed a fixed number of samples later, and the shift that
        // minimises the error across the settled part of a run is that delay.
        // Without it nothing can be compared against the original at all, since
        // at 440 Hz a six millisecond slip is most of a period.
        std::size_t measureCodecDelay(const std::vector<std::int16_t>& input, const std::vector<std::int16_t>& decoded)
        {
            const std::size_t from = frameOffset(sSkippedFrames);
            std::size_t best = 0;
            double bestError = std::numeric_limits<double>::max();
            for (std::size_t delay = 0; delay <= sMaxCodecDelaySamples; ++delay)
            {
                const double error
                    = meanSquaredError(decoded.data() + from, input.data() + from - delay, decoded.size() - from);
                if (error < bestError)
                {
                    bestError = error;
                    best = delay;
                }
            }
            return best;
        }

        // The only direct question that can be asked of a packet about its
        // redundancy. opus_decode with decode_fec set answers nothing: a packet
        // with no redundancy in it makes the decoder conceal instead, and the
        // call still reports success and still fills the frame, so success
        // there is not evidence that any FEC exists.
        bool hasLbrr(const std::vector<unsigned char>& packet)
        {
            return opus_packet_has_lbrr(packet.data(), static_cast<opus_int32>(packet.size())) == 1;
        }

        // An empty packet is what the encoder returns for a frame it decided
        // not to send, and it is also what a broken encoder returns. Feeding
        // one to the decoder is the documented way to ask for concealment, so
        // a run of them would leave every loss test here passing on nothing.
        ::testing::AssertionResult encodeAll(
            VoiceEncoder& encoder, const std::vector<std::int16_t>& pcm, std::vector<std::vector<unsigned char>>& out)
        {
            out.clear();
            for (std::size_t offset = 0; offset + sVoiceFrameSamples <= pcm.size(); offset += sVoiceFrameSamples)
            {
                std::vector<unsigned char> packet(sVoiceMaxPacketBytes);
                packet.resize(encoder.encode(pcm.data() + offset, packet.data(), packet.size()));
                if (packet.empty())
                    return ::testing::AssertionFailure() << "encoder produced nothing for frame " << out.size();
                out.push_back(std::move(packet));
            }
            return ::testing::AssertionSuccess();
        }

        // Neither concealment nor FEC has anything to work from until a frame
        // has been decoded normally, so the loss tests run the decoder up to
        // the gap they are about to open. The result is reported rather than
        // asserted here so that a failure stops the caller: a fatal assertion
        // inside a helper only leaves the helper.
        ::testing::AssertionResult decodeThrough(
            VoiceDecoder& decoder, const std::vector<std::vector<unsigned char>>& packets, int frames, std::int16_t* pcm)
        {
            for (int frame = 0; frame < frames; ++frame)
            {
                if (!decoder.decode(packets[frame].data(), packets[frame].size(), pcm + frameOffset(frame)))
                    return ::testing::AssertionFailure() << "decode failed on frame " << frame;
            }
            return ::testing::AssertionSuccess();
        }

        TEST(MWSoundVoiceCodecTest, shouldRoundTripATone)
        {
            const std::vector<std::int16_t> input = makeTone(sFrames);
            VoiceEncoder encoder;
            VoiceDecoder decoder;
            std::vector<std::int16_t> output(input.size());
            std::vector<unsigned char> packet(sVoiceMaxPacketBytes);

            for (int frame = 0; frame < sFrames; ++frame)
            {
                const std::size_t size
                    = encoder.encode(input.data() + frameOffset(frame), packet.data(), packet.size());
                ASSERT_GT(size, 0u);
                ASSERT_TRUE(decoder.decode(packet.data(), size, output.data() + frameOffset(frame)));
            }

            const std::size_t measured = input.size() - frameOffset(sSkippedFrames);
            const double sent = rms(input.data() + frameOffset(sSkippedFrames), measured);
            const double heard = rms(output.data() + frameOffset(sSkippedFrames), measured);

            EXPECT_GT(heard, sent * 0.5);
            EXPECT_LT(heard, sent * 2.0);
        }

        TEST(MWSoundVoiceCodecTest, shouldProduceReasonablePacketSizes)
        {
            const std::vector<std::int16_t> input = makeTone(sFrames);
            VoiceEncoder encoder;
            ASSERT_EQ(encoder.getBitrate(), sVoiceDefaultBitrate);
            std::vector<std::vector<unsigned char>> packets;

            ASSERT_TRUE(encodeAll(encoder, input, packets));

            ASSERT_EQ(packets.size(), static_cast<std::size_t>(sFrames));
            for (const std::vector<unsigned char>& packet : packets)
            {
                // The cap is what the relay enforces, so the distance below it
                // is the headroom a talker has before packets start being
                // dropped instead of sent.
                EXPECT_LT(packet.size(), sVoiceMaxPacketBytes / 2);
            }
        }

        TEST(MWSoundVoiceCodecTest, shouldCarryInBandRedundancyOnlyWhenLossIsExpected)
        {
            const std::vector<std::int16_t> input = makeSpeechLike(sFrames);
            VoiceEncoder fecEncoder;
            std::vector<std::vector<unsigned char>> fecPackets;
            ASSERT_TRUE(encodeAll(fecEncoder, input, fecPackets));
            VoiceEncoder plainEncoder;
            plainEncoder.setExpectedLoss(sNoExpectedLoss);
            std::vector<std::vector<unsigned char>> plainPackets;
            ASSERT_TRUE(encodeAll(plainEncoder, input, plainPackets));
            ASSERT_EQ(fecPackets.size(), plainPackets.size());

            // Redundancy is not unconditional even with FEC configured: the
            // first packet has no predecessor to protect, and on this run SILK
            // also stops coding it over the last three frames. The window
            // checked is the settled stretch the recovery test draws on, where
            // this build carries it in every packet without exception.
            for (int frame = sSkippedFrames; frame <= sLostFrame + 1; ++frame)
                EXPECT_TRUE(hasLbrr(fecPackets[frame])) << "frame " << frame;

            // The other arm has to be empty of it everywhere, not just in the
            // window: one packet with redundancy would mean the comparison is
            // not between FEC and concealment at all.
            for (int frame = 0; frame < sFrames; ++frame)
                EXPECT_FALSE(hasLbrr(plainPackets[frame])) << "frame " << frame;
        }

        TEST(MWSoundVoiceCodecTest, shouldRecoverALostFrameFromFec)
        {
            const std::vector<std::int16_t> input = makeSpeechLike(sFrames);
            VoiceEncoder fecEncoder;
            std::vector<std::vector<unsigned char>> fecPackets;
            ASSERT_TRUE(encodeAll(fecEncoder, input, fecPackets));
            VoiceEncoder plainEncoder;
            plainEncoder.setExpectedLoss(sNoExpectedLoss);
            std::vector<std::vector<unsigned char>> plainPackets;
            ASSERT_TRUE(encodeAll(plainEncoder, input, plainPackets));
            ASSERT_GT(fecPackets.size(), static_cast<std::size_t>(sLostFrame + 1));
            ASSERT_EQ(fecPackets.size(), plainPackets.size());

            // The FEC copy of a frame rides in the packet after it, so the
            // successor of the frame that went missing is what reconstructs it,
            // and whether that successor carries redundancy is what separates
            // the two arms.
            const std::vector<unsigned char>& fecSuccessor = fecPackets[sLostFrame + 1];
            const std::vector<unsigned char>& plainSuccessor = plainPackets[sLostFrame + 1];
            ASSERT_TRUE(hasLbrr(fecSuccessor));
            ASSERT_FALSE(hasLbrr(plainSuccessor));

            // Each decoder is walked to the gap on its own stream, so both face
            // the loss with a full frame of history behind them and neither is
            // reconstructing from a decoder that has seen less audio.
            VoiceDecoder fecDecoder;
            std::vector<std::int16_t> fecHistory(frameOffset(sLostFrame));
            ASSERT_TRUE(decodeThrough(fecDecoder, fecPackets, sLostFrame, fecHistory.data()));
            VoiceDecoder plainDecoder;
            std::vector<std::int16_t> plainHistory(frameOffset(sLostFrame));
            ASSERT_TRUE(decodeThrough(plainDecoder, plainPackets, sLostFrame, plainHistory.data()));

            std::vector<std::int16_t> recovered(sVoiceFrameSamples, sSentinel);
            ASSERT_TRUE(fecDecoder.decodeFec(fecSuccessor.data(), fecSuccessor.size(), recovered.data()));
            EXPECT_EQ(std::count(recovered.begin(), recovered.end(), sSentinel), 0);

            std::vector<std::int16_t> concealed(sVoiceFrameSamples, sSentinel);
            ASSERT_TRUE(plainDecoder.decodeFec(plainSuccessor.data(), plainSuccessor.size(), concealed.data()));
            EXPECT_EQ(std::count(concealed.begin(), concealed.end(), sSentinel), 0);

            const std::size_t delay = measureCodecDelay(input, fecHistory);
            ASSERT_LT(delay, sMaxCodecDelaySamples);
            const std::int16_t* original = input.data() + frameOffset(sLostFrame) - delay;
            const double energy = rms(original, recovered.size());
            const double recoveredError = meanSquaredError(recovered.data(), original, recovered.size());
            const double concealedError = meanSquaredError(concealed.data(), original, concealed.size());

            EXPECT_LT(recoveredError * sFecErrorRatio, concealedError);
            EXPECT_LT(recoveredError, energy * energy * sFecErrorFraction);
        }

        TEST(MWSoundVoiceCodecTest, shouldConcealAFrameWithoutFec)
        {
            const std::vector<std::int16_t> input = makeSpeechLike(sFrames);
            VoiceEncoder encoder;
            std::vector<std::vector<unsigned char>> packets;
            ASSERT_TRUE(encodeAll(encoder, input, packets));
            VoiceDecoder decoder;
            std::vector<std::int16_t> history(frameOffset(sSkippedFrames));
            ASSERT_TRUE(decodeThrough(decoder, packets, sSkippedFrames, history.data()));
            std::vector<std::int16_t> pcm(sVoiceFrameSamples, sSentinel);

            EXPECT_NO_THROW(decoder.conceal(pcm.data()));

            EXPECT_EQ(std::count(pcm.begin(), pcm.end(), sSentinel), 0);
        }

        TEST(MWSoundVoiceCodecTest, shouldSurviveACorruptPacket)
        {
            VoiceDecoder decoder;
            std::vector<std::int16_t> pcm(sVoiceFrameSamples, sSentinel);
            std::vector<unsigned char> packet(64);
            std::mt19937 random(42);
            std::uniform_int_distribution<int> byte(0, 255);
            std::generate(
                packet.begin(), packet.end(), [&] { return static_cast<unsigned char>(byte(random)); });
            // A random first byte usually still describes something Opus is
            // willing to decode, so the header is made one it must refuse: code
            // 3 claiming 63 frames of 20 ms, far past the 120 ms a packet may
            // hold. The payload behind it stays garbage.
            packet[0] = 0xfb;
            packet[1] = 0x3f;

            EXPECT_FALSE(decoder.decode(packet.data(), packet.size(), pcm.data()));

            EXPECT_EQ(std::count(pcm.begin(), pcm.end(), std::int16_t{ 0 }), static_cast<std::ptrdiff_t>(pcm.size()));
        }
    }
}
