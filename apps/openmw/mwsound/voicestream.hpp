#ifndef GAME_SOUND_VOICESTREAM_H
#define GAME_SOUND_VOICESTREAM_H

namespace MWSound
{
    // How one attached speaker is heard. Distances are Morrowind units, about
    // 70 to the metre, so the defaults are a 2 m reference distance and a 30 m
    // cutoff: a normal speaking voice carries across a room and is gone by the
    // far end of a street.
    //
    // Per speaker rather than global because the same attachment has to serve a
    // whisper, a conversation and a shout, and because the distances the say
    // path uses are read out of the GMSTs once per process into statics that
    // nothing can vary afterwards.
    struct VoiceStreamParams
    {
        float mRefDistance = 140.0f;
        float mMaxDistance = 2100.0f;

        // A gain floor, the way Mumble does it. Attenuation alone takes a voice
        // to nothing well before the speaker is out of sight, and a teammate
        // who has walked 25 m off should still be quietly present rather than
        // absent.
        float mMinGain = 0.15f;

        // Slightly steeper than the 1.0 the engine gives everything else, so
        // that being close reads as being close: a speaker at arm's length
        // should stand out against one across the room more than a sound effect
        // needs to.
        float mRolloff = 1.2f;

        // Party mode: heard flat, wherever the speaker is.
        bool mNonPositional = false;
    };
}

#endif // GAME_SOUND_VOICESTREAM_H
