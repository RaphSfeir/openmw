#include "session.hpp"

#include <algorithm>
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
        constexpr std::size_t sNumChannels = 2;
        constexpr int sMaxEventsPerPump = 256;
        constexpr std::chrono::seconds sConnectTimeout(8);
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
            bool mReliable;
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
            const std::uint32_t flags = out.mReliable ? ENET_PACKET_FLAG_RELIABLE : 0;
            ENetPacket* packet = enet_packet_create(out.mData.data(), out.mData.size(), flags);
            if (packet == nullptr)
                return;
            if (mLiveRole == Role::Client)
            {
                if (mServerPeer != nullptr && mLiveConnected)
                    enet_peer_send(mServerPeer, channel, packet);
                else
                    enet_packet_destroy(packet);
            }
            else if (mLiveRole == Role::Host)
            {
                auto it = mPeers.find(out.mTo);
                if (it != mPeers.end())
                    enet_peer_send(it->second, channel, packet);
                else
                    enet_packet_destroy(packet);
            }
            else
                enet_packet_destroy(packet);
        }

        void broadcastOne(const Outgoing& out)
        {
            if (mHost == nullptr || mLiveRole != Role::Host)
                return;
            const std::uint8_t channel = std::min<std::uint8_t>(out.mChannel, sNumChannels - 1);
            const std::uint32_t flags = out.mReliable ? ENET_PACKET_FLAG_RELIABLE : 0;
            ENetPacket* packet = enet_packet_create(out.mData.data(), out.mData.size(), flags);
            if (packet != nullptr)
                enet_host_broadcast(mHost, channel, packet);
        }

        void service()
        {
            if (mHost == nullptr)
                return;
            ENetEvent event;
            for (int i = 0; i < sMaxEventsPerPump && enet_host_service(mHost, &event, 0) > 0; ++i)
            {
                switch (event.type)
                {
                    case ENET_EVENT_TYPE_CONNECT:
                    {
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

    void Session::send(std::uint32_t to, std::uint8_t channel, bool reliable, std::string data)
    {
        std::lock_guard lock(mImpl->mMutex);
        mImpl->mOutQueue.push_back(Outgoing{ to, channel, reliable, std::move(data) });
    }

    void Session::broadcast(std::uint8_t channel, bool reliable, std::string data)
    {
        // Broadcast is encoded as a send to the (invalid as a target) server id with a marker
        // channel bit; use a dedicated queue entry flag instead: reuse Outgoing with mTo set
        // to the broadcast marker.
        std::lock_guard lock(mImpl->mMutex);
        mImpl->mOutQueue.push_back(Outgoing{ sBroadcastMarker, channel, reliable, std::move(data) });
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
        for (const Outgoing& outgoing : out)
        {
            if (outgoing.mTo == sBroadcastMarker && mImpl->mLiveRole == Role::Host)
                mImpl->broadcastOne(outgoing);
            else
                mImpl->sendOne(outgoing);
        }
        mImpl->service();
        mImpl->checkConnectTimeout();
        mImpl->updateSnapshot();
    }
}
