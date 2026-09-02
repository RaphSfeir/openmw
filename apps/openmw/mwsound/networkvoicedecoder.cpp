#include "networkvoicedecoder.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <components/debug/debuglog.hpp>

namespace MWSound
{
    namespace
    {
        // The jitter buffer deliberately knows nothing about Opus, and the codec
        // deliberately knows nothing about the wire, so the frame size and the
        // packet cap are spelled out on both sides. This is the only place that
        // sees both headers, which makes it the only place that can notice them
        // drifting apart.
        static_assert(Voip::sFrameMs * sVoiceSampleRate / 1000 == sVoiceFrameSamples,
            "jitter buffer frame length disagrees with the codec frame size");
        static_assert(Voip::sMaxPayloadBytes == sVoiceMaxPacketBytes,
            "jitter buffer payload cap disagrees with the codec packet cap");
    }

    NetworkVoiceDecoder::NetworkVoiceDecoder()
        : SoundDecoder(nullptr)
        , mBuffer(mSettings)
        , mFrame(sVoiceFrameSamples, 0)
        , mFrameOffset(static_cast<std::size_t>(sVoiceFrameSamples) * sizeof(std::int16_t))
    {
    }

    NetworkVoiceDecoder::~NetworkVoiceDecoder() = default;

    void NetworkVoiceDecoder::pushPacket(
        std::uint16_t seq, const unsigned char* data, std::size_t size, bool endOfSpurt)
    {
        Voip::VoicePacket packet;
        packet.mSeq = seq;
        packet.mEndOfSpurt = endOfSpurt;
        packet.mData = data;
        packet.mSize = size;

        const std::lock_guard<std::mutex> lock(mMutex);
        mBuffer.push(packet, std::chrono::steady_clock::now());
    }

    void NetworkVoiceDecoder::resetBuffer()
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mBuffer.reset();
        mFadeGain = 1.0f;
    }

    Voip::JitterBufferStats NetworkVoiceDecoder::stats() const
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return mBuffer.stats();
    }

    int NetworkVoiceDecoder::bufferedDelayMs() const
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return mBuffer.delayMs();
    }

    void NetworkVoiceDecoder::open(VFS::Path::NormalizedView)
    {
        throw std::runtime_error("A network voice stream has no file to open");
    }

    void NetworkVoiceDecoder::close() {}

    std::string NetworkVoiceDecoder::getName()
    {
        return "network voice";
    }

    void NetworkVoiceDecoder::getInfo(int* samplerate, ChannelConfig* chans, SampleType* type)
    {
        *samplerate = sVoiceSampleRate;
        *chans = ChannelConfig_Mono;
        *type = SampleType_Int16;
    }

    void NetworkVoiceDecoder::readAll(std::vector<char>&)
    {
        throw std::runtime_error("A network voice stream never ends and cannot be read whole");
    }

    void NetworkVoiceDecoder::decodeOneFrame()
    {
        const Voip::PopResult result = mBuffer.pop();

        float targetGain = 1.0f;
        switch (result.mOutcome)
        {
            case Voip::PopOutcome::Frame:
                mDecoder.decode(result.mData, result.mSize, mFrame.data());
                break;
            case Voip::PopOutcome::Fec:
                // mData is the SUCCESSOR's packet and mSeq the frame that never
                // arrived: the redundancy for a frame rides in the one after it.
                mDecoder.decodeFec(result.mData, result.mSize, mFrame.data());
                break;
            case Voip::PopOutcome::Conceal:
            {
                mDecoder.conceal(mFrame.data());
                const int run = mBuffer.concealRun();
                const int limit = std::max(1, mSettings.mMaxConcealFrames);
                targetGain = std::clamp(static_cast<float>(limit - run) / static_cast<float>(limit), 0.0f, 1.0f);
                break;
            }
            case Voip::PopOutcome::Silence:
                // Deliberate quiet, not loss. Concealing here is what makes
                // silence sound metallic, and it would charge dead air to the
                // loss statistic.
                std::fill(mFrame.begin(), mFrame.end(), std::int16_t{ 0 });
                mFadeGain = 1.0f;
                return;
        }

        if (mFadeGain != 1.0f || targetGain != 1.0f)
        {
            const float from = mFadeGain;
            const float span = static_cast<float>(mFrame.size());
            for (std::size_t i = 0; i < mFrame.size(); ++i)
            {
                const float t = static_cast<float>(i) / span;
                mFrame[i] = static_cast<std::int16_t>(static_cast<float>(mFrame[i]) * (from + (targetGain - from) * t));
            }
        }
        mFadeGain = targetGain;
    }

    std::size_t NetworkVoiceDecoder::read(char* buffer, std::size_t bytes)
    {
        const std::size_t frameBytes = mFrame.size() * sizeof(std::int16_t);
        std::size_t written = 0;

        const std::lock_guard<std::mutex> lock(mMutex);
        try
        {
            while (written < bytes)
            {
                if (mFrameOffset >= frameBytes)
                {
                    decodeOneFrame();
                    mFrameOffset = 0;
                }

                const std::size_t chunk = std::min(bytes - written, frameBytes - mFrameOffset);
                std::memcpy(buffer + written, reinterpret_cast<const char*>(mFrame.data()) + mFrameOffset, chunk);
                mFrameOffset += chunk;
                written += chunk;
            }
        }
        catch (const std::exception& e)
        {
            // Letting this escape would end the stream permanently, which is a
            // far worse outcome than a moment of silence.
            Log(Debug::Warning) << "Voice decode failed, filling silence: " << e.what();
        }

        if (written < bytes)
            std::memset(buffer + written, 0, bytes - written);

        mSamplesRead += bytes / sizeof(std::int16_t);
        return bytes;
    }

    std::size_t NetworkVoiceDecoder::getSampleOffset()
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        return mSamplesRead;
    }
}
