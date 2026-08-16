#ifndef COMPONENTS_NET_SESSION_H
#define COMPONENTS_NET_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Net
{
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
        void send(std::uint32_t to, std::uint8_t channel, bool reliable, std::string data);
        void broadcast(std::uint8_t channel, bool reliable, std::string data);

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
