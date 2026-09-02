#include <components/net/session.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace
{
    using namespace testing;
    using namespace Net;

    // A real loopback pair rather than a mock. What is under test here is how
    // ENet behaves at the edges - a channel that one side does not have, a
    // message diverted before it can be queued - and a mock would only ever
    // confirm what the test author already believed.
    constexpr std::uint16_t sPort = 27713;
    constexpr auto sTimeout = std::chrono::seconds(5);

    void pumpBoth(Session& a, Session& b)
    {
        a.pump();
        b.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ::testing::AssertionResult connectPair(Session& host, Session& client)
    {
        host.requestHost(sPort, 4);
        host.pump();
        if (!host.getLastError().empty())
            return ::testing::AssertionFailure() << "host failed: " << host.getLastError();

        client.requestConnect("127.0.0.1", sPort);

        const auto deadline = std::chrono::steady_clock::now() + sTimeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            pumpBoth(host, client);
            if (client.isConnected() && !host.getPeers().empty())
                return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "peers did not connect within the timeout";
    }

    // Drains both sides for a while, collecting whatever the host was given.
    std::vector<Event> exchange(Session& host, Session& client, int rounds = 60)
    {
        std::vector<Event> collected;
        for (int i = 0; i < rounds; ++i)
        {
            pumpBoth(host, client);
            for (Event& event : host.drainEvents())
                collected.push_back(std::move(event));
        }
        return collected;
    }

    TEST(NetSessionTest, shouldDeliverGameplayMessagesOnTheDefaultChannels)
    {
        Session host;
        Session client;
        ASSERT_TRUE(connectPair(host, client));

        client.send(Session::sServerPeerId, 0, Delivery::Reliable, "reliable");
        client.send(Session::sServerPeerId, 1, Delivery::Unreliable, "unreliable");

        int seen = 0;
        for (const Event& event : exchange(host, client))
        {
            if (event.mType == Event::Type::Message && (event.mData == "reliable" || event.mData == "unreliable"))
                ++seen;
        }
        EXPECT_EQ(seen, 2);
    }

    TEST(NetSessionTest, shouldDivertAClaimedChannelAwayFromTheEventQueue)
    {
        Session host;
        Session client;

        std::atomic<int> handled{ 0 };
        std::string received;
        std::uint32_t origin = 0xFFFFFFFFu;
        host.setChannelHandler(2, [&](std::uint32_t peer, std::string_view data) {
            received.assign(data);
            origin = peer;
            handled.fetch_add(1);
        });

        ASSERT_TRUE(connectPair(host, client));

        client.send(Session::sServerPeerId, 2, Delivery::Unsequenced, "voice");
        client.send(Session::sServerPeerId, 0, Delivery::Reliable, "gameplay");

        std::vector<Event> events = exchange(host, client);

        EXPECT_EQ(handled.load(), 1);
        EXPECT_EQ(received, "voice");
        // The relay stamps the authoritative origin from this, so it has to be
        // the sending peer and not the server's own id.
        EXPECT_NE(origin, Session::sServerPeerId);

        // The whole point of claiming a channel: nothing on it reaches the
        // consumer that gameplay messages go to.
        for (const Event& event : events)
            EXPECT_NE(event.mData, "voice");

        const bool sawGameplay = std::any_of(events.begin(), events.end(),
            [](const Event& event) { return event.mData == "gameplay"; });
        EXPECT_TRUE(sawGameplay) << "claiming a channel must not disturb the others";
    }

    TEST(NetSessionTest, shouldReleaseAClaimedChannelBackToTheEventQueue)
    {
        Session host;
        Session client;

        std::atomic<int> handled{ 0 };
        host.setChannelHandler(2, [&](std::uint32_t, std::string_view) { handled.fetch_add(1); });
        ASSERT_TRUE(connectPair(host, client));

        host.setChannelHandler(2, {});
        client.send(Session::sServerPeerId, 2, Delivery::Unsequenced, "after release");

        bool seen = false;
        for (const Event& event : exchange(host, client))
            seen = seen || event.mData == "after release";

        EXPECT_EQ(handled.load(), 0);
        EXPECT_TRUE(seen);
    }

    TEST(NetSessionTest, shouldDropTheOldestQueuedMessageWhenAChannelIsCapped)
    {
        Session session;
        session.setChannelSendLimit(2, 3);

        // Never pumped, so nothing drains: this is the loading-screen case,
        // where the producer keeps going and the queue must not.
        for (int i = 0; i < 10; ++i)
            session.send(Session::sServerPeerId, 2, Delivery::Unsequenced, std::to_string(i));

        EXPECT_EQ(session.droppedSends(2), 7u);
        // An uncapped channel is unaffected by another channel's cap.
        for (int i = 0; i < 10; ++i)
            session.send(Session::sServerPeerId, 0, Delivery::Reliable, "gameplay");
        EXPECT_EQ(session.droppedSends(0), 0u);
    }

    TEST(NetSessionTest, shouldSurviveAFloodOfOutOfRangeChannelSends)
    {
        // Note what this does NOT cover. The interesting case is a peer built
        // before channel 2 existed: ENet negotiates the count down silently, a
        // send to the missing channel is refused, and a refused send hands the
        // packet back rather than freeing it - which is why sendOne now checks
        // the result. Two sessions from the same build always agree on three
        // channels, and an out-of-range channel is clamped before it is sent,
        // so that path cannot be reached from here at all. Reaching it needs a
        // genuinely older binary, which is an M2 in-game acceptance item.
        //
        // What is left is still worth asserting: a flood of clamped sends does
        // not corrupt the session or wedge the pump.
        Session host;
        Session client;
        ASSERT_TRUE(connectPair(host, client));

        const std::uint32_t peer = host.getPeers().front();
        for (int i = 0; i < 200; ++i)
            host.send(peer, Session::sMaxChannels - 1, Delivery::Unsequenced, std::string(200, 'x'));

        exchange(host, client);

        // Still usable afterwards, which is the observable part of not having
        // corrupted anything.
        host.send(peer, 0, Delivery::Reliable, "still alive");
        bool seen = false;
        for (int i = 0; i < 60 && !seen; ++i)
        {
            pumpBoth(host, client);
            for (const Event& event : client.drainEvents())
                seen = seen || event.mData == "still alive";
        }
        EXPECT_TRUE(seen);
    }
}
