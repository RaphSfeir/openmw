#ifndef OPENMW_COMPONENTS_VOIP_WIRE_H
#define OPENMW_COMPONENTS_VOIP_WIRE_H

#include <cstddef>
#include <cstdint>

#include "jitterbuffer.hpp" // sMaxPayloadBytes only

namespace Voip
{
    // The third ENet channel. Net::Session::sChannelCount must be at least 3 or
    // every voice send is clamped back down to a gameplay channel and arrives as
    // unparseable gameplay traffic.
    inline constexpr std::uint8_t sVoiceChannel = 2;

    inline constexpr std::uint8_t sWireVersion = 1;

    // [u8 ver][u8 flags][u16 seq LE][u32 speaker LE]
    inline constexpr std::size_t sHeaderBytes = 8;

    // Whole datagram, header included. This is a CORRECTNESS bound and not a
    // bandwidth one: ENet tests the payload length before it looks at the flags,
    // and its fragment path knows only the reliable and unreliable-fragment
    // cases, so an unsequenced packet past roughly 1370 bytes silently becomes a
    // reliable, ordered, retransmitted fragment set - the exact delivery class
    // voice chose Unsequenced to avoid.
    inline constexpr std::size_t sMaxPacketBytes = 300;

    // A frame the relay accepts must be one the receiver's ring can hold, or the
    // drop is silent and happens on the wrong machine. The two constants measure
    // different things (whole datagram against payload), which is why raising
    // one would otherwise quietly break the other.
    static_assert(sMaxPacketBytes - sHeaderBytes <= sMaxPayloadBytes);
    static_assert(sMaxPacketBytes > sHeaderBytes);

    enum Flag : std::uint8_t
    {
        FlagEndOfSpurt = 1 << 0,
        FlagShout = 1 << 1,
        FlagNonPositional = 1 << 2,
    };

    struct Header
    {
        std::uint8_t mVersion = sWireVersion;
        std::uint8_t mFlags = 0;
        std::uint16_t mSeq = 0;
        // Sender writes 0; the relay stamps the authoritative origin peer id.
        std::uint32_t mSpeaker = 0;
    };

    // Explicit little endian both ways. ENet does not byte-swap payloads, and
    // nothing else in this fork has ever had to care because every other payload
    // goes through LuaUtil's serializer. Never memcpy the struct: its layout is
    // the compiler's business and the wire's layout is not.
    //
    // Unknown FLAG bits are the caller's business: readHeader passes them
    // through unmasked and the relay forwards them, per the convention the mod's
    // wire already follows, that a newer peer talking about things we do not
    // know is harmless. Only an unknown VERSION is a drop.
    //
    // readHeader is the only one of the three that checks a length; the writers
    // require a buffer of at least sHeaderBytes bytes, which on the relay's path
    // is guaranteed by readHeader having already succeeded on it.
    bool readHeader(const unsigned char* data, std::size_t size, Header& out);
    void writeHeader(unsigned char* data, const Header& header);
    void writeSpeaker(unsigned char* data, std::uint32_t speaker);
}

#endif // OPENMW_COMPONENTS_VOIP_WIRE_H
