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
        // Roughly 3.5 m and 60 m at Morrowind's ~70 units per metre.
        //
        // The first numbers tried were 2 m and 30 m, taken from what other games
        // use, and in play they were both too small: attenuation starts the
        // moment you are not nose to nose, and people who could plainly see each
        // other could not hear each other. Reference distance is how far full
        // volume carries, so it wants to be conversational range rather than
        // arm's length.
        float mRefDistance = 250.0f;
        float mMaxDistance = 4200.0f;

        // A gain floor, the way Mumble does it. Attenuation alone takes a voice
        // to nothing well before the speaker is out of sight, and a teammate
        // who has walked off should still be quietly present rather than absent.
        float mMinGain = 0.25f;

        // Gentler than the 1.0 the engine gives everything else. Steeper was the
        // first instinct -- close should read as close -- but speech is not a
        // sound effect: it carries information, and an inverse curve that makes
        // a nearby speaker vivid makes a slightly-further one unintelligible.
        float mRolloff = 0.9f;

        // Party mode: heard flat, wherever the speaker is.
        bool mNonPositional = false;

        // This listener's opinion of THIS speaker, multiplied into the sound's
        // own volume rather than the category's, so it composes with the global
        // Voice slider instead of replacing it. Zero really is silent: the
        // min-gain floor is rescaled by the source gain on every update, so it
        // collapses along with it (openaloutput.cpp, updateCommon).
        //
        // Deliberately NOT part of sameGeometry below. Volume is one of the
        // properties updateStream rewrites every frame, so it can be changed on
        // somebody who is talking right now without rebuilding their stream.
        float mGain = 1.0f;

        // Reference distance, max distance and rolloff are written to the AL
        // source ONCE, in initCommon3D; only gain-like properties are refreshed
        // by the per-frame updateCommon. So changing any of THESE on a speaker
        // who is already being heard means recreating their stream, and the
        // caller has to be able to tell that it needs to.
        bool sameGeometry(const VoiceStreamParams& o) const
        {
            return mRefDistance == o.mRefDistance && mMaxDistance == o.mMaxDistance && mMinGain == o.mMinGain
                && mRolloff == o.mRolloff && mNonPositional == o.mNonPositional;
        }
    };
}

#endif // GAME_SOUND_VOICESTREAM_H
