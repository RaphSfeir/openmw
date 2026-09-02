#ifndef GAME_SOUND_LOUDNESS_H
#define GAME_SOUND_LOUDNESS_H

#include <deque>
#include <vector>

#include "sounddecoder.hpp"

namespace MWSound
{

    class Sound_Loudness
    {
        float mSamplesPerSec;
        int mSampleRate;
        ChannelConfig mChannelConfig;
        SampleType mSampleType;

        // A minute of history, and no more. Every consumer reads at the current
        // playback offset, which trails the write head by the output queue and
        // nothing else, so older values are never asked for - but a multiplayer
        // voice stream is attached for as long as the speaker is present and is
        // analysed even while they are silent, so an unbounded history would
        // accrue for the whole session with nothing to reclaim it. A minute is
        // also longer than any spoken line in the game, which is why the vanilla
        // callers cannot tell the difference.
        static constexpr std::size_t sMaxSamples = 1200;

        // Written at mTotalSamples % sMaxSamples. The count itself does not
        // wrap: it is what says whether an index has been evicted.
        std::vector<float> mSamples;
        std::size_t mTotalSamples = 0;

        std::deque<char> mQueue;

    public:
        /**
         * @param samplesPerSecond How many loudness values per second of audio to compute.
         * @param sampleRate the sample rate of the sound buffer
         * @param chans channel layout of the buffer
         * @param type sample type of the buffer
         */
        Sound_Loudness(float samplesPerSecond, int sampleRate, ChannelConfig chans, SampleType type)
            : mSamplesPerSec(samplesPerSecond)
            , mSampleRate(sampleRate)
            , mChannelConfig(chans)
            , mSampleType(type)
        {
        }

        /**
         * Analyzes the energy (closely related to loudness) of a sound buffer.
         * The buffer will be divided into segments according to \a valuesPerSecond,
         * and for each segment a loudness value in the range of [0,1] will be computed.
         * The computed values are then added to the mSamples vector. This method should be called continuously
         * with chunks of audio until the whole audio file is processed.
         * If the size of \a data does not exactly fit a number of loudness samples, the remainder
         * will be kept in the mQueue and used in the next call to analyzeLoudness.
         * @param data the sound buffer to analyze, containing raw samples
         */
        void analyzeLoudness(const std::vector<char>& data);

        /**
         * Get loudness at a particular time. Before calling this, the stream has to be analyzed up to that point in
         * time (see analyzeLoudness()).
         */
        float getLoudnessAtTime(float sec) const;
    };

}

#endif /* GAME_SOUND_LOUDNESS_H */
