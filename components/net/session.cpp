#include "session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <variant>

#include <enet/enet.h>

#include <components/debug/debuglog.hpp>

namespace Net
{
    namespace
    {
        constexpr std::size_t sNumChannels = Session::sChannelCount;
        constexpr int sMaxEventsPerPump = 256;
        // Diverted messages do not spend the gameplay budget, but they still
        // have to be bounded or a flood on a claimed channel would pin the pump
        // thread for as long as the sender cared to keep going.
        constexpr int sMaxDivertedPerPump = 1024;

        std::uint32_t packetFlags(Delivery delivery)
        {
            switch (delivery)
            {
                case Delivery::Reliable:
                    return ENET_PACKET_FLAG_RELIABLE;
                case Delivery::Unreliable:
                    return 0;
                case Delivery::Unsequenced:
                    return ENET_PACKET_FLAG_UNSEQUENCED;
            }
            return 0;
        }

        constexpr std::chrono::seconds sConnectTimeout(8);
        // How long a live peer may go silent before the link is abandoned. See the
        // note at the ENET_EVENT_TYPE_CONNECT case: ENet's own default works out to a
        // flat five seconds on any realistic link, which a loading game client
        // exceeds routinely and innocently.
        constexpr std::uint32_t sTimeoutLimit = 32;
        constexpr std::uint32_t sTimeoutMinimumMs = 20000;
        constexpr std::uint32_t sTimeoutMaximumMs = 40000;
        // Outgoing::mTo value that requests a broadcast to all peers (host only).
        constexpr std::uint32_t sBroadcastMarker = 0xFFFFFFFFu;

        std::mutex sEnetInitMutex;
        int sEnetInitCount = 0;

        struct HostRequest
        {
            std::uint16_t mPort;
            unsigned mMaxPeers;
        };

        struct ConnectRequest
        {
            std::string mAddress;
            std::uint16_t mPort;
        };

        struct DisconnectRequest
        {
        };

        // Drop ONE peer, host only. Ending a whole session is a different thing
        // and has its own request; this is what a server needs to be able to
        // turn somebody away and mean it.
        struct KickRequest
        {
            std::uint32_t mPeer;
        };

        using ControlRequest = std::variant<HostRequest, ConnectRequest, DisconnectRequest, KickRequest>;

        struct Outgoing
        {
            std::uint32_t mTo;
            std::uint8_t mChannel;
            Delivery mDelivery;
            std::string mData;
        };
    }

    struct Session::Impl
    {
        // Guards everything below up to the "pump-thread only" section.
        mutable std::mutex mMutex;
        std::deque<ControlRequest> mControlQueue;
        std::deque<Outgoing> mOutQueue;
        std::vector<Event> mInEvents;
        Role mRole = Role::None;
        bool mConnected = false;
        std::vector<std::uint32_t> mPeerIds;
        std::string mLastError;

        // Per-channel outgoing caps and their drop counters, guarded by mMutex
        // alongside mOutQueue.
        std::array<std::size_t, Session::sMaxChannels> mOutLimit{};
        std::array<std::size_t, Session::sMaxChannels> mOutCount{};
        std::array<std::uint64_t, Session::sMaxChannels> mOutDropped{};

        // Copy-on-write so the pump can take one snapshot per tick instead of
        // locking per packet, and so a handler stays alive for the duration of
        // a call that a concurrent swap would otherwise pull out from under it.
        using HandlerTable = std::array<Session::ChannelHandler, Session::sMaxChannels>;
        mutable std::mutex mHandlerMutex;
        std::shared_ptr<const HandlerTable> mHandlers;

        std::shared_ptr<const HandlerTable> snapshotHandlers() const
        {
            const std::lock_guard<std::mutex> lock(mHandlerMutex);
            return mHandlers;
        }

        // Pump-thread only.
        ENetHost* mHost = nullptr;
        Role mLiveRole = Role::None;
        bool mLiveConnected = false;
        std::map<std::uint32_t, ENetPeer*> mPeers;
        std::uint32_t mNextPeerId = 1;
        ENetPeer* mServerPeer = nullptr;
        std::optional<std::chrono::steady_clock::time_point> mConnectDeadline;

