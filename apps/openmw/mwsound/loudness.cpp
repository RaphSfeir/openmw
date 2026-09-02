#include "loudness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace MWSound
{

    void Sound_Loudness::analyzeLoudness(const std::vector<char>& data)
    {
        mQueue.insert(mQueue.end(), data.begin(), data.end());
        if (!mQueue.size())
            return;

        int samplesPerSegment = static_cast<int>(mSampleRate / mSamplesPerSec);
        std::size_t numSamples = bytesToFrames(mQueue.size(), mChannelConfig, mSampleType);
        std::size_t advance = framesToBytes(1, mChannelConfig, mSampleType);

        std::size_t segment = 0;
        std::size_t sample = 0;
        while (segment < numSamples / samplesPerSegment)
        {
            float sum = 0;
            int samplesAdded = 0;
            while (sample < numSamples && sample < (segment + 1) * samplesPerSegment)
            {
                // get sample on a scale from -1 to 1
                float value = 0;
                if (mSampleType == SampleType_UInt8)
                    value = ((char)(mQueue[sample * advance] ^ 0x80)) / 128.f;
                else if (mSampleType == SampleType_Int16)
                {
                    value = *reinterpret_cast<const int16_t*>(&mQueue[sample * advance]);
                    value /= float(std::numeric_limits<int16_t>::max());
                }
                else if (mSampleType == SampleType_Float32)
                {
                    value = *reinterpret_cast<const float*>(&mQueue[sample * advance]);
                    value = std::clamp(value, -1.f, 1.f); // Float samples *should* be scaled to [-1,1] already.
                }

                sum += value * value;
                ++samplesAdded;
                ++sample;
            }

            float rms = 0; // root mean square
            if (samplesAdded > 0)
                rms = std::sqrt(sum / samplesAdded);
            if (mSamples.empty())
                mSamples.assign(sMaxSamples, 0.0f);
            mSamples[mTotalSamples % sMaxSamples] = rms;
            ++mTotalSamples;
            ++segment;
        }

        mQueue.erase(mQueue.begin(), mQueue.begin() + sample * advance);
    }

    float Sound_Loudness::getLoudnessAtTime(float sec) const
    {
        if (mSamplesPerSec <= 0.0f || mTotalSamples == 0 || sec < 0.0f)
            return 0.0f;

        size_t index = std::min(static_cast<size_t>(sec * mSamplesPerSec), mTotalSamples - 1);

        // Asking for something older than the history holds. Silence is the
        // honest answer; the modulo on its own would return whatever a minute
        // of audio later happens to have written over it.
        if (mTotalSamples > sMaxSamples && index < mTotalSamples - sMaxSamples)
            return 0.0f;

        return mSamples[index % sMaxSamples];
    }

}
