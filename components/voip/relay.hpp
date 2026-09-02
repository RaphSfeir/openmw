#ifndef OPENMW_COMPONENTS_VOIP_RELAY_H
#define OPENMW_COMPONENTS_VOIP_RELAY_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wire.hpp"

namespace Voip
{
    // Structural ceiling on tracked peers: the table is a fixed array, so it
    // cannot grow at all rather than growing until something checks it. Three
    // requirements meet here - peer ids never repeat within a process, so a map
    // that is only inserted into is unbounded under reconnect churn; the box
    // runs with systemd MemoryMax=400M; and the packet path must not allocate.
    inline constexpr std::size_t sMaxRelayPeers = 64;

    struct RelayLimits
    {
        // Whole datagram, header included. Clamped to sMaxPacketBytes by the
        // constructor, because the scratch buffer a datagram is stamped in is
        // that size: a larger clamp would be an overrun, not a larger packet.
        std::size_t mMaxPacketBytes = sMaxPacketBytes;
        // 50 pps is the design rate; 60 leaves room for a client whose pump
        // quantises two frames into one tick, and the burst absorbs a ~300 ms
        // stall being flushed at once rather than punishing it.
        double mPacketsPerSecond = 60.0;
        double mBurstPackets = 90.0;
        // Hard ceilings on per-peer state. Nothing in here grows past these no
        // matter what a peer or a Lua module does. mMaxPeers is clamped to
        // sMaxRelayPeers by the constructor for the same reason as the size.
        std::size_t mMaxPeers = sMaxRelayPeers;
        std::size_t mMaxRouteTargets = 64;
    };

    // The substitute for logging. At 50 pps per speaker a per-packet log line is
    // a denial of service against journald, so every rejection lands in exactly
    // one of these and network.voipStats() is how anybody ever sees it.
    struct RelayCounters
    {
        std::uint64_t mReceived = 0; // datagrams handed to onPacket
        std::uint64_t mForwarded = 0; // datagrams handed to the sink, so fan-out is counted per recipient
        std::uint64_t mBytesIn = 0;
        std::uint64_t mBytesOut = 0;
        std::uint64_t mDroppedDisabled = 0;
        std::uint64_t mDroppedNotCapable = 0; // explicitly marked incapable, see setCapable
        std::uint64_t mDroppedMalformed = 0; // shorter than a header
        std::uint64_t mDroppedVersion = 0;
        std::uint64_t mDroppedOversize = 0;
        std::uint64_t mDroppedRate = 0;
        std::uint64_t mDroppedNoRoute = 0; // nobody to send it to, including no sink
        // Sender wrote a speaker id of its own. Counted and OVERWRITTEN, not
        // dropped: dropping lets one malformed client silence itself invisibly,
        // overwriting makes spoofing impossible and leaves the evidence here.
        std::uint64_t mSpoofedOrigin = 0;
        std::uint64_t mPrunedPeers = 0;
        // Peer table or route table full. A datagram from a peer the full table
        // cannot track is dropped and counted here rather than under a dropped*
        // name, because the cause is the cap and not the packet.
        std::uint64_t mRefusedPeers = 0;
    };

    // The opaque voice relay: validate, rate-limit, stamp the authoritative
    // origin, fan out. It reads bytes 0..7 of a datagram and writes bytes 4..7.
    // Bytes 8.. are copied and never inspected, which is the whole reason this
    // lives in components and the dedicated server stays audio-free at link
    // level. Nothing in here names a sample rate, a channel count, a frame
    // duration or a codec.
    //
    // Holds no socket and no clock, for the same reasons JitterBuffer does not:
    // the sink is a callback so a test can be a vector, and time enters at
    // exactly two places, both of them arguments.
    //
    // NOT thread-safe, and on the dedicated server it does not need to be: the
    // pump and the Lua tick are the same thread there. A LISTEN HOST is
    // different - mwlua dispatches Lua on a worker thread while pump is
    // main-thread (session.hpp) - so the client side must serialise its own
    // calls into this object.
    class Relay
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;
        // (destination peer, whole datagram with the rewritten header). The
        // caller wires this to Net::Session::send on sVoiceChannel with
        // Net::Delivery::Unsequenced. The bytes are borrowed: valid only for the
        // duration of the call.
        using Sink = std::function<void(std::uint32_t, const unsigned char*, std::size_t)>;

        explicit Relay(const RelayLimits& limits = {});

        void setSink(Sink sink);

        // Default OFF. Voice is a policy the control plane turns on, so a server
        // whose Lua never mentions voice relays nothing.
        void setEnabled(bool enabled);
        bool enabled() const { return mEnabled; }

