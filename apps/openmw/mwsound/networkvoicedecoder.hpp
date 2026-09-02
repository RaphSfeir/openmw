#ifndef GAME_SOUND_NETWORKVOICEDECODER_H
#define GAME_SOUND_NETWORKVOICEDECODER_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <components/voip/jitterbuffer.hpp>

#include "sounddecoder.hpp"
#include "voicecodec.hpp"

namespace MWSound
{
    // One remote speaker, presented to the sound system as something it can
    // stream from. The stream thread pulls PCM out of read(); packets arrive
    // from somewhere else entirely and are pushed in from the other side.
    //
    // The hard rule this class exists to keep is in read(): it must hand back
    // every byte it was asked for, always. A short read is not treated as a
    // hiccup by the layer above - it marks the stream finished for good, which
    // drops the stream and destroys the speaker's attachment rather than
    // muting it. Silence is always the right answer to having nothing to say;
    // a short buffer never is. For the same reason nothing in read() is allowed
    // to throw, because an exception ends the stream just as permanently.
    class NetworkVoiceDecoder final : public SoundDecoder
    {
    public:
        NetworkVoiceDecoder();
        ~NetworkVoiceDecoder() override;

        // Called from whichever thread takes packets off the wire, or in the
        // loopback drill straight from the capture thread.
        void pushPacket(std::uint16_t seq, const unsigned char* data, std::size_t size, bool endOfSpurt);

        // Speaker gone. Deliberately does not reset the Opus decoder: its state
        // is what makes speech continuous across a pause, and one decoder lives
        // for the whole attachment.
        void resetBuffer();

        Voip::JitterBufferStats stats() const;
        int bufferedDelayMs() const;

        // SoundDecoder. open() is never called: this decoder is handed to the
        // sound system already carrying its source, the way the movie audio
        // bridge is.
        void open(VFS::Path::NormalizedView fname) override;
        void close() override;
        std::string getName() override;
        void getInfo(int* samplerate, ChannelConfig* chans, SampleType* type) override;
        std::size_t read(char* buffer, std::size_t bytes) override;
        void readAll(std::vector<char>& output) override;
        std::size_t getSampleOffset() override;

    private:
        // Decodes exactly one frame into mFrame, whatever the buffer says.
        void decodeOneFrame();

        mutable std::mutex mMutex;
        Voip::JitterBufferSettings mSettings;
        Voip::JitterBuffer mBuffer;
        VoiceDecoder mDecoder;

        // Concealment is convincing for a frame or two and turns metallic after
        // that, so a long loss run is faded out rather than played to the end.
        // The gain is carried across frames and interpolated within them: a
        // gain that stepped at frame boundaries would put a click every 20 ms
        // into exactly the situation that already sounds bad.
        float mFadeGain = 1.0f;

        std::vector<std::int16_t> mFrame;
        // How much of mFrame the last read() left behind. A read is measured in
        // bytes the sound system chose, not in codec frames, so the two do not
        // divide evenly and the remainder has to survive to the next call.
        std::size_t mFrameOffset = 0;

        std::size_t mSamplesRead = 0;
    };
}

#endif // GAME_SOUND_NETWORKVOICEDECODER_H
