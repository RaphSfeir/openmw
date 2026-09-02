#include "relay.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace Voip
{
    Relay::Relay(const RelayLimits& limits)
        : mLimits(limits)
    {
        mLimits.mMaxPacketBytes = std::min(mLimits.mMaxPacketBytes, sMaxPacketBytes);
        mLimits.mMaxPeers = std::min(mLimits.mMaxPeers, sMaxRelayPeers);
        mLimits.mMaxRouteTargets = std::min(mLimits.mMaxRouteTargets, sMaxRelayPeers);
    }

    void Relay::setSink(Sink sink)
    {
        mSink = std::move(sink);
    }

    void Relay::setEnabled(bool enabled)
    {
        mEnabled = enabled;
    }

    const Relay::PeerState* Relay::findPeer(std::uint32_t peer) const
    {
        for (std::size_t i = 0; i < mPeerCount; ++i)
        {
            if (mPeers[i].mId == peer)
                return &mPeers[i];
        }
        return nullptr;
    }

    Relay::PeerState* Relay::findPeer(std::uint32_t peer)
    {
        return const_cast<PeerState*>(std::as_const(*this).findPeer(peer));
    }

    Relay::PeerState* Relay::trackPeer(std::uint32_t peer, TimePoint now)
    {
        if (PeerState* existing = findPeer(peer))
            return existing;

        if (mPeerCount >= mLimits.mMaxPeers)
        {
            ++mCounters.mRefusedPeers;
            return nullptr;
        }

        PeerState& state = mPeers[mPeerCount++];
        state = PeerState{};
        state.mId = peer;
        // A new peer starts with a full bucket: the burst allowance exists for
        // exactly this case, a client whose first frames arrive in one clump.
        state.mTokens = mLimits.mBurstPackets;
        state.mLastRefill = now;
        return &state;
    }

    void Relay::refill(PeerState& state, TimePoint now)
    {
        const double elapsed = std::chrono::duration<double>(now - state.mLastRefill).count();
        // Time never runs backwards on a steady clock, but a caller that mixes
        // stamps from two ticks must not be able to hand out free tokens either.
        if (elapsed <= 0.0)
            return;

        state.mLastRefill = now;
        state.mTokens = std::min(mLimits.mBurstPackets, state.mTokens + elapsed * mLimits.mPacketsPerSecond);
    }

    bool Relay::allow(PeerState& state, TimePoint now)
    {
        refill(state, now);
        if (state.mTokens < 1.0)
            return false;

        state.mTokens -= 1.0;
        return true;
    }

    bool Relay::setCapable(std::uint32_t peer, bool capable)
    {
        // No clock here by design. A peer created by this path anchors its
        // bucket at the epoch, which the first refill clamps straight back to
        // the burst ceiling it already holds - the same state a peer the sweep
        // discovered is in.
        PeerState* state = trackPeer(peer, TimePoint{});
        if (state == nullptr)
            return false;

        state->mCapable = capable;
        return true;
    }

    bool Relay::capable(std::uint32_t peer) const
    {
        // An untracked peer is not capable. This half matters as much as the
        // field's default: the question is most often asked about a peer that
        // has just left, and answering "yes" for someone the table no longer
        // holds is how a route policy ends up addressing nobody.
        const PeerState* state = findPeer(peer);
        return state != nullptr && state->mCapable;
    }

    bool Relay::setRoute(std::uint32_t from, const std::vector<std::uint32_t>& to)
    {
        if (to.size() > mLimits.mMaxRouteTargets)
        {
            ++mCounters.mRefusedPeers;
            return false;
        }

        if (mRoutes.find(from) == mRoutes.end() && mRoutes.size() >= mLimits.mMaxPeers)
        {
            ++mCounters.mRefusedPeers;
            return false;
        }

        std::vector<std::uint32_t>& targets = mRoutes[from];
        targets.clear();
        for (const std::uint32_t peer : to)
        {
            // A repeated target would send one frame twice, which sounds like a
            // codec fault and is not one.
            if (std::find(targets.begin(), targets.end(), peer) == targets.end())
                targets.push_back(peer);
        }
        return true;
    }

    void Relay::clearRoute(std::uint32_t from)
    {
        mRoutes.erase(from);
    }

    void Relay::onPacket(std::uint32_t fromPeer, const unsigned char* data, std::size_t size, TimePoint now)
    {
        ++mCounters.mReceived;
        mCounters.mBytesIn += size;

        if (!mEnabled)
        {
            ++mCounters.mDroppedDisabled;
            return;
        }

        PeerState* state = trackPeer(fromPeer, now);
        if (state == nullptr)
            return; // trackPeer counted the refusal

        if (!state->mCapable)
        {
            ++mCounters.mDroppedNotCapable;
            return;
        }

        // Everything from here to writeSpeaker touches bytes 0..7 and nothing
        // else. The payload is copied sight unseen.
        Header header;
        if (!readHeader(data, size, header))
        {
            ++mCounters.mDroppedMalformed;
            return;
        }

        if (size > mLimits.mMaxPacketBytes)
        {
            ++mCounters.mDroppedOversize;
            return;
        }

        // Unknown flag bits ride along unmasked; only an unknown version is a
        // drop, so an older relay cannot silently mute a newer client.
        if (header.mVersion != sWireVersion)
        {
            ++mCounters.mDroppedVersion;
            return;
        }

        if (!allow(*state, now))
        {
            ++mCounters.mDroppedRate;
            return;
        }

        if (header.mSpeaker != 0)
            ++mCounters.mSpoofedOrigin;

        std::memcpy(mScratch.data(), data, size);
        writeSpeaker(mScratch.data(), fromPeer);
        fanOut(fromPeer, mScratch.data(), size);
    }

    void Relay::fanOut(std::uint32_t fromPeer, const unsigned char* data, std::size_t size)
    {
        if (!mSink)
        {
            ++mCounters.mDroppedNoRoute;
            return;
        }

        std::size_t sent = 0;
        const auto route = mRoutes.find(fromPeer);
        if (route != mRoutes.end())
        {
            for (const std::uint32_t to : route->second)
            {
                // The origin is excluded here and not only at setRoute, because
                // this is the routing decision: nothing a policy can say may
                // make a speaker hear themselves.
                if (to == fromPeer)
                    continue;

                const PeerState* target = findPeer(to);
                if (target == nullptr || !target->mCapable)
                    continue;

                mSink(to, data, size);
                ++sent;
            }
        }
        else
        {
            for (std::size_t i = 0; i < mPeerCount; ++i)
            {
                const PeerState& target = mPeers[i];
                if (target.mId == fromPeer || !target.mCapable)
                    continue;

                mSink(target.mId, data, size);
                ++sent;
            }
        }

        if (sent == 0)
        {
            ++mCounters.mDroppedNoRoute;
            return;
        }

        mCounters.mForwarded += sent;
        mCounters.mBytesOut += sent * size;
    }

    void Relay::tick(const std::vector<std::uint32_t>& livePeers, TimePoint now)
    {
        for (std::size_t i = 0; i < mPeerCount; ++i)
            mPeers[i].mSeen = false;

        for (const std::uint32_t peer : livePeers)
        {
            PeerState* state = trackPeer(peer, now);
            if (state == nullptr)
                continue;

            state->mSeen = true;
            refill(*state, now);
        }

        std::size_t i = 0;
        while (i < mPeerCount)
        {
            if (mPeers[i].mSeen)
            {
                ++i;
                continue;
            }

            // Order carries no meaning, so the hole is filled from the end
            // rather than by shifting the tail down.
            mPeers[i] = mPeers[mPeerCount - 1];
            --mPeerCount;
            ++mCounters.mPrunedPeers;
        }

        // A route survives only as long as the peer it speaks for. Peers the
        // sweep just kept are all tracked, so anything keyed on an untracked id
        // belongs to somebody who has left.
        for (auto it = mRoutes.begin(); it != mRoutes.end();)
        {
            if (findPeer(it->first) == nullptr)
                it = mRoutes.erase(it);
            else
                ++it;
        }
    }

    std::size_t Relay::capablePeers() const
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i < mPeerCount; ++i)
        {
            if (mPeers[i].mCapable)
                ++count;
        }
        return count;
    }
}
