#include "voicecapture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <thread>

#include <components/debug/debuglog.hpp>

#include "al.h"
#include "alc.h"
#include "alext.h"
#include "voicecodec.hpp"

namespace MWSound
{
    namespace
    {
        // The ring handed to alcCaptureOpenDevice, and the count reported by
        // ALC_CAPTURE_SAMPLES, are both in sample frames - not bytes, and not
        // the byte count you get from multiplying by the sample size. 9600
        // frames of 48 kHz mono is 200 ms, which is ten times the poll period
        // and enough slack that a scheduler hiccup, a shader compile or a cell
        // load does not cost audio. An overrun is silent when it happens: the
        // driver simply keeps the newest samples and the loss only shows up as
        // a click.
        constexpr int sCaptureRingFrames = 9600;

        constexpr int sPollIntervalMs = 5;

        // A working microphone never trips this however quiet the room, because
        // real capture hardware always puts a little dither noise in the low
        // bits - the headset on this machine measures peak 1 against a device
        // denied by Windows privacy, which measures a flat zero. Two seconds is
        // short enough that a player holding the key finds out promptly, and
        // short enough that the three second --voip-mic-test drill can actually
        // observe the state rather than stopping just before it becomes true.
        constexpr auto sSilenceTimeout = std::chrono::seconds(2);

        std::string_view describe(VoiceCaptureStatus status)
        {
            switch (status)
            {
                case VoiceCaptureStatus::Closed:
                    return "closed";
                case VoiceCaptureStatus::Ok:
                    return "ok";
                case VoiceCaptureStatus::Silent:
                    return "silent";
                case VoiceCaptureStatus::Failed:
                    return "failed";
                case VoiceCaptureStatus::Disconnected:
                    return "disconnected";
            }
            return "unknown";
        }

        void writeLe(std::ofstream& out, std::uint32_t value, int bytes)
        {
            for (int i = 0; i < bytes; ++i)
                out.put(static_cast<char>((value >> (8 * i)) & 0xffu));
        }

        // 16-bit mono PCM at sVoiceSampleRate, canonical 44-byte header. The
        // sample block is copied out verbatim, which assumes a little-endian
        // host; every platform the client builds for is one.
        bool writeMonoWav(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm)
        {
            std::ofstream out(path, std::ios::binary);
            if (!out)
                return false;

            const std::uint32_t dataBytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));

            out.write("RIFF", 4);
            // Everything in the file after this field: the 4-byte WAVE tag, the
            // 24-byte fmt chunk with its header, the 8-byte data header, then
            // the samples.
            writeLe(out, 36u + dataBytes, 4);
            out.write("WAVE", 4);

            out.write("fmt ", 4);
            writeLe(out, 16, 4);
            writeLe(out, 1, 2); // WAVE_FORMAT_PCM
            writeLe(out, sVoiceChannels, 2);
            writeLe(out, sVoiceSampleRate, 4);
            writeLe(out, static_cast<std::uint32_t>(sVoiceSampleRate * sVoiceChannels * 2), 4); // byte rate
            writeLe(out, static_cast<std::uint32_t>(sVoiceChannels * 2), 2); // block align
            writeLe(out, 16, 2); // bits per sample