        void teardown()
        {
            if (mHost != nullptr)
            {
                enet_host_destroy(mHost);
                mHost = nullptr;
            }
            mLiveRole = Role::None;
            mLiveConnected = false;
            mPeers.clear();
            mServerPeer = nullptr;
            mConnectDeadline = std::nullopt;
        }

        void pushEvent(Event event)
        {
            std::lock_guard lock(mMutex);
            mInEvents.push_back(std::move(event));
        }

        void setError(std::string message)
        {
            Log(Debug::Warning) << "Net::Session: " << message;
            std::lock_guard lock(mMutex);
            mLastError = std::move(message);
        }

        void startHost(const HostRequest& request)
        {
            teardown();
            ENetAddress address;
            address.host = ENET_HOST_ANY;
            address.port = request.mPort;
            mHost = enet_host_create(&address, std::max(1u, request.mMaxPeers), sNumChannels, 0, 0);
            if (mHost == nullptr)
            {
                setError("failed to create host on port " + std::to_string(request.mPort));
                return;
            }
            mLiveRole = Role::Host;
            mLiveConnected = true; // listening
            Log(Debug::Info) << "Net::Session: hosting on port " << request.mPort;
        }

        void startConnect(const ConnectRequest& request)
        {
            teardown();
            mHost = enet_host_create(nullptr, 1, sNumChannels, 0, 0);
            if (mHost == nullptr)
            {
                setError("failed to create client socket");
                return;
            }
            ENetAddress address;
            if (enet_address_set_host(&address, request.mAddress.c_str()) != 0)
            {
                teardown();
                setError("failed to resolve address: " + request.mAddress);
                return;
            }
            address.port = request.mPort;
            mServerPeer = enet_host_connect(mHost, &address, sNumChannels, 0);
            if (mServerPeer == nullptr)
            {
                teardown();
                setError("failed to initiate connection to " + request.mAddress);
                return;
            }
            mLiveRole = Role::Client;
            mLiveConnected = false;
            mConnectDeadline = std::chrono::steady_clock::now() + sConnectTimeout;
            Log(Debug::Info) << "Net::Session: connecting to " << request.mAddress << ":" << request.mPort;
        }

        void doDisconnect()
        {
            if (mLiveRole == Role::Client && mServerPeer != nullptr && mLiveConnected)
                enet_peer_disconnect_now(mServerPeer, 0);
            else if (mLiveRole == Role::Host && mHost != nullptr)
            {
                for (const auto& [id, peer] : mPeers)
                    enet_peer_disconnect_now(peer, 0);
                enet_host_flush(mHost);
            }
            bool wasActive = mLiveRole != Role::None;
            teardown();
            if (wasActive)
                pushEvent(Event{ Event::Type::Disconnect, sServerPeerId, 0, {} });
        }

        // One peer, gone. Host only — a client has exactly one connection and
        // ending it is requestDisconnect. Deliberately the graceful form:
        // disconnect_now would drop the link before the queued reason has left
        // the wire, so the peer would be turned away without ever being told
        // why. ENet delivers the disconnect event itself, so the session's own
        // bookkeeping happens on the ordinary path rather than here.
        void doKick(std::uint32_t id)
        {
            if (mLiveRole != Role::Host || mHost == nullptr)
                return;
            const auto it = mPeers.find(id);
            if (it == mPeers.end())
                return;
            enet_host_flush(mHost);
            enet_peer_disconnect(it->second, 0);
            enet_host_flush(mHost);
        }

        std::uint32_t peerId(ENetPeer* peer) const
        {
            return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(peer->data));
        }

        void sendOne(const Outgoing& out)
        {
            if (mHost == nullptr)
                return;
            const std::uint8_t channel = std::min<std::uint8_t>(out.mChannel, sNumChannels - 1);
            ENetPacket* packet = enet_packet_create(out.mData.data(), out.mData.size(), packetFlags(out.mDelivery));
            if (packet == nullptr)
                return;

            // enet_peer_send hands the packet back on failure rather than
            // freeing it, and it fails for an ordinary reason now that there is
            // a channel not every peer has: a build made before channel 2
            // existed negotiates two channels, and everything aimed at its
            // third one is refused. Ignoring the result there would leak a
            // packet and its payload fifty times a second per such peer.
            ENetPeer* target = nullptr;
            if (mLiveRole == Role::Client)
            {
                if (mServerPeer != nullptr && mLiveConnected)
                    target = mServerPeer;
            }
            else if (mLiveRole == Role::Host)
            {
                auto it = mPeers.find(out.mTo);
                if (it != mPeers.end())
                    target = it->second;
            }

            if (target == nullptr || enet_peer_send(target, channel, packet) < 0)
                enet_packet_destroy(packet);
        }

