#include "voicestream.hpp"

namespace MWSound
{
    // The header is the whole of this unit; the checks are here so that a
    // default nobody can hear, or one the distance model rejects outright,
    // fails the build instead of the drill.
    static_assert(VoiceStreamParams{}.mRefDistance > 0.0f);
    static_assert(VoiceStreamParams{}.mRefDistance < VoiceStreamParams{}.mMaxDistance);
    static_assert(VoiceStreamParams{}.mMinGain >= 0.0f && VoiceStreamParams{}.mMinGain <= 1.0f);
    static_assert(VoiceStreamParams{}.mRolloff >= 0.0f);
}