            out.write("data", 4);
            writeLe(out, dataBytes, 4);
            if (dataBytes != 0)
                out.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);

            out.flush();
            return out.good();
        }
    }

    std::vector<std::string> VoiceCapture::enumerateDevices()
    {
        std::vector<std::string> devices;
        // One NUL-terminated name after another, the list itself ended by an
        // empty name, so the walk stops on the second NUL rather than a count.
        const ALCchar* names = alcGetString(nullptr, ALC_CAPTURE_DEVICE_SPECIFIER);
        while (names && *names)
        {
            devices.emplace_back(names);
            names += std::strlen(names) + 1;
        }
        return devices;
    }

    std::string VoiceCapture::defaultDeviceName()
    {
        const ALCchar* name = alcGetString(nullptr, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
        if (name == nullptr)
            return {};
        return name;
    }

    VoiceCapture::VoiceCapture() = default;

    VoiceCapture::~VoiceCapture()
    {
        // The poll thread dereferences mDevice on every iteration, so it has to
        // be joined before the device it is reading from goes away.
        stop();
        close();
    }

    bool VoiceCapture::open(const std::string& deviceName)
    {
        close();

        const ALCchar* name = deviceName.empty() ? nullptr : deviceName.c_str();
        mDevice = alcCaptureOpenDevice(name, sVoiceSampleRate, AL_FORMAT_MONO16, sCaptureRingFrames);
        if (mDevice == nullptr)
        {
            // A refused open leaves the error on the null device, there being
            // no device to ask. The common cause on Windows is a microphone
            // array endpoint whose WASAPI backend refuses a mono request
            // outright (openal-soft issue 473); an application holding the
            // endpoint in exclusive mode looks the same from here.
            const ALCenum err = alcGetError(nullptr);
            Log(Debug::Warning) << "Failed to open voice capture device \""
                                << (deviceName.empty() ? "(default)" : deviceName)
                                << "\": " << alcGetString(nullptr, err) << " (" << err << ")";
            mStatus.store(VoiceCaptureStatus::Failed);
            return false;
        }

        const std::string opened = deviceName.empty() ? defaultDeviceName() : deviceName;
        {
            const std::lock_guard<std::mutex> lock(mNameMutex);
            mDeviceName = opened;
        }

        mHasDisconnectExt = alcIsExtensionPresent(mDevice, "ALC_EXT_disconnect") == ALC_TRUE;
        mStatus.store(VoiceCaptureStatus::Ok);

        Log(Debug::Info) << "Opened voice capture device \"" << opened << "\"";
        return true;
    }

    void VoiceCapture::close()
    {
        stop();

        const std::lock_guard<std::mutex> life(mLifecycleMutex);

        if (mDevice != nullptr)
        {
            alcCaptureCloseDevice(mDevice);
            mDevice = nullptr;
            mHasDisconnectExt = false;
        }

        // Reached even when there was no device: a refused open leaves the
        // status at Failed, and an object nobody has reopened since is closed,
        // not failing.
        mStatus.store(VoiceCaptureStatus::Closed);
        mLevel.store(0.0f);

        const std::lock_guard<std::mutex> lock(mNameMutex);
        mDeviceName.clear();
    }

    bool VoiceCapture::isOpen() const
    {
        return mDevice != nullptr;
    }

    void VoiceCapture::start(FrameCallback callback)
    {
        stop();

        const std::lock_guard<std::mutex> life(mLifecycleMutex);
        if (mDevice == nullptr)
            return;

        alcGetError(mDevice);
        alcCaptureStart(mDevice);
        const ALCenum err = alcGetError(mDevice);
        if (err != ALC_NO_ERROR)
        {
            // A device can open and then refuse to start - another process
            // taking the endpoint exclusively in between is the usual way. That
            // has to be caught here: a device that never starts delivers no
            // samples at all, and every later symptom is an absence.
            Log(Debug::Warning) << "Failed to start voice capture on \"" << deviceName()
                                << "\": " << alcGetString(mDevice, err) << " (" << err << ")";
            mStatus.store(VoiceCaptureStatus::Failed);
            return;
        }

        mCallback = std::move(callback);
        mQuitNow = false;
        mThread = std::thread([this] {
            mThreadId.store(std::this_thread::get_id());
            run();
        });
    }

    void VoiceCapture::stop()
    {
        // A callback reaching stop() is running on the very thread stop() would
        // join. Ask the loop to finish and let it unwind on its own; joining
        // here would be a self-join, which terminates.
        if (mThreadId.load() == std::this_thread::get_id())
        {
            mQuitNow = true;
            return;
        }

        const std::lock_guard<std::mutex> life(mLifecycleMutex);

        if (mThread.joinable())
        {
            {
                const std::lock_guard<std::mutex> lock(mMutex);
                mQuitNow = true;
            }
            mCondVar.notify_all();
            mThread.join();
            mThreadId.store(std::thread::id{});
            mCallback = nullptr;
        }

        if (mDevice != nullptr)
            alcCaptureStop(mDevice);

        mLevel.store(0.0f);
    }

    void VoiceCapture::setTransmitting(bool transmitting)
    {
        mTransmitting.store(transmitting);
    }

    bool VoiceCapture::isTransmitting() const
    {
        return mTransmitting.load();
    }

    VoiceCaptureStatus VoiceCapture::status() const
    {
        return mStatus.load();
    }

    float VoiceCapture::level() const
    {
        return mLevel.load(std::memory_order_relaxed);
    }

    std::string VoiceCapture::deviceName() const
    {
        const std::lock_guard<std::mutex> lock(mNameMutex);
        return mDeviceName;
    }

    void VoiceCapture::run()
    {
        std::vector<std::int16_t> frame(sVoiceFrameSamples);

        // Whatever the driver buffered between alcCaptureStart and this thread
        // being scheduled is already stale, and sending it would put the moment
        // before the player pressed the key on the air.
        {
            ALCint stale = 0;
            alcGetIntegerv(mDevice, ALC_CAPTURE_SAMPLES, 1, &stale);
            while (stale > 0)
            {
                const ALCsizei count = static_cast<ALCsizei>(std::min<ALCint>(stale, sVoiceFrameSamples));
                alcCaptureSamples(mDevice, frame.data(), count);
                stale -= count;
            }
        }

        auto lastConnectedCheck = std::chrono::steady_clock::now();
        auto lastAudible = lastConnectedCheck;

        // Nothing below is done while holding mMutex. The lock exists only to
        // pair with the condition variable at the bottom of the loop: holding
        // it across the ALC calls and the callback would mean stop() waits on
        // the audio backend, and a callback that stops capture - the obvious
        // response to the socket dying - would deadlock against itself.
        while (!mQuitNow)
        {
            bool audible = false;

            ALCint available = 0;
            alcGetIntegerv(mDevice, ALC_CAPTURE_SAMPLES, 1, &available);

            while (available >= sVoiceFrameSamples && !mQuitNow)
            {
                alcCaptureSamples(mDevice, frame.data(), static_cast<ALCsizei>(sVoiceFrameSamples));
                available -= sVoiceFrameSamples;

                std::uint64_t sumSquares = 0;
                bool anyNonZero = false;
                for (std::int16_t sample : frame)
                {
                    const std::int32_t value = sample;
                    sumSquares += static_cast<std::uint64_t>(value * value);
                    anyNonZero = anyNonZero || value != 0;
                }

                const float rms = std::sqrt(static_cast<float>(sumSquares) / sVoiceFrameSamples) / 32768.0f;
                mLevel.store(rms, std::memory_order_relaxed);

                audible = audible || anyNonZero;

                if (!mTransmitting.load())
                    continue;

                if (mCallback)
                    mCallback(frame.data(), frame.size());
            }

            // Judged on elapsed time rather than a frame count, because the two
            // ways a microphone can produce nothing are indistinguishable to
            // the player and only one of them delivers frames to count: Windows
            // privacy denial hands over frames of pure zeroes, while a device
            // that opened but never really started hands over nothing at all.
            const auto now = std::chrono::steady_clock::now();
            if (!mTransmitting.load() || audible)
            {
                lastAudible = now;
                if (audible)
                {
                    VoiceCaptureStatus silent = VoiceCaptureStatus::Silent;
                    if (mStatus.compare_exchange_strong(silent, VoiceCaptureStatus::Ok))
                        Log(Debug::Info) << "Voice capture device \"" << deviceName() << "\" is delivering audio again";
                }
            }
            else if (now - lastAudible >= sSilenceTimeout)
            {
                VoiceCaptureStatus ok = VoiceCaptureStatus::Ok;
                if (mStatus.compare_exchange_strong(ok, VoiceCaptureStatus::Silent))
                    Log(Debug::Warning) << "Voice capture device \"" << deviceName()
                                        << "\" is open but every sample is zero; the microphone is off or muted, "
                                           "nothing is routed into it if it is a virtual device, or Windows "
                                           "microphone privacy is denying this process";
            }

            // ALC_CONNECTED costs a round trip into the backend, and an
            // endpoint that has been unplugged stays unplugged, so once a
            // second is as often as this is worth asking.
            //
            // There is deliberately no attempt to follow a change of the
            // Windows default capture device: openal-soft does not migrate an
            // open capture device, and ALC_SOFT_reopen_device is playback-only
            // (openal-soft issue 686). The caller reopens on purpose instead.
            if (mHasDisconnectExt && now - lastConnectedCheck >= std::chrono::seconds(1))
            {
                lastConnectedCheck = now;
                ALCint connected = ALC_TRUE;
                alcGetIntegerv(mDevice, ALC_CONNECTED, 1, &connected);
                if (connected == ALC_FALSE
                    && mStatus.exchange(VoiceCaptureStatus::Disconnected) != VoiceCaptureStatus::Disconnected)
                    Log(Debug::Warning) << "Voice capture device \"" << deviceName() << "\" was disconnected";
            }

            // The predicate is what makes stop() prompt rather than merely
            // eventual: without it the wait would return only on the timeout,
            // and shutdown would quietly inherit whatever the poll interval
            // happens to be.
            std::unique_lock<std::mutex> lock(mMutex);
            mCondVar.wait_for(lock, std::chrono::milliseconds(sPollIntervalMs), [this] { return mQuitNow.load(); });
        }
    }

    bool runVoiceMicTest(const std::string& deviceName, const std::filesystem::path& outFile, int seconds)
    {
        const std::string defaultName = VoiceCapture::defaultDeviceName();
        const std::vector<std::string> devices = VoiceCapture::enumerateDevices();
        if (devices.empty())
            Log(Debug::Warning) << "Voice mic test: OpenAL reports no capture devices at all";
        for (const std::string& name : devices)
            Log(Debug::Info) << "Voice mic test: device \"" << name << "\""
                             << (name == defaultName ? " (default)" : "");

        VoiceCapture capture;
        if (!capture.open(deviceName))
        {
            Log(Debug::Error) << "Voice mic test: could not open \"" << (deviceName.empty() ? defaultName : deviceName)
                              << "\"";
            return false;
        }

        const int wanted = std::max(1, seconds);

        std::vector<std::int16_t> recorded;
        recorded.reserve(static_cast<std::size_t>(wanted) * sVoiceSampleRate);
        int frames = 0;
        int peak = 0;
        double rmsSum = 0.0;

        // These locals are touched only by the capture thread until stop()
        // joins it, and the join is what makes the results visible here.
        capture.start([&](const std::int16_t* pcm, std::size_t samples) {
            recorded.insert(recorded.end(), pcm, pcm + samples);
            double sumSquares = 0.0;
            for (std::size_t i = 0; i < samples; ++i)
            {
                const int value = pcm[i];
                peak = std::max(peak, std::abs(value));
                sumSquares += static_cast<double>(value) * value;
            }
            rmsSum += std::sqrt(sumSquares / static_cast<double>(samples)) / 32768.0;
            ++frames;
        });
        capture.setTransmitting(true);

        Log(Debug::Info) << "Voice mic test: recording " << wanted << "s from \"" << capture.deviceName() << "\"";
        for (int second = 0; second < wanted; ++second)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            Log(Debug::Info) << "Voice mic test: " << (second + 1) << "s, level " << capture.level();
        }

        capture.stop();
        // Read the verdict before the close, which resets the status to Closed.
        const VoiceCaptureStatus status = capture.status();
        capture.close();

        const float meanRms = frames > 0 ? static_cast<float>(rmsSum / frames) : 0.0f;

        if (writeMonoWav(outFile, recorded))
            Log(Debug::Info) << "Voice mic test: wrote " << recorded.size() << " samples to " << outFile;
        else
            Log(Debug::Error) << "Voice mic test: could not write " << outFile;

        Log(Debug::Info) << "Voice mic test: " << frames << " frames, peak " << peak << "/32768, mean RMS " << meanRms
                         << ", status " << describe(status);

        if (frames == 0)
        {
            Log(Debug::Error) << "Voice mic test: the device delivered no frames at all";
            return false;
        }
        if (peak == 0)
        {
            Log(Debug::Error) << "Voice mic test: every captured sample was zero; on Windows this is normally the "
                                 "microphone privacy setting denying this process";
            return false;
        }
        return true;
    }
}
