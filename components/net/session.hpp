#ifndef COMPONENTS_NET_SESSION_H
#define COMPONENTS_NET_SESSION_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Net
{
    // How a packet is put on the wire. This is what `bool reliable` used to be,
    // widened because the middle case turns out not to be the one voice wants.
    //
    //  Reliable    ENET_PACKET_FLAG_RELIABLE. Retransmitted, delivered in order.
    //  Unreliable  ENet's default, and not what the name suggests: it is
    //              unreliable-SEQUENCED. Every such packet carries a per-channel
    //              sequence number, and the receiver discards any that is not
    //              newer than the last one it dispatched on that channel, inside
    //              ENet, before the application is told anything. A packet one
    //              slot late does not arrive at all, and neither does the
    //              in-band FEC it was carrying for its predecessor.
    //  Unsequenced ENET_PACKET_FLAG_UNSEQUENCED. Delivered in arrival order,
    //              late packets included; only exact duplicates are suppressed.
    //              The only class that hands a reordered stream to the
    //              application, which is the input a jitter buffer exists to
    //              take.
    //
    // Size trap: ENet tests the payload length before it looks at the flags, and
    // its fragment path knows only the reliable and unreliable-fragment cases.
    // An unsequenced packet past roughly 1370 bytes therefore goes out as a
    // reliable, ordered, retransmitted fragment set. Voice frames must stay
    // small, and the relay clamp is what enforces that.
    enum class Delivery
    {
        Reliable,
        Unreliable,
        Unsequenced,
    };

    enum class Role
    {
        None,
        Host,
        Client,
    };

    struct Event
    {
        enum class Type
        {
            Connect,
            Disconnect,
            Message,
        };
        Type mType;
        std::uint32_t mPeer = 0;
        std::uint8_t mChannel = 0;
        std::string mData; // payload, only set for Message
    };

    // Thread-safe wrapper around an ENet host.
    //
    // All methods except pump() only touch mutex-guarded queues and state snapshots, so
    // they may be called from the Lua worker thread. All actual networking happens in
    // pump(), which must be called from a single thread (the main thread, at frame
    // start, see LuaManager::synchronizedUpdate).
    class Session
    {
    public:
        // Peer id that addresses the server when running as a client.
        static constexpr std::uint32_t sServerPeerId = 0;

        // Channels negotiated at connect: 0 reliable gameplay, 1 unreliable
        // gameplay, 2 opaque bulk, which is voice. ENet negotiates the count
        // DOWN to whichever end asked for fewer and says nothing about it, so a
        // peer built before channel 2 existed connects and plays normally and
        // simply has no channel 2. Making that visible is the capability
        // handshake's job, not the transport's.
        static constexpr std::uint8_t sChannelCount = 3;
        // Upper bound for the channel-indexed tables below, kept larger than
        // sChannelCount so adding a channel is one constant and not a resize.
        static constexpr std::uint8_t sMaxChannels = 4;

        // Called for every message arriving on a claimed channel, INSTEAD of
        // that message being queued for drainEvents. Runs on the pump thread,
        // with no lock held, over a view of the packet that is valid only for
        // the duration of the call. It must not call pump(), and must not throw.
        //
        // Whatever the handler captures has to outlive the last pump that can
        // reach it, so release it from the pump thread.
        using ChannelHandler = std::function<void(std::uint32_t peer, std::string_view data)>;

        Session();
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Control requests, executed on the next pump().
        void requestHost(std::uint16_t port, unsigned maxPeers);
        void requestConnect(std::string address, std::uint16_t port);
        void requestDisconnect();
        // Host only: drop one peer, gracefully, so anything already queued for
        // them (a refusal and its reason) still reaches them first.
        void requestKick(std::uint32_t peer);

        // State snapshot, updated by pump().
        Role getRole() const;
        bool isConnected() const;
        std::vector<std::uint32_t> getPeers() const;
        std::string getLastError() const;

        // Queue an outgoing message. As a client `to` is ignored (always the server).
        void send(std::uint32_t to, std::uint8_t channel, Delivery delivery, std::string data);
        void broadcast(std::uint8_t channel, Delivery delivery, std::string data);

        // The pre-existing spelling, kept so gameplay call sites that only ever
        // meant "reliable or not" do not have to care that a third class exists.
        void send(std::uint32_t to, std::uint8_t channel, bool reliable, std::string data)
        {
            send(to, channel, reliable ? Delivery::Reliable : Delivery::Unreliable, std::move(data));
        }
        void broadcast(std::uint8_t channel, bool reliable, std::string data)
        {
            broadcast(channel, reliable ? Delivery::Reliable : Delivery::Unreliable, std::move(data));
        }

        // Claim a channel. Safe from any thread; takes effect on the next pump.
        // Passing an empty handler releases the channel back to drainEvents.
        void setChannelHandler(std::uint8_t channel, ChannelHandler handler);

        // Cap on how many messages may sit queued for one channel, so a stream
        // that keeps producing while the pump is stalled behind a loading screen
        // cannot turn into minutes of backlog to be played out afterwards. Zero
        // means unlimited, which is what channels 0 and 1 stay. When the cap is
        // reached the OLDEST message on that channel is dropped: for audio the
        // stale end of the queue is the worthless end.
        void setChannelSendLimit(std::uint8_t channel, std::size_t maxQueued);
        std::uint64_t droppedSends(std::uint8_t channel) const;

        // Take all events received since the last call.
        std::vector<Event> drainEvents();

        // Main thread only: execute control requests, flush sends, service the socket.
        void pump();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif // COMPONENTS_NET_SESSION_H
