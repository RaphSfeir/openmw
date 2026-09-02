#include "apps/openmw/mwsound/soundmanagerimp.hpp"

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include <components/testing/util.hpp>
#include <components/vfs/filemap.hpp>
#include <components/vfs/manager.hpp>

#include "apps/openmw/mwworld/ptr.hpp"

namespace MWSound
{
    namespace
    {
        using namespace testing;

        // No output device, which is what makes this testable at all: nothing
        // reaches OpenAL and the counter is asked the one question it exists to
        // answer. It is deliberately independent of whether anything was
        // attached - clear() cannot know who was borrowing a handle, so it
        // always reports, and the holder compares rather than being told.
        struct SilentSoundManager
        {
            std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS(VFS::FileMap{});
            SoundManager mSounds{ mVFS.get(), false };
        };

        TEST(MWSoundClearGenerationTest, shouldNotMoveWhileNothingClearsTheVoiceContainers)
        {
            SilentSoundManager fixture;
            const std::uint64_t start = fixture.mSounds.clearGeneration();

            EXPECT_EQ(fixture.mSounds.clearGeneration(), start);
        }

        TEST(MWSoundClearGenerationTest, shouldMoveWhenClearRuns)
        {
            SilentSoundManager fixture;
            const std::uint64_t start = fixture.mSounds.clearGeneration();

            fixture.mSounds.clear();

            EXPECT_NE(fixture.mSounds.clearGeneration(), start);
        }

        TEST(MWSoundClearGenerationTest, shouldMoveOncePerClear)
        {
            SilentSoundManager fixture;
            const std::uint64_t start = fixture.mSounds.clearGeneration();

            fixture.mSounds.clear();
            const std::uint64_t once = fixture.mSounds.clearGeneration();
            fixture.mSounds.clear();

            // Two loads in a row have to read as two resets and not as one, or
            // a holder that latched the first would carry its handles through
            // the second.
            EXPECT_NE(once, start);
            EXPECT_NE(fixture.mSounds.clearGeneration(), once);
        }

        TEST(MWSoundClearGenerationTest, shouldNotMoveForVoiceRemovalsTheCallerAskedFor)
        {
            SilentSoundManager fixture;
            const std::uint64_t start = fixture.mSounds.clearGeneration();

            // Every other way an entry leaves the voice containers. The two
            // stops are the owner's own doing, so it has nothing stale to drop
            // afterwards; the rest never touch them at all. stopSound3D(ptr) is
            // the one path that reaches stopVoiceStream from outside voice, and
            // it has no callers anywhere in the tree today - if it gains one,
            // the counter will not cover it and per-container liveness will
            // have to.
            fixture.mSounds.stopVoiceTrack(1);
            fixture.mSounds.stopVoiceStream(MWWorld::ConstPtr());
            fixture.mSounds.stopSound3D(MWWorld::ConstPtr());
            fixture.mSounds.stopSound(static_cast<const MWWorld::CellStore*>(nullptr));
            fixture.mSounds.updatePtr(MWWorld::ConstPtr(), MWWorld::ConstPtr());
            fixture.mSounds.stopMusic();

            EXPECT_EQ(fixture.mSounds.clearGeneration(), start);
        }
    }
}
