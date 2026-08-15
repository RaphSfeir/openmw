#include "campaignbindings.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <sol/sol.hpp>

#include <components/lua/luastate.hpp>
#include <components/lua/serialization.hpp>

#include "context.hpp"
#include "luamanagerimp.hpp"

namespace MWLua
{
    namespace
    {
        // Names are constrained rather than sanitized: a rejected name is a bug
        // the caller should hear about, not a silently renamed file.
        bool isValidName(std::string_view name)
        {
            if (name.empty() || name.size() > 64)
                return false;
            for (const char c : name)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '-' || c == '_' || c == '@';
                if (!ok)
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
