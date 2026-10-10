#include "mapmemory.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/fourcc.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/formatversion.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

namespace MWLua
{
    // Both run on the MAIN thread only: the bindings queue them as delayed
    // actions, and the engine's own calls at quit and cleanup are main-thread.
    // This is the client engine, which always has a window manager and a world
    // (Environment hands out NotNullPtr); the dedicated server is another binary.
    void saveMapMemory(const std::filesystem::path& file)
    {
        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        MWBase::World* world = MWBase::Environment::get().getWorld();
        // A sibling temp file renamed over the target: a crash mid-write can
        // never leave a truncated memory behind (the campaign files do the same).
        std::filesystem::path tmp = file;
        tmp += ".tmp";
        std::filesystem::create_directories(file.parent_path());
        {
            std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
            if (!stream)
                throw std::runtime_error("map memory: cannot open " + tmp.string());
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.setVersion(0);
            writer.setAuthor("");
            writer.setDescription("");
            writer.setRecordCount(0); // the reader walks to the end of the file
            writer.save(stream);
            windowManager->write(writer, *windowManager->getLoadingScreen());
            world->writeFogRecords(writer);
            writer.close();
            stream.flush();
            if (!stream)
                throw std::runtime_error("map memory: failed writing " + tmp.string());
        }
        std::filesystem::rename(tmp, file);
    }

    void loadMapMemory(const std::filesystem::path& file)
    {
        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        MWBase::World* world = MWBase::Environment::get().getWorld();
        ESM::ESMReader reader;
        reader.open(file);
        // The records are laid out by the format version in the header; one
        // from another engine would be read by another layout, not refused.
        const ESM::FormatVersion version = reader.getFormatVersion();
        if (version > ESM::CurrentSaveGameFormatVersion || version < ESM::MinSupportedSaveGameFormatVersion)
            throw std::runtime_error("map memory " + file.string() + ": format version "
                + std::to_string(version) + " is not one this engine reads (" + std::to_string(ESM::MinSupportedSaveGameFormatVersion)
                + ".." + std::to_string(ESM::CurrentSaveGameFormatVersion) + ")");
        while (reader.hasMoreRecs())
        {
            const ESM::NAME n = reader.getRecName();
            reader.getRecHeader();
            switch (n.toInt())
            {
                case ESM::REC_GMAP:
                case ESM::REC_KEYS:
                case ESM::REC_ASPL:
                case ESM::REC_MARK:
                    windowManager->readRecord(reader, n.toInt());
                    break;
                case ESM::fourCC("MPFG"):
                    world->readFogRecord(reader);
                    break;
                default:
                    reader.skipRecord();
            }
        }
        reader.close();
        // The markers were added without their event (as a savegame load adds
        // them, which then rebuilds every window); here the windows stand, so
        // the event is sent once for all of them.
        windowManager->customMarkersChanged();
    }
}