        // The capability gate. A peer marked incapable has its channel-2 traffic
        // dropped before its bytes are read, and is never a fan-out target.
        //
        // A peer nobody has mentioned counts as CAPABLE. That default is
        // deliberate and temporary: the gate is meant to key off the 'voi'
        // capability handshake, and that message type does not exist in the wire
        // spec yet, so defaulting to incapable would relay nothing at all and
        // fail the milestone for a reason that has nothing to do with this code.
        // Once 'voi' exists the announce is what opens the gate and the default
        // becomes incapable.
        //
        // Defaulting open is only safe because the transport now destroys a
        // packet no peer accepted: a build from before channel 2 existed
        // negotiates two channels, refuses every voice send, and used to leak
        // the packet each time.
        //
        // Returns false when the peer table is full and this peer is not in it.
        bool setCapable(std::uint32_t peer, bool capable);
        bool capable(std::uint32_t peer) const;

        // No entry == the default == every OTHER capable peer. Only exceptions
        // are stored, so the common case costs zero bytes per peer and the
        // gameplay question of cell-gated routing stays out of the transport
        // entirely. Duplicate targets are collapsed; the origin is never a
        // target however it is listed. An empty target list is a valid
        // exception and means nobody hears this peer.
        //
        // Returns false when the list is longer than mMaxRouteTargets or the
        // route table is full.
        bool setRoute(std::uint32_t from, const std::vector<std::uint32_t>& to);
        void clearRoute(std::uint32_t from);

        // The hot path, called from inside the transport's pump on the pump
        // thread. Never allocates, never blocks, never logs. Either forwards
        // through the sink or increments exactly one counter.
        void onPacket(std::uint32_t fromPeer, const unsigned char* data, std::size_t size, TimePoint now);

        // Matches Net::Session::ChannelHandler's payload type without this
        // header having to know that Net::Session exists: the dependency points
        // one way, transport to relay, and never back.
        void onPacket(std::uint32_t fromPeer, std::string_view data, TimePoint now)
        {
            onPacket(fromPeer, reinterpret_cast<const unsigned char*>(data.data()), data.size(), now);
        }

        // Once per server tick, with the transport's authoritative peer list.
        // Starts tracking peers that have appeared, ages every rate bucket, and
        // prunes the state of peers that are gone.
        //
        // A SWEEP, deliberately, not a disconnect callback: the mod's disconnect
        // branch wraps its cleanups in pcall because a throwing module used to
        // skip the rest of the chain, and a relay that leaks whenever one Lua
        // module throws is a relay that leaks. This cannot be skipped - it is
        // also where the fan-out learns who is listening, so without it a
        // speaker has no audience.
        void tick(const std::vector<std::uint32_t>& livePeers, TimePoint now);

        const RelayCounters& counters() const { return mCounters; }
        std::size_t trackedPeers() const { return mPeerCount; }
        std::size_t capablePeers() const;
        std::size_t routedPeers() const { return mRoutes.size(); }

    private:
        struct PeerState
        {
            std::uint32_t mId = 0;
            double mTokens = 0.0;
            TimePoint mLastRefill{};
            // Closed until the peer says otherwise. It was open while the 'voi'
            // announce did not exist yet and there was nobody to say it; now
            // that the control plane can, silence means one of: no add-on, no
            // voice build, or a build older than the announce -- and forwarding
            // to any of the three is bandwidth spent on somebody who will
            // discard every frame.
            //
            // Note the failure mode inverts with this default. Open, a broken
            // control plane cost privacy: everyone was relayed. Closed, it costs
            // the feature entirely, and it does so in silence, because every
            // rejection here is a counter rather than a log line. The first
            // thing to read when voice is missing is voipStats().capable.
            bool mCapable = false;
            bool mSeen = false; // set by tick's sweep
        };

        const PeerState* findPeer(std::uint32_t peer) const;
        PeerState* findPeer(std::uint32_t peer);
        // Existing state, or a new full bucket, or nullptr when the table is
        // full - in which case the refusal has already been counted.
        PeerState* trackPeer(std::uint32_t peer, TimePoint now);
        void refill(PeerState& state, TimePoint now);
        bool allow(PeerState& state, TimePoint now);
        void fanOut(std::uint32_t fromPeer, const unsigned char* data, std::size_t size);

        RelayLimits mLimits;
        Sink mSink;
        bool mEnabled = false;
        // A flat table scanned linearly: at most sMaxRelayPeers four-byte
        // comparisons, against a node allocation and a hash per lookup for a map
        // that would then need its own bound anyway.
        std::array<PeerState, sMaxRelayPeers> mPeers{};
        std::size_t mPeerCount = 0;
        // Exceptions only, so this is empty in the common case and costs a
        // lookup on a table with no buckets.
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> mRoutes;
        // Reused scratch for the rewritten datagram: fixed size, so the hot path
        // allocates nothing.
        std::array<unsigned char, sMaxPacketBytes> mScratch{};
        RelayCounters mCounters;
    };
}

#endif // OPENMW_COMPONENTS_VOIP_RELAY_H
