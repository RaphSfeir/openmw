#ifndef MWLUA_MAPMEMORY_H
#define MWLUA_MAPMEMORY_H

#include <filesystem>

namespace MWLua
{
    // MP: THE MAP MEMORY OF A PROFILE. Multiplayer never loads a savegame, so
    // everything the engine keeps only there was gone at every launch: the
    // world map's explored overlay and visited places (REC_GMAP), the custom
    // markers (REC_MARK), the quick keys (REC_KEYS), the selected spell
    // (REC_ASPL) and every cell's local fog of war. These write and read
    // exactly those records, with the engine's own save and load code, to a
    // file of their own -- client-only state, by the owner's rule; the server
    // never sees it. MAIN THREAD ONLY (they touch the GUI and the scene): the
    // bindings queue them as delayed actions. Both throw on failure; load
    // expects the file to exist.
    void saveMapMemory(const std::filesystem::path& file);
    void loadMapMemory(const std::filesystem::path& file);
}

#endif
