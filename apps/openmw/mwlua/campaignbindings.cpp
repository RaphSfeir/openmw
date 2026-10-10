#include "campaignbindings.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <sol/sol.hpp>
#include <yaml-cpp/yaml.h>

#include <components/lua/luastate.hpp>
#include <components/lua/jsonmirror.hpp>
#include <components/lua/serialization.hpp>

#include "context.hpp"
#include "luamanagerimp.hpp"
#include "mapmemory.hpp"

namespace MWLua
{
    namespace
    {
        // Names are constrained rather than sanitized: a rejected name is a bug
        // the caller should hear about, not a silently renamed file.
        // TES3MP names its per-cell files by the cell description itself
        // ("Balmora, Guild of Mages.json", "-3, -2.json") and only refuses what
        // an OS refuses. The campaign's cell shards do the same, so the rule
        // admits the characters Morrowind cell names actually use -- spaces,
        // commas, apostrophes, periods, parentheses -- and rejects the
        // filesystem-illegal set, control characters, path separators, a
        // leading/trailing space or period (Windows) and "..". Still
        // constrained rather than sanitized: a rejected name is a bug the
        // caller hears about.
        // The DOS device names: a file called NUL or CON1.x opens the device
        // on Windows, whatever the extension. The stem is what is reserved.
        bool isReservedStem(std::string_view name)
        {
            std::string stem(name.substr(0, name.find('.')));
            for (char& c : stem)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
                return true;
            if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)
                && stem[3] >= '0' && stem[3] <= '9')
                return true;
            return false;
        }

        bool isValidName(std::string_view name)
        {
            if (name.empty() || name.size() > 200)
                return false;
            if (name.front() == ' ' || name.front() == '.' || name.back() == ' ' || name.back() == '.')
                return false;
            if (name.find("..") != std::string_view::npos)
                return false;
            if (isReservedStem(name))
                return false;
            for (const char c : name)
            {
                const unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 0x20 || uc == 0x7f)
                    return false;
                const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                const bool punct = c == '-' || c == '_' || c == '@' || c == ' ' || c == '.' || c == ','
                    || c == '\'' || c == '(' || c == ')';
                if (!alnum && !punct)
                    return false;
            }
            return true;
        }

        std::filesystem::path campaignDir(const Context& context)
        {
            return context.mLuaManager->userConfigPath() / "mp-campaigns";
        }

        std::filesystem::path fileFor(const Context& context, std::string_view name)
        {
            if (!isValidName(name))
                throw std::runtime_error("campaign: invalid name (a-z, A-Z, 0-9, '-', '_', '@' only)");
            return campaignDir(context) / (std::string(name) + ".bin");
        }

        std::filesystem::path jsonFileFor(const Context& context, std::string_view name)
        {
            if (!isValidName(name))
                throw std::runtime_error("campaign: invalid name (a-z, A-Z, 0-9, '-', '_', '@' only)");
            return campaignDir(context) / (std::string(name) + ".json");
        }

        // The map memory lives beside the campaigns, under the same name rule.
        std::filesystem::path mapFileFor(const Context& context, std::string_view name)
        {
            if (!isValidName(name))
                throw std::runtime_error("campaign: invalid map name (a-z, A-Z, 0-9, '-', '_', '@' only)");
            return context.mLuaManager->userConfigPath() / "mp-maps" / (std::string(name) + ".omwmap");
        }

    }

    sol::table initCampaignPackage(const Context& context)
    {
        sol::table api(context.sol(), sol::create);
        const LuaUtil::UserdataSerializer* serializer = context.mSerializer;

        // Serialize with the engine's own codec (the same one events and the wire
        // use), write to a sibling temp file, then rename over the target: a crash
        // mid-write can never leave a truncated campaign behind.
        api["write"] = [context, serializer](std::string_view name, const sol::object& data) {
            const std::filesystem::path file = fileFor(context, name);
            std::filesystem::path tmp = file;
            tmp += ".tmp";
            std::filesystem::create_directories(file.parent_path());
            const std::string binary = LuaUtil::serialize(data, serializer);
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    throw std::runtime_error("campaign.write: cannot open " + tmp.string());
                out.write(binary.data(), static_cast<std::streamsize>(binary.size()));
                out.flush();
                if (!out)
                    throw std::runtime_error("campaign.write: failed writing " + tmp.string());
            }
            std::filesystem::rename(tmp, file);
        };

        // THE MAP MEMORY OF A PROFILE (mapmemory.hpp): the records the engine
        // keeps only in a savegame -- explored world map, visited places,
        // custom markers, quick keys, selected spell, every cell's fog --
        // written to and read from a file of their own under mp-maps/.
        // False when this process has no map to speak of (the dedicated
        // server) or, on load, no file. The last name saved under is written
        // once more by the engine itself at quit, while the world is whole.
        // The name is checked here, in the caller's frame, so a bad one is a
        // Lua error; the work itself is QUEUED like every other engine
        // mutation (the GUI and the scene are main-thread property and Lua may
        // be running off it), and a failure inside it is logged by the queue.
        // saveMap answers true for 'queued'; loadMap answers whether there is a
        // file to read, and reads it when there is.
        api["saveMap"] = [context](std::string_view name) -> bool {
            const std::filesystem::path file = mapFileFor(context, name);
            context.mLuaManager->addAction(
                [file, manager = context.mLuaManager] {
                    saveMapMemory(file);
                    manager->setMapMemoryFile(file);
                },
                "saveMap");
            return true;
        };

        api["loadMap"] = [context](std::string_view name) -> bool {
            const std::filesystem::path file = mapFileFor(context, name);
            if (!std::filesystem::exists(file))
                return false;
            context.mLuaManager->addAction([file] { loadMapMemory(file); }, "loadMap");
            return true;
        };

        api["read"] = [context, serializer](std::string_view name, sol::this_state s) -> sol::object {
            sol::state_view lua(s);
            const std::filesystem::path file = fileFor(context, name);
            if (!std::filesystem::exists(file))
                return sol::nil;
            std::ifstream in(file, std::ios::binary);
            if (!in)
                throw std::runtime_error("campaign.read: cannot open " + file.string());
            const std::string binary{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
            return LuaUtil::deserialize(lua, binary, serializer);
        };

        // Same atomic write as the binary path: temp file, then rename. A dump
        // interrupted half way must not leave a file that parses as a truncated
        // campaign -- the whole point of the mirror is to be trustworthy when
        // something has gone wrong.
        api["writeJson"] = [context](std::string_view name, const sol::object& data) {
            const std::filesystem::path file = jsonFileFor(context, name);
            std::filesystem::path tmp = file;
            tmp += ".tmp";
            std::filesystem::create_directories(file.parent_path());
            YAML::Emitter emitter;
            emitter << YAML::DoubleQuoted << YAML::Flow;
            LuaUtil::JsonMirror::write(data, emitter);
            if (!emitter.good())
                throw std::runtime_error("campaign.writeJson: " + emitter.GetLastError());
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    throw std::runtime_error("campaign.writeJson: cannot open " + tmp.string());
                out << emitter.c_str() << '\n';
                out.flush();
                if (!out)
                    throw std::runtime_error("campaign.writeJson: failed writing " + tmp.string());
            }
            std::filesystem::rename(tmp, file);
        };

        api["readJson"] = [context](std::string_view name, sol::this_state s) -> sol::object {
            sol::state_view lua(s);
            const std::filesystem::path file = jsonFileFor(context, name);
            if (!std::filesystem::exists(file))
                return sol::nil;
            try
            {
                const YAML::Node root = YAML::LoadFile(file.string());
                return LuaUtil::JsonMirror::read(root, lua);
            }
            catch (const YAML::Exception& e)
            {
                // Named and rethrown: an import that silently half-applies a
                // broken file is worse than one that refuses.
                throw std::runtime_error("campaign.readJson: " + file.string() + ": " + e.what());
            }
        };

        api["list"] = [context](sol::this_state s) {
            sol::state_view lua(s);
            sol::table result(lua, sol::create);
            const std::filesystem::path dir = campaignDir(context);
            if (!std::filesystem::exists(dir))
                return result;
            int index = 1;
            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".bin")
                    result[index++] = entry.path().stem().string();
            }
            return result;
        };

        api["remove"] = [context](std::string_view name) {
            std::filesystem::remove(fileFor(context, name));
        };

        return LuaUtil::makeReadOnly(api);
    }
}
