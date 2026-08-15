#ifndef OPENMW_MP_SERVER_CONFIG_H
#define OPENMW_MP_SERVER_CONFIG_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MPServer
{
    // Everything the server needs to know before it can answer anybody. There is
    // no game data here on purpose: the arbitration this binary runs was written
    // to decide from its own ledgers, and giving it a world to consult would
    // invite the dependency straight back in.
    struct Config
    {
        std::uint16_t mPort = 25565;
        unsigned mMaxPeers = 8;
        // The campaign IS the database: the same file a listen host writes, and
        // interchangeable with it byte for byte.
        std::string mCampaign;
        std::filesystem::path mDataDir = "."; // holds mp-campaigns/
        // Where the mod's Lua lives. The server runs the SAME modules the client
        // does, so this points at the mod directory rather than at a copy.
        std::filesystem::path mScriptDir;
        // The content list, in order and complete — .omwscripts entries included,
        // because they consume a content index and the handshake compares the
        // whole list element by element.
        std::vector<std::string> mContent;
        std::string mPassword;
        double mTimescale = 30.0;
        unsigned mTickHz = 60;

        // Reads a key=value file, then applies command-line overrides. Returns
        // false and explains itself when the result could not run.
        static bool load(int argc, char* argv[], Config& out, std::string& error);
    };
}

#endif // OPENMW_MP_SERVER_CONFIG_H
