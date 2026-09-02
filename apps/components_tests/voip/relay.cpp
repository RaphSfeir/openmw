#include <components/voip/relay.hpp>
#include <components/voip/wire.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace testing;
    using namespace Voip;

    struct Sent
    {
        std::uint32_t mTo = 0;
        std::vector<unsigned char> mBytes;
    };

    Relay::TimePoint at(int milliseconds)
    {
        return Relay::TimePoint(std::chrono::milliseconds(milliseconds));
    }

    Relay::Sink recorder(std::vector<Sent>& out)
    {
        return [&out](std::uint32_t to, const unsigned char* data, std::size_t size) {
            out.push_back(Sent{ to, std::vector<unsigned char>(data, data + size) });
        };
    }

    // Payload bytes are derived from the sequence number so a test can tell
    // which datagram came back out, and whether anything past the header moved.
    std::vector<unsigned char> makeDatagram(int seq, std::size_t payloadBytes = 40, std::uint32_t speaker = 0,
        std::uint8_t flags = 0, std::uint8_t version = sWireVersion)
    {
        std::vector<unsigned char> datagram(sHeaderBytes + payloadBytes);
        Header header;
        header.mVersion = version;
        header.mFlags = flags;
        header.mSeq = static_cast<std::uint16_t>(seq);
        header.mSpeaker = speaker;
        writeHeader(datagram.data(), header);
        for (std::size_t i = 0; i < payloadBytes; ++i)
            datagram[sHeaderBytes + i] = static_cast<unsigned char>(seq * 7 + static_cast<int>(i));
        return datagram;
    }

    // Enabled, with a sink, and with the peer list the transport would have
    // handed tick() - without which the fan-out has no audience to find.
    // A relay with these peers connected and, by default, all of them having
    // announced voice. The announce is explicit because capability is closed
    // until a peer asks: most tests are about something else and want the gate
    // open, but the ones that are ABOUT the gate pass announced = false and
    // grant it themselves, so that what they assert is the thing they name.
    void bringUp(Relay& relay, const std::vector<std::uint32_t>& peers, std::vector<Sent>& sink,
        bool announced = true)
    {
        relay.setSink(recorder(sink));
        relay.setEnabled(true);
        relay.tick(peers, at(0));
        if (announced)
        {
            for (const std::uint32_t peer : peers)
                relay.setCapable(peer, true);
        }
    }

    std::string peerList(std::vector<std::uint32_t> peers)
    {
        std::sort(peers.begin(), peers.end());
        std::string text;
        for (const std::uint32_t peer : peers)
        {
            if (!text.empty())
                text += ",";
            text += std::to_string(peer);
        }
        return text.empty() ? std::string("nobody") : text;
    }

    // Reported rather than asserted: a fatal assertion inside a helper only
    // leaves the helper.
    AssertionResult wentTo(const std::vector<Sent>& sent, const std::vector<std::uint32_t>& expected)
    {
        std::vector<std::uint32_t> actual;
        for (const Sent& one : sent)
            actual.push_back(one.mTo);
        if (peerList(actual) != peerList(expected))
            return AssertionFailure() << "forwarded to " << peerList(actual) << ", expected " << peerList(expected);
        return AssertionSuccess();
    }

    // Everything the relay is allowed to change is bytes 4..7, and it must have
    // changed them to the true origin.
    AssertionResult carries(const Sent& sent, std::uint32_t origin, const std::vector<unsigned char>& original)
    {
        if (sent.mBytes.size() != original.size())
            return AssertionFailure() << "forwarded " << sent.mBytes.size() << " bytes, expected " << original.size();

        Header out;
        Header in;
        if (!readHeader(sent.mBytes.data(), sent.mBytes.size(), out))
            return AssertionFailure() << "forwarded datagram cannot hold a header";
        if (!readHeader(original.data(), original.size(), in))
            return AssertionFailure() << "the test built a datagram without a header";
        if (out.mSpeaker != origin)
            return AssertionFailure() << "speaker id is " << out.mSpeaker << ", expected the true origin " << origin;
        if (out.mVersion != in.mVersion || out.mFlags != in.mFlags || out.mSeq != in.mSeq)
            return AssertionFailure() << "version, flags or seq were rewritten";

        const std::ptrdiff_t header = static_cast<std::ptrdiff_t>(sHeaderBytes);
        if (!std::equal(original.begin() + header, original.end(), sent.mBytes.begin() + header))
            return AssertionFailure() << "payload bytes past the header were altered";
        return AssertionSuccess();
    }

    TEST(VoipRelayTest, shouldForwardToEveryOtherPeerByDefault)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2, 3 }, sent);

        const std::vector<unsigned char> datagram = makeDatagram(7);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));

        ASSERT_EQ(sent.size(), 2u);
        EXPECT_TRUE(wentTo(sent, { 2, 3 }));
        EXPECT_TRUE(carries(sent[0], 1, datagram));
        EXPECT_TRUE(carries(sent[1], 1, datagram));
        EXPECT_EQ(relay.counters().mReceived, 1u);
        EXPECT_EQ(relay.counters().mForwarded, 2u);
        EXPECT_EQ(relay.counters().mBytesIn, datagram.size());
        EXPECT_EQ(relay.counters().mBytesOut, 2 * datagram.size());
        EXPECT_EQ(relay.counters().mDroppedNoRoute, 0u);
    }

    TEST(VoipRelayTest, shouldNeverRouteAPacketBackToItsOrigin)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2 }, sent);
        // Even asked to, in as many words.
        EXPECT_TRUE(relay.setRoute(1, { 1, 2, 1 }));

        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));

        EXPECT_TRUE(wentTo(sent, { 2 }));
        EXPECT_EQ(relay.counters().mForwarded, 1u);
    }

    TEST(VoipRelayTest, shouldKeepThePayloadByteIdenticalAcrossTheRelay)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 4, 5 }, sent);

        // The largest datagram the clamp allows, so the copy is exercised to the
        // last byte of the scratch buffer.
        const std::vector<unsigned char> datagram = makeDatagram(
            300, sMaxPacketBytes - sHeaderBytes, 0, static_cast<std::uint8_t>(FlagEndOfSpurt | FlagShout));
        relay.onPacket(4, datagram.data(), datagram.size(), at(10));

        ASSERT_EQ(sent.size(), 1u);
        EXPECT_EQ(sent[0].mBytes.size(), sMaxPacketBytes);
        EXPECT_TRUE(carries(sent[0], 4, datagram));
        EXPECT_EQ(relay.counters().mDroppedOversize, 0u);
    }

    TEST(VoipRelayTest, shouldDropADatagramShorterThanTheHeader)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2 }, sent);

        const std::vector<unsigned char> stub(sHeaderBytes - 1, 0);
        relay.onPacket(1, stub.data(), stub.size(), at(10));
        relay.onPacket(1, nullptr, 0, at(10));

        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mReceived, 2u);
        EXPECT_EQ(relay.counters().mDroppedMalformed, 2u);
        EXPECT_EQ(relay.counters().mDroppedOversize, 0u);
        EXPECT_EQ(relay.counters().mDroppedVersion, 0u);
    }

    TEST(VoipRelayTest, shouldDropAnOversizeDatagram)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2 }, sent);

        const std::vector<unsigned char> tooBig = makeDatagram(1, sMaxPacketBytes - sHeaderBytes + 1);
        relay.onPacket(1, tooBig.data(), tooBig.size(), at(10));
        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedOversize, 1u);

        const std::vector<unsigned char> exactlyBigEnough = makeDatagram(2, sMaxPacketBytes - sHeaderBytes);
        relay.onPacket(1, exactlyBigEnough.data(), exactlyBigEnough.size(), at(20));
        EXPECT_EQ(sent.size(), 1u);
        EXPECT_EQ(relay.counters().mDroppedOversize, 1u);
    }

    TEST(VoipRelayTest, shouldClampAConfiguredPacketSizeToTheWireMaximum)
    {
        RelayLimits limits;
        limits.mMaxPacketBytes = 4000; // larger than the scratch buffer it would be copied into
        std::vector<Sent> sent;
        Relay relay(limits);
        bringUp(relay, { 1, 2 }, sent);

        const std::vector<unsigned char> datagram = makeDatagram(1, sMaxPacketBytes - sHeaderBytes + 1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));

        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedOversize, 1u);
    }

    TEST(VoipRelayTest, shouldDropAnUnknownWireVersion)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2 }, sent);

        const std::vector<unsigned char> datagram
            = makeDatagram(1, 40, 0, 0, static_cast<std::uint8_t>(sWireVersion + 1));
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));

        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedVersion, 1u);
        EXPECT_EQ(relay.counters().mDroppedMalformed, 0u);
    }

    TEST(VoipRelayTest, shouldForwardUnknownFlagBitsUnchanged)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2 }, sent);

        // A newer client talking about things this build does not know is
        // harmless; muting it would not be.
        const std::vector<unsigned char> datagram = makeDatagram(1, 40, 0, 0xff);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));

        ASSERT_EQ(sent.size(), 1u);
        EXPECT_TRUE(carries(sent[0], 1, datagram));
        EXPECT_EQ(sent[0].mBytes[1], 0xff);
        EXPECT_EQ(relay.counters().mDroppedVersion, 0u);
    }

    TEST(VoipRelayTest, shouldOverwriteAndCountASpoofedSpeakerId)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2 }, sent);

        const std::vector<unsigned char> datagram = makeDatagram(9, 40, 4242);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));

        ASSERT_EQ(sent.size(), 1u);
        Header forwarded;
        ASSERT_TRUE(readHeader(sent[0].mBytes.data(), sent[0].mBytes.size(), forwarded));
        EXPECT_EQ(forwarded.mSpeaker, 1u);
        EXPECT_EQ(relay.counters().mSpoofedOrigin, 1u);
        // Neutralised, not dropped: a client that silences itself invisibly is
        // worse than one that leaves evidence in a counter.
        EXPECT_EQ(relay.counters().mForwarded, 1u);
        EXPECT_EQ(relay.counters().mDroppedMalformed, 0u);
    }

    TEST(VoipRelayTest, shouldDropOverTheRateCapButTolerateABurst)
    {
        RelayLimits limits;
        limits.mPacketsPerSecond = 10.0;
        limits.mBurstPackets = 5.0;
        std::vector<Sent> sent;
        Relay relay(limits);
        bringUp(relay, { 1, 2 }, sent);

        const std::vector<unsigned char> datagram = makeDatagram(1);
        for (int i = 0; i < 6; ++i)
            relay.onPacket(1, datagram.data(), datagram.size(), at(0));

        EXPECT_EQ(sent.size(), 5u);
        EXPECT_EQ(relay.counters().mDroppedRate, 1u);

        // Half a second later the bucket has refilled to exactly its ceiling.
        for (int i = 0; i < 6; ++i)
            relay.onPacket(1, datagram.data(), datagram.size(), at(500));

        EXPECT_EQ(sent.size(), 10u);
        EXPECT_EQ(relay.counters().mDroppedRate, 2u);
        EXPECT_EQ(relay.counters().mForwarded, 10u);
    }

    TEST(VoipRelayTest, shouldForwardNothingWhileDisabled)
    {
        std::vector<Sent> sent;
        Relay relay;
        relay.setSink(recorder(sent));
        relay.tick({ 1, 2 }, at(0));
        // Both willing, so that what is under test is the operator's switch and
        // not the peers'. They are separate gates and both must be open.
        relay.setCapable(1, true);
        relay.setCapable(2, true);
        EXPECT_FALSE(relay.enabled());

        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));
        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedDisabled, 1u);

        relay.setEnabled(true);
        relay.onPacket(1, datagram.data(), datagram.size(), at(20));
        EXPECT_EQ(sent.size(), 1u);
        EXPECT_EQ(relay.counters().mDroppedDisabled, 1u);
    }

    TEST(VoipRelayTest, shouldNotRelayAPeerThatNeverAnnouncedCapability)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2 }, sent, /*announced=*/false);

        // Connected is not the same as willing. A peer that has not announced
        // has no add-on, no voice build, or one older than the announce, and
        // forwarding to any of them is bandwidth spent on frames that will be
        // discarded.
        EXPECT_FALSE(relay.capable(1));
        EXPECT_FALSE(relay.capable(4242)); // not even connected
        EXPECT_EQ(relay.capablePeers(), 0u);

        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));

        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedNotCapable, 1u);
    }

    TEST(VoipRelayTest, shouldRequireBothEndsToHaveAnnounced)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2, 3 }, sent, /*announced=*/false);

        // Only the speaker and one listener are willing. The other listener is
        // skipped at fan-out rather than the whole packet being dropped, which
        // is what makes a mixed party partially audible instead of silent --
        // and is why a player without the add-on hears nothing while everyone
        // else carries on.
        relay.setCapable(1, true);
        relay.setCapable(2, true);

        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));

        EXPECT_TRUE(wentTo(sent, { 2 }));
        EXPECT_EQ(relay.capablePeers(), 2u);
    }

    TEST(VoipRelayTest, shouldDropAPeerMarkedIncapableBeforeReadingItsBytes)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2, 3 }, sent);
        EXPECT_TRUE(relay.setCapable(2, false));
        EXPECT_FALSE(relay.capable(2));
        EXPECT_EQ(relay.capablePeers(), 2u);

        // Nothing it sends is read: a datagram that would fail every other guard
        // still lands on this one.
        const std::vector<unsigned char> rubbish(3, 0xff);
        relay.onPacket(2, rubbish.data(), rubbish.size(), at(10));
        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedNotCapable, 1u);
        EXPECT_EQ(relay.counters().mDroppedMalformed, 0u);

        // And nothing reaches it either.
        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(20));
        EXPECT_TRUE(wentTo(sent, { 3 }));

        sent.clear();
        EXPECT_TRUE(relay.setCapable(2, true));
        relay.onPacket(2, datagram.data(), datagram.size(), at(30));
        EXPECT_TRUE(wentTo(sent, { 1, 3 }));
    }

    TEST(VoipRelayTest, shouldHonourAnExplicitRoute)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2, 3, 4 }, sent);
        EXPECT_TRUE(relay.setRoute(1, { 3 }));
        EXPECT_EQ(relay.routedPeers(), 1u);

        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));
        EXPECT_TRUE(wentTo(sent, { 3 }));

        // Only the peer the route names is affected.
        sent.clear();
        relay.onPacket(2, datagram.data(), datagram.size(), at(20));
        EXPECT_TRUE(wentTo(sent, { 1, 3, 4 }));
    }

    TEST(VoipRelayTest, shouldRestoreTheDefaultRouteWhenARouteIsCleared)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2, 3 }, sent);
        EXPECT_TRUE(relay.setRoute(1, { 3 }));

        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));
        EXPECT_TRUE(wentTo(sent, { 3 }));

        sent.clear();
        relay.clearRoute(1);
        EXPECT_EQ(relay.routedPeers(), 0u);
        relay.onPacket(1, datagram.data(), datagram.size(), at(20));
        EXPECT_TRUE(wentTo(sent, { 2, 3 }));
    }

    TEST(VoipRelayTest, shouldCountADatagramWithNowhereToGo)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2, 3 }, sent);
        const std::vector<unsigned char> datagram = makeDatagram(1);

        // An empty target list is a valid exception, and means nobody hears it.
        EXPECT_TRUE(relay.setRoute(1, {}));
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));
        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedNoRoute, 1u);

        // So is a route naming somebody who is not here.
        EXPECT_TRUE(relay.setRoute(1, { 4242 }));
        relay.onPacket(1, datagram.data(), datagram.size(), at(20));
        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedNoRoute, 2u);

        // And so is being the only peer left.
        relay.clearRoute(1);
        relay.tick({ 1 }, at(30));
        relay.onPacket(1, datagram.data(), datagram.size(), at(40));
        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.counters().mDroppedNoRoute, 3u);
        EXPECT_EQ(relay.counters().mForwarded, 0u);
    }

    TEST(VoipRelayTest, shouldPruneADepartedPeerOnTick)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2, 3 }, sent);
        EXPECT_TRUE(relay.setRoute(3, { 1 }));
        EXPECT_EQ(relay.trackedPeers(), 3u);

        relay.tick({ 1, 2 }, at(1000));
        EXPECT_EQ(relay.trackedPeers(), 2u);
        EXPECT_EQ(relay.counters().mPrunedPeers, 1u);
        // The route went with the peer it spoke for.
        EXPECT_EQ(relay.routedPeers(), 0u);

        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(1010));
        EXPECT_TRUE(wentTo(sent, { 2 }));

        // A peer that comes back is tracked again, from scratch -- and "from
        // scratch" includes its capability. A returning peer has to announce
        // again; inheriting the grant of whoever last held that id is how a
        // recycled peer number ends up carrying somebody else's permission.
        relay.tick({ 1, 2, 3 }, at(2000));
        EXPECT_EQ(relay.trackedPeers(), 3u);
        EXPECT_EQ(relay.counters().mPrunedPeers, 1u);
        EXPECT_FALSE(relay.capable(3));
    }

    TEST(VoipRelayTest, shouldRefuseToGrowPastThePeerCap)
    {
        RelayLimits limits;
        limits.mMaxPeers = 2;
        std::vector<Sent> sent;
        Relay relay(limits);
        // No blanket grant: setCapable on a peer the full table cannot hold is
        // itself a refusal, so announcing for all four would count two extra
        // and this test's subject -- how many peers the cap refused -- would be
        // measuring its own setup.
        bringUp(relay, { 1, 2, 3, 4 }, sent, /*announced=*/false);
        relay.setCapable(1, true);
        relay.setCapable(2, true);

        EXPECT_EQ(relay.trackedPeers(), 2u);
        EXPECT_EQ(relay.counters().mRefusedPeers, 2u);
        EXPECT_FALSE(relay.setCapable(9, false));

        // A peer the full table cannot track is refused, not relayed.
        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(3, datagram.data(), datagram.size(), at(10));
        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(relay.trackedPeers(), 2u);
        EXPECT_EQ(relay.counters().mReceived, 1u);
        EXPECT_GT(relay.counters().mRefusedPeers, 2u);
    }

    TEST(VoipRelayTest, shouldRefuseARouteLongerThanTheTargetCap)
    {
        RelayLimits limits;
        limits.mMaxRouteTargets = 2;
        Relay relay(limits);

        EXPECT_TRUE(relay.setRoute(1, { 2, 3 }));
        EXPECT_FALSE(relay.setRoute(1, { 2, 3, 4 }));
        EXPECT_EQ(relay.routedPeers(), 1u);
        EXPECT_EQ(relay.counters().mRefusedPeers, 1u);
    }

    TEST(VoipRelayTest, shouldRoundTripTheWireHeaderInLittleEndian)
    {
        std::array<unsigned char, sHeaderBytes> bytes{};
        Header written;
        written.mFlags = static_cast<std::uint8_t>(FlagShout | FlagNonPositional);
        written.mSeq = 0x0201;
        written.mSpeaker = 0x08070605;
        writeHeader(bytes.data(), written);

        EXPECT_EQ(bytes[0], sWireVersion);
        EXPECT_EQ(bytes[1], static_cast<unsigned char>(FlagShout | FlagNonPositional));
        EXPECT_EQ(bytes[2], 0x01);
        EXPECT_EQ(bytes[3], 0x02);
        EXPECT_EQ(bytes[4], 0x05);
        EXPECT_EQ(bytes[5], 0x06);
        EXPECT_EQ(bytes[6], 0x07);
        EXPECT_EQ(bytes[7], 0x08);

        Header read;
        ASSERT_TRUE(readHeader(bytes.data(), bytes.size(), read));
        EXPECT_EQ(read.mVersion, written.mVersion);
        EXPECT_EQ(read.mFlags, written.mFlags);
        EXPECT_EQ(read.mSeq, written.mSeq);
        EXPECT_EQ(read.mSpeaker, written.mSpeaker);

        // Only bytes 4..7 move when the origin is stamped.
        writeSpeaker(bytes.data(), 0xfffefdfc);
        ASSERT_TRUE(readHeader(bytes.data(), bytes.size(), read));
        EXPECT_EQ(read.mSpeaker, 0xfffefdfcu);
        EXPECT_EQ(read.mSeq, written.mSeq);
        EXPECT_EQ(read.mFlags, written.mFlags);

        EXPECT_FALSE(readHeader(bytes.data(), sHeaderBytes - 1, read));
        EXPECT_FALSE(readHeader(nullptr, sHeaderBytes, read));
    }

    TEST(VoipRelayTest, shouldNotForwardWithoutASink)
    {
        Relay relay;
        relay.setEnabled(true);
        relay.tick({ 1, 2 }, at(0));
        // Granted explicitly, so the packet reaches the missing-sink check.
        // Without this it would be dropped a guard earlier as not capable, and
        // the test would go green while proving nothing about sinks.
        relay.setCapable(1, true);
        relay.setCapable(2, true);

        const std::vector<unsigned char> datagram = makeDatagram(1);
        relay.onPacket(1, datagram.data(), datagram.size(), at(10));
        EXPECT_EQ(relay.counters().mDroppedNoRoute, 1u);
        EXPECT_EQ(relay.counters().mForwarded, 0u);
    }

    TEST(VoipRelayTest, shouldAcceptTheTransportsPayloadTypeUnchanged)
    {
        std::vector<Sent> sent;
        Relay relay;
        bringUp(relay, { 1, 2 }, sent);

        // The shape Net::Session::ChannelHandler hands over, adapted without the
        // relay knowing that the transport exists.
        const std::vector<unsigned char> datagram = makeDatagram(11);
        const std::string_view view(reinterpret_cast<const char*>(datagram.data()), datagram.size());
        relay.onPacket(1, view, at(10));

        ASSERT_EQ(sent.size(), 1u);
        EXPECT_TRUE(carries(sent[0], 1, datagram));
    }
}
