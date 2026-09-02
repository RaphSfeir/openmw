#ifndef GAME_SOUND_VOICECAPTURE_H
#define GAME_SOUND_VOICECAPTURE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ALCdevice;

namespace MWSound
{
    // Ok means frames are arriving. Silent means the device opened and is
    // handing back nothing but zeroes, which on Windows almost always means
    // microphone privacy is denying this process rather than a broken mic, and
    // is worth saying out loud because nothing else about the device looks
    // wrong. Failed is a refused open, Disconnected an endpoint that went away
    // while it was open.
    enum class VoiceCaptureStatus
    {
        Closed,
        Ok,
        Silent,
        Failed,
        Disconnected,
    };

    // Microphone capture on a thread of its own. The ALC capture API is
    // independent of the playback context, so nothing here touches the
    // OpenALOutput device, and the thread keeps draining through loading
    // screens and minimisation - both of which stall SoundManager::update,
    // and neither of which should cut a talking player off mid-sentence.
    class VoiceCapture
    {
    public:
        using FrameCallback = std::function<void(const std::int16_t* pcm, std::size_t samples)>;

        static std::vector<std::string> enumerateDevices();
        static std::string defaultDeviceName();

        VoiceCapture();
        ~VoiceCapture();

        VoiceCapture(const VoiceCapture&) = delete;
        VoiceCapture& operator=(const VoiceCapture&) = delete;

        // deviceName empty = system default. Never throws: a machine with no
        // usable microphone is an ordinary state for a game to be in, not an
        // error worth unwinding for.
        bool open(const std::string& deviceName = {});
        void close();
        bool isOpen() const;

        // Starts/stops capture and the poll thread together, so the microphone
        // is only live while something is listening to it - on Windows this is
        // what the in-use indicator follows, and leaving it lit for a whole
        // session because voice happens to be compiled in is not acceptable.
        //
        // The callback runs on the poll thread, once per whole 20 ms frame, and
        // must not block: the same thread is what keeps the driver's ring from
        // overrunning. It may call back into this object; stop() from inside it
        // asks the thread to finish rather than joining it against itself.
        void start(FrameCallback callback);
        void stop();

        // Gate: while false the thread keeps draining the device (so the ring
        // never overruns and stale audio never surfaces on the next press) but
        // does not invoke the callback.
        void setTransmitting(bool transmitting);
        bool isTransmitting() const;

        VoiceCaptureStatus status() const;

        // RMS of the most recent frame, 0..1. Updated whether or not the gate
        // is open, so a settings panel can show a level meter without putting
        // the player on the air.
        float level() const;

        std::string deviceName() const;

    private:
        void run();

        ALCdevice* mDevice = nullptr;
        bool mHasDisconnectExt = false;

        mutable std::mutex mNameMutex;
        std::string mDeviceName;

        std::atomic<VoiceCaptureStatus> mStatus{ VoiceCaptureStatus::Closed };
        std::atomic<float> mLevel{ 0.0f };
        std::atomic<bool> mTransmitting{ false };

        FrameCallback mCallback;

        std::atomic<bool> mQuitNow{ false };
        std::mutex mMutex;
        std::condition_variable mCondVar;
        std::thread mThread;

        // Recorded separately from mThread so that stop() can recognise a call
        // arriving from the poll thread itself without reading mThread, which
        // another thread may be joining at that moment.
        std::atomic<std::thread::id> mThreadId;

        // Serialises the start/stop/close transitions themselves. Two threads
        // reaching stop() together would otherwise both see a joinable thread
        // and both join it.
        std::mutex mLifecycleMutex;
    };

    // Enumerates capture devices, records a few seconds from one, writes a
    // 48 kHz mono WAV next to the log and reports the level it saw. The point
    // is to find out whether this machine can capture at all before any of the
    // rest of the voice path exists.
    bool runVoiceMicTest(const std::string& deviceName, const std::filesystem::path& outFile, int seconds);
}

#endif // GAME_SOUND_VOICECAPTURE_H