        void broadcastOne(const Outgoing& out)
        {
            if (mHost == nullptr || mLiveRole != Role::Host)
                return;
            const std::uint8_t channel = std::min<std::uint8_t>(out.mChannel, sNumChannels - 1);
            ENetPacket* packet = enet_packet_create(out.mData.data(), out.mData.size(), packetFlags(out.mDelivery));
            // Unlike enet_peer_send, the broadcast path disposes of a packet no
            // peer accepted, so there is nothing to clean up here.
            if (packet != nullptr)
                enet_host_broadcast(mHost, channel, packet);
        }

        void service(const std::shared_ptr<const HandlerTable>& handlers)
        {
            if (mHost == nullptr)
                return;
            ENetEvent event;
            // Two budgets, because a claimed channel must neither starve
            // gameplay nor be starved by it. Gameplay is checked before an event
            // is taken off ENet, so nothing is ever dequeued and thrown away;
            // diverted messages spend only the larger total.
            int gameplay = 0;
            int total = 0;
            while (gameplay < sMaxEventsPerPump && total < sMaxEventsPerPump + sMaxDivertedPerPump
                && enet_host_service(mHost, &event, 0) > 0)
            {
                ++total;
                if (event.type == ENET_EVENT_TYPE_RECEIVE && handlers != nullptr
                    && event.channelID < Session::sMaxChannels && (*handlers)[event.channelID])
                {
                    const std::uint32_t id = mLiveRole == Role::Host ? peerId(event.peer) : sServerPeerId;
                    try
                    {
                        (*handlers)[event.channelID](id,
                            std::string_view(
                                reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength));
                    }
                    catch (const std::exception& e)
                    {
                        // An escaping exception would skip the destroy below and
                        // leak the packet, once per message, for as long as
                        // whatever is wrong keeps being wrong.
                        Log(Debug::Error) << "Net: channel " << static_cast<int>(event.channelID)
                                          << " handler threw: " << e.what();
                    }
                    enet_packet_destroy(event.packet);
                    continue;
                }

                ++gameplay;
                switch (event.type)
                {
                    case ENET_EVENT_TYPE_CONNECT:
                    {
                        // Give a peer room to stall before we give up on it.
                        //
                        // ENet's default is roundTripTime * ENET_PEER_TIMEOUT_LIMIT,
                        // clamped to a 5 second MINIMUM. Round trip time is ~0 on a
                        // local link and ~30ms over the internet, so that product is
                        // always far below the floor and every connection in practice
                        // gets exactly five seconds. Five seconds is nothing to a
                        // Morrowind client: loading a city interior in a heavy modlist
                        // blocks the main loop — and therefore the ENet pump — for
                        // longer than that routinely. The peer was then dropped for
                        // being busy, mid-session, having done nothing wrong.
                        //
                        // A game is not a chat protocol. Being slow is normal here and
                        // being gone is not, so wait properly before concluding the
                        // second: 20 seconds of complete silence before a healthy link
                        // is abandoned, 40 at the outside.
                        enet_peer_timeout(event.peer, sTimeoutLimit, sTimeoutMinimumMs, sTimeoutMaximumMs);
                        std::uint32_t id = sServerPeerId;
                        if (mLiveRole == Role::Host)
                        {
                            id = mNextPeerId++;
                            event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
                            mPeers.emplace(id, event.peer);
                        }
                        else
                        {
                            mLiveConnected = true;
                            mConnectDeadline = std::nullopt;
                        }
                        pushEvent(Event{ Event::Type::Connect, id, 0, {} });
                        break;
                    }
                    case ENET_EVENT_TYPE_RECEIVE:
                    {
                        const std::uint32_t id = mLiveRole == Role::Host ? peerId(event.peer) : sServerPeerId;
                        std::string data(reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength);
                        pushEvent(Event{ Event::Type::Message, id, static_cast<std::uint8_t>(event.channelID),
                            std::move(data) });
                        enet_packet_destroy(event.packet);
                        break;
                    }
                    case ENET_EVENT_TYPE_DISCONNECT:
                    {
                        if (mLiveRole == Role::Host)
                        {
                            const std::uint32_t id = peerId(event.peer);
                            mPeers.erase(id);
                            pushEvent(Event{ Event::Type::Disconnect, id, 0, {} });
                        }
                        else
                        {
                            teardown();
                            pushEvent(Event{ Event::Type::Disconnect, sServerPeerId, 0, {} });
                        }
                        break;
                    }
                    default:
                        break;
                }
                if (mHost == nullptr)
                    break; // teardown happened while handling an event
            }
        }

