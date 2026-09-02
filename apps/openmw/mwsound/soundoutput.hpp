#ifndef GAME_SOUND_SOUNDOUTPUT_H
#define GAME_SOUND_SOUNDOUTPUT_H

#include <memory>
#include <string>
#include <vector>

#include <components/settings/hrtfmode.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/soundmanager.hpp"

namespace MWSound
{
    class SoundManager;
    struct SoundDecoder;
    class Sound;
    class Stream;

    // An opaque handle for the implementation's sound buffers.
    typedef void* Sound_Handle;
    // An opaque handle for the implementation's sound instances.
    typedef void* Sound_Instance;

    enum Environment
    {
        Env_Normal,
        Env_Underwater
    };

    // How much audio a stream keeps queued ahead of the listener. The defaults
    // are what every stream used before voice existed, and music and movie
    // audio still want them: a deep queue that survives a stalled decoder.
    //
    // Voice wants the opposite. The queue is dead latency between a word being
    // spoken and being heard, and the default geometry buries speech under
    // three quarters of a second of it.
    //
    // mBufferSamples exists because the seconds form cannot express a whole
    // number of codec frames: the size is computed by truncating a float, and
    // 0.02f * 48000 comes out at 959, which is not a 20 ms Opus frame. A
    // non-zero sample count is used verbatim instead.
    struct StreamGeometry
    {
        unsigned int mBufferCount = 6;
        float mBufferSeconds = 0.125f;
        unsigned int mBufferSamples = 0;
    };

    using HrtfMode = Settings::HrtfMode;

    class SoundOutput
    {
        SoundManager& mManager;

        virtual std::vector<std::string> enumerate() = 0;
        virtual bool init(const std::string& devname, const std::string& hrtfname, HrtfMode hrtfmode) = 0;
        virtual void deinit() = 0;

        virtual std::vector<std::string> enumerateHrtf() = 0;

        virtual std::pair<Sound_Handle, size_t> loadSound(VFS::Path::NormalizedView fname) = 0;
        virtual size_t unloadSound(Sound_Handle data) = 0;

        virtual bool playSound(Sound* sound, Sound_Handle data, float offset) = 0;
        virtual bool playSound3D(Sound* sound, Sound_Handle data, float offset) = 0;
        virtual void finishSound(Sound* sound) = 0;
        virtual bool isSoundPlaying(Sound* sound) = 0;
        virtual void updateSound(Sound* sound) = 0;

        virtual bool streamSound(
            DecoderPtr decoder, Stream* sound, bool getLoudnessData = false, const StreamGeometry& geom = {})
            = 0;
        virtual bool streamSound3D(
            DecoderPtr decoder, Stream* sound, bool getLoudnessData, const StreamGeometry& geom = {})
            = 0;
        virtual void finishStream(Stream* sound) = 0;

        // Cuts short the stream thread's idle wait so a stream that has just
        // been handed new data does not sit through the rest of it. Voice runs
        // on buffers far shorter than that wait.
        virtual void wakeStreamThread() = 0;
        virtual double getStreamDelay(Stream* sound) = 0;
        virtual float getStreamOffset(Stream* sound) = 0;
        virtual float getStreamLoudness(Stream* sound) = 0;
        virtual bool isStreamPlaying(Stream* sound) = 0;
        virtual void updateStream(Stream* sound) = 0;

        virtual void startUpdate() = 0;
        virtual void finishUpdate() = 0;

        virtual void updateListener(const osg::Vec3f& pos, const osg::Vec3f& atdir, const osg::Vec3f& updir,
            const osg::Vec3f& vel, Environment env)
            = 0;

        virtual void pauseSounds(int types) = 0;
        virtual void resumeSounds(int types) = 0;

        virtual void pauseActiveDevice() = 0;
        virtual void resumeActiveDevice() = 0;

        SoundOutput& operator=(const SoundOutput& rhs);
        SoundOutput(const SoundOutput& rhs);

    protected:
        bool mInitialized;

        SoundOutput(SoundManager& mgr)
            : mManager(mgr)
            , mInitialized(false)
        {
        }

    public:
        virtual ~SoundOutput() {}

        bool isInitialized() const { return mInitialized; }

        friend class OpenALOutput;
        friend class SoundManager;
        friend class SoundBufferPool;
    };
}

#endif
