#ifndef GAME_SOUND_VOICECODEC_H
#define GAME_SOUND_VOICECODEC_H

#include <cstddef>
#include <cstdint>
#include <memory>

struct OpusEncoder;
struct OpusDecoder;

namespace MWSound
{
    // Opus, configured for multiplayer voice. Everything runs at 48 kHz mono:
    // Opus works at 48 kHz internally whatever you ask for, so anything else
    // only buys a resampler, and the capture and playback ends are both 48 kHz
    // already.
    inline constexpr int sVoiceSampleRate = 48000;
    inline constexpr int sVoiceChannels = 1;

    // 20 ms per frame. Smaller frames spend more on per-packet overhead than on
    // audio (the header is ~48 bytes against a ~60 byte payload), larger ones
    // add straight to mouth-to-ear latency.
    inline constexpr int sVoiceFrameSamples = 960;

    // A 20 ms frame at the bitrates we use is well under 200 bytes; the cap is
    // what the receiver is willing to look at, and it is also enforced by the
    // relay so a peer cannot make us allocate.
    inline constexpr std::size_t sVoiceMaxPacketBytes = 400;

    inline constexpr int sVoiceDefaultBitrate = 24000;

    // In-band FEC only exists in the SILK/hybrid modes, so the bitrate has to
    // stay low enough that the encoder does not switch to CELT-only.
    inline constexpr int sVoiceMaxFecBitrate = 32000;

    class VoiceEncoder
    {
    public:
        // Throws std::runtime_error if Opus refuses to start.
        explicit VoiceEncoder(int bitrate = sVoiceDefaultBitrate);
        ~VoiceEncoder();

        VoiceEncoder(const VoiceEncoder&) = delete;
        VoiceEncoder& operator=(const VoiceEncoder&) = delete;

        // Encodes exactly sVoiceFrameSamples samples of mono 16-bit PCM.
        // Returns the number of bytes written to out, or 0 if Opus produced
        // nothing to send (DTX decided this frame is silence).
        std::size_t encode(const std::int16_t* pcm, unsigned char* out, std::size_t outSize);

        void setBitrate(int bps);

        // Off while the jitter buffer and sequence handling are being debugged:
        // DTX makes the packet stream deliberately gappy, which is impossible
        // to tell apart from the losses we are trying to measure.
        void setDtx(bool enabled);

        // What the encoder should assume the network is losing, in percent.
        // FEC is inert unless this is non-zero as well as enabled.
        void setExpectedLoss(int percent);

        int getBitrate() const { return mBitrate; }

    private:
        OpusEncoder* mEncoder = nullptr;
        int mBitrate;
    };

    class VoiceDecoder
    {
    public:
        // One per speaker, kept alive across talk spurts: resetting mid-stream
        // throws away the decoder state that makes speech sound continuous.
        VoiceDecoder();
        ~VoiceDecoder();

        VoiceDecoder(const VoiceDecoder&) = delete;
        VoiceDecoder& operator=(const VoiceDecoder&) = delete;

        // Decodes one packet into exactly sVoiceFrameSamples samples.
        // Returns false and leaves silence in pcm if the packet is corrupt.
        bool decode(const unsigned char* data, std::size_t size, std::int16_t* pcm);

        // Recovers a frame that never arrived from the in-band FEC data carried
        // by the FOLLOWING packet. Only useful if the jitter buffer holds one
        // frame more than it strictly needs, so that the next packet is already
        // in hand when the gap is noticed.
        bool decodeFec(const unsigned char* nextPacket, std::size_t size, std::int16_t* pcm);

        // Packet loss concealment: what to play when neither the frame nor its
        // successor arrived. Fine for a frame or two, an obvious artefact after
        // that, so the caller fades out rather than concealing indefinitely.
        void conceal(std::int16_t* pcm);

    private:
        OpusDecoder* mDecoder = nullptr;
    };
}

#endif // GAME_SOUND_VOICECODEC_H
