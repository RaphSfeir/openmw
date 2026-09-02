#include "voicecodec.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include <opus.h>

#include <components/debug/debuglog.hpp>

namespace MWSound
{
    namespace
    {
        // Opus refuses anything under 6 kbit/s. The ceiling is not a taste
        // judgement: in-band FEC lives in the SILK layer, and the encoder
        // abandons SILK for CELT-only once the bitrate is high enough that it
        // no longer needs it. Every packet the voice path sends is expected to
        // carry redundancy for its predecessor, so a bitrate that quietly turns
        // that off is worse than a bitrate that sounds slightly worse.
        constexpr int sMinBitrate = 6000;
        constexpr int sMaxBitrate = sVoiceMaxFecBitrate;

        int clampBitrate(int bps)
        {
            const int clamped = std::clamp(bps, sMinBitrate, sMaxBitrate);
            if (clamped != bps)
                Log(Debug::Warning) << "Voice bitrate " << bps << " is outside the range in-band FEC survives; using "
                                    << clamped;
            return clamped;
        }

        // The ctls are macros wrapping a variadic call, so the result has to be
        // handed over after the fact rather than wrapped. None of them is fatal
        // on its own, but a silently rejected one leaves the rest of the voice
        // path relying on a setting that is not actually in force.
        void checkCtl(int result, const char* what)
        {
            if (result != OPUS_OK)
                Log(Debug::Warning) << "Opus rejected " << what << ": " << opus_strerror(result);
        }

        void silence(std::int16_t* pcm, int from)
        {
            std::memset(pcm + from, 0, static_cast<std::size_t>(sVoiceFrameSamples - from) * sizeof(std::int16_t));
        }
    }

    VoiceEncoder::VoiceEncoder(int bitrate)
        : mBitrate(clampBitrate(bitrate))
    {
        int err = OPUS_OK;
        mEncoder = opus_encoder_create(sVoiceSampleRate, sVoiceChannels, OPUS_APPLICATION_VOIP, &err);
        if (mEncoder == nullptr || err != OPUS_OK)
        {
            // Opus can hand back both a state and an error. The constructor is
            // about to throw, so the destructor will never run, and the state
            // has to go back here or it is lost.
            if (mEncoder != nullptr)
                opus_encoder_destroy(mEncoder);
            mEncoder = nullptr;
            throw std::runtime_error(std::string("Failed to create Opus encoder: ") + opus_strerror(err));
        }

        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_BITRATE(mBitrate)), "bitrate");
        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_VBR(1)), "VBR");
        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_COMPLEXITY(10)), "complexity");
        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)), "voice signal hint");

        // The two FEC controls only mean anything together: the encoder spends
        // bits on redundancy for the previous frame in proportion to the loss
        // it is told to expect, so enabling FEC while the expected loss is zero
        // produces a stream with no redundancy in it at all.
        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_INBAND_FEC(1)), "in-band FEC");
        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_PACKET_LOSS_PERC(10)), "expected packet loss");

        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_LSB_DEPTH(16)), "LSB depth");
        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_DTX(0)), "DTX");

        Log(Debug::Info) << "Opus voice encoder: " << mBitrate << " bps, " << sVoiceFrameSamples << " samples/frame";
    }

    VoiceEncoder::~VoiceEncoder()
    {
        if (mEncoder != nullptr)
            opus_encoder_destroy(mEncoder);
    }

    std::size_t VoiceEncoder::encode(const std::int16_t* pcm, unsigned char* out, std::size_t outSize)
    {
        const opus_int32 bytes
            = opus_encode(mEncoder, pcm, sVoiceFrameSamples, out, static_cast<opus_int32>(outSize));
        if (bytes < 0)
        {
            Log(Debug::Warning) << "Opus encode failed: " << opus_strerror(bytes);
            return 0;
        }

        // A one byte packet is Opus saying the frame is silence and carries
        // nothing worth the header it would travel under.
        if (bytes <= 1)
            return 0;

        return static_cast<std::size_t>(bytes);
    }

    void VoiceEncoder::setBitrate(int bps)
    {
        mBitrate = clampBitrate(bps);
        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_BITRATE(mBitrate)), "bitrate");
    }

    void VoiceEncoder::setDtx(bool enabled)
    {
        checkCtl(opus_encoder_ctl(mEncoder, OPUS_SET_DTX(enabled ? 1 : 0)), "DTX");
    }

    void VoiceEncoder::setExpectedLoss(int percent)
    {
        checkCtl(
            opus_encoder_ctl(mEncoder, OPUS_SET_PACKET_LOSS_PERC(std::clamp(percent, 0, 100))), "expected packet loss");
    }

    VoiceDecoder::VoiceDecoder()
    {
        int err = OPUS_OK;
        mDecoder = opus_decoder_create(sVoiceSampleRate, sVoiceChannels, &err);
        if (mDecoder == nullptr || err != OPUS_OK)
        {
            if (mDecoder != nullptr)
                opus_decoder_destroy(mDecoder);
            mDecoder = nullptr;
            throw std::runtime_error(std::string("Failed to create Opus decoder: ") + opus_strerror(err));
        }
    }

    VoiceDecoder::~VoiceDecoder()
    {
        if (mDecoder != nullptr)
            opus_decoder_destroy(mDecoder);
    }

    bool VoiceDecoder::decode(const unsigned char* data, std::size_t size, std::int16_t* pcm)
    {
        const int samples = opus_decode(mDecoder, data, static_cast<opus_int32>(size), pcm, sVoiceFrameSamples, 0);
        if (samples < 0)
        {
            Log(Debug::Warning) << "Opus decode failed: " << opus_strerror(samples);
            silence(pcm, 0);
            return false;
        }

        // The caller consumes a whole frame whatever happens, so a packet that
        // decoded to fewer samples still has to hand back a full one.
        silence(pcm, samples);
        return true;
    }

    bool VoiceDecoder::decodeFec(const unsigned char* nextPacket, std::size_t size, std::int16_t* pcm)
    {
        const int samples
            = opus_decode(mDecoder, nextPacket, static_cast<opus_int32>(size), pcm, sVoiceFrameSamples, 1);
        if (samples < 0)
        {
            silence(pcm, 0);
            return false;
        }

        silence(pcm, samples);
        return true;
    }

    void VoiceDecoder::conceal(std::int16_t* pcm)
    {
        const int samples = opus_decode(mDecoder, nullptr, 0, pcm, sVoiceFrameSamples, 0);
        if (samples < 0)
        {
            silence(pcm, 0);
            return;
        }

        silence(pcm, samples);
    }
}