        void checkConnectTimeout()
        {
            if (mLiveRole == Role::Client && !mLiveConnected && mConnectDeadline
                && std::chrono::steady_clock::now() > *mConnectDeadline)
            {
                teardown();
                setError("connection attempt timed out");
                pushEvent(Event{ Event::Type::Disconnect, sServerPeerId, 0, {} });
            }
        }

        void updateSnapshot()
        {
            std::vector<std::uint32_t> ids;
            ids.reserve(mPeers.size());
            for (const auto& [id, peer] : mPeers)
                ids.push_back(id);
            std::lock_guard lock(mMutex);
            mRole = mLiveRole;
            mConnected = mLiveConnected;
            mPeerIds = std::move(ids);
        }
    };

    Session::Session()
        : mImpl(std::make_unique<Impl>())
    {
        std::lock_guard lock(sEnetInitMutex);
        if (sEnetInitCount++ == 0 && enet_initialize() != 0)
            Log(Debug::Error) << "Net::Session: failed to initialize ENet";
    }

    Session::~Session()
    {
        mImpl->teardown();
        std::lock_guard lock(sEnetInitMutex);
        if (--sEnetInitCount == 0)
            enet_deinitialize();
    }

    void Session::requestHost(std::uint16_t port, unsigned maxPeers)
    {
        std::lock_guard lock(mImpl->mMutex);
        mImpl->mControlQueue.push_back(HostRequest{ port, maxPeers });
    }

    void Session::requestConnect(std::string address, std::uint16_t port)
    {
        std::lock_guard lock(mImpl->mMutex);
        mImpl->mControlQueue.push_back(ConnectRequest{ std::move(address), port });
    }

    void Session::requestDisconnect()
    {
        std::lock_guard lock(mImpl->mMutex);
        mImpl->mControlQueue.push_back(DisconnectRequest{});
    }

    void Session::requestKick(std::uint32_t peer)
    {
        std::lock_guard lock(mImpl->mMutex);
        mImpl->mControlQueue.push_back(KickRequest{ peer });
    }

    Role Session::getRole() const
    {
        std::lock_guard lock(mImpl->mMutex);
        return mImpl->mRole;
    }

    bool Session::isConnected() const
    {
        std::lock_guard lock(mImpl->mMutex);
        return mImpl->mConnected;
    }

    std::vector<std::uint32_t> Session::getPeers() const
    {
        std::lock_guard lock(mImpl->mMutex);
        return mImpl->mPeerIds;
    }

    std::string Session::getLastError() const
    {
        std::lock_guard lock(mImpl->mMutex);
        return mImpl->mLastError;
    }

    void Session::send(std::uint32_t to, std::uint8_t channel, Delivery delivery, std::string data)
    {
        std::lock_guard lock(mImpl->mMutex);

        if (channel < sMaxChannels && mImpl->mOutLimit[channel] != 0)
        {
            while (mImpl->mOutCount[channel] >= mImpl->mOutLimit[channel])
            {
                // Drop from the front of this channel, not the back. A caller
                // that hit the cap is producing faster than the pump can drain,
                // and for a stream the oldest entry is the one least worth
                // keeping: playing out a loading screen's worth of stale speech
                // is worse than losing it.
                const auto stale = std::find_if(mImpl->mOutQueue.begin(), mImpl->mOutQueue.end(),
                    [channel](const Outgoing& queued) { return queued.mChannel == channel; });
                if (stale == mImpl->mOutQueue.end())
                {
                    mImpl->mOutCount[channel] = 0;
                    break;
                }
                mImpl->mOutQueue.erase(stale);
                --mImpl->mOutCount[channel];
                ++mImpl->mOutDropped[channel];
            }
            ++mImpl->mOutCount[channel];
        }

        mImpl->mOutQueue.push_back(Outgoing{ to, channel, delivery, std::move(data) });
    }

    void Session::broadcast(std::uint8_t channel, Delivery delivery, std::string data)
    {
        // Broadcast is encoded as a send to the broadcast marker rather than a
        // real peer id, so it goes through send() and inherits the same cap.
        send(sBroadcastMarker, channel, delivery, std::move(data));
    }

    void Session::setChannelHandler(std::uint8_t channel, ChannelHandler handler)
    {
        if (channel >= sMaxChannels)
            return;

        const std::lock_guard<std::mutex> lock(mImpl->mHandlerMutex);
        auto table = mImpl->mHandlers != nullptr ? std::make_shared<Impl::HandlerTable>(*mImpl->mHandlers)
                                                 : std::make_shared<Impl::HandlerTable>();
        (*table)[channel] = std::move(handler);
        mImpl->mHandlers = std::move(table);
    }

    void Session::setChannelSendLimit(std::uint8_t channel, std::size_t maxQueued)
    {
        if (channel >= sMaxChannels)
            return;
        std::lock_guard lock(mImpl->mMutex);
        mImpl->mOutLimit[channel] = maxQueued;
    }

    std::uint64_t Session::droppedSends(std::uint8_t channel) const
    {
        if (channel >= sMaxChannels)
            return 0;
        std::lock_guard lock(mImpl->mMutex);
        return mImpl->mOutDropped[channel];
    }

    std::vector<Event> Session::drainEvents()
    {
        std::lock_guard lock(mImpl->mMutex);
        std::vector<Event> events = std::move(mImpl->mInEvents);
        mImpl->mInEvents.clear();
        return events;
    }

    void Session::pump()
    {
        std::deque<ControlRequest> control;
        std::deque<Outgoing> out;
        {
            std::lock_guard lock(mImpl->mMutex);
            control.swap(mImpl->mControlQueue);
            out.swap(mImpl->mOutQueue);
            mImpl->mOutCount.fill(0);
        }
        for (const ControlRequest& request : control)
        {
            if (std::holds_alternative<HostRequest>(request))
                mImpl->startHost(std::get<HostRequest>(request));
            else if (std::holds_alternative<ConnectRequest>(request))
                mImpl->startConnect(std::get<ConnectRequest>(request));
            else if (std::holds_alternative<KickRequest>(request))
                mImpl->doKick(std::get<KickRequest>(request).mPeer);
            else
                mImpl->doDisconnect();
        }
        const auto flush = [this](const std::deque<Outgoing>& queue) {
            for (const Outgoing& outgoing : queue)
            {
                if (outgoing.mTo == sBroadcastMarker && mImpl->mLiveRole == Role::Host)
                    mImpl->broadcastOne(outgoing);
                else
                    mImpl->sendOne(outgoing);
            }
        };

        flush(out);
        mImpl->service(mImpl->snapshotHandlers());

        // A channel handler is where a relay lives, so most of what it produces
        // is queued during the service() above. Draining once more here is what
        // keeps a relayed packet from waiting a whole tick for the next pump.
        {
            std::deque<Outgoing> relayed;
            {
                std::lock_guard lock(mImpl->mMutex);
                relayed.swap(mImpl->mOutQueue);
                mImpl->mOutCount.fill(0);
            }
            flush(relayed);

            // enet_peer_send only queues onto the peer's outgoing command list;
            // the datagram itself leaves when the host is serviced, and
            // service() above has already had its turn. Without this the frame
            // a handler just produced would sit until the next pump, which is
            // the whole latency this second drain was meant to avoid. The host
            // can be gone by now: a disconnect handled inside service() tears
            // it down.
            if (!relayed.empty() && mImpl->mHost != nullptr)
                enet_host_flush(mImpl->mHost);
        }

        mImpl->checkConnectTimeout();
        mImpl->updateSnapshot();
    }
}
