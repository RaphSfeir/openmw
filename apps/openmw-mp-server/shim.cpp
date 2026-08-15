#include "shim.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <components/lua/serialization.hpp>
#include <components/net/session.hpp>

#include "config.hpp"

namespace MPServer
{
    namespace
    {
        constexpr std::uint8_t sReliableChannel = 0;
        constexpr std::uint8_t sUnreliableChannel = 1;

        double monotonicSeconds()
        {
            using namespace std::chrono;
            static const auto start = steady_clock::now();
            return duration<double>(steady_clock::now() - start).count();
        }

        bool isReliable(const sol::optional<sol::table>& options)
        {
            if (options)
                return options->get_or("reliable", true);
            return true;
        }

        // Mirrors mwlua/networkbindings.cpp deliberately, down to the channel
        // numbers: the two ends have to agree about what "reliable" means, and
        // the cheapest guarantee of that is the same code twice rather than a
        // shared header nobody reads.
        sol::table networkPackage(sol::state& lua, Net::Session& session)
        {
            sol::table api(lua, sol::create);
            api["host"] = [&session](sol::optional<int> port, sol::optional<int> maxPeers) {
                session.requestHost(static_cast<std::uint16_t>(port.value_or(25565)),
                    static_cast<unsigned>(std::max(1, maxPeers.value_or(8))));
            };
            api["connect"] = [](std::string_view, sol::optional<int>) {
                // A server does not join anybody. Refused rather than ignored:
                // a module calling this is a module that thinks it is a client.
                throw std::runtime_error("network.connect: the dedicated server never joins a session");
            };
            api["disconnect"] = [&session]() { session.requestDisconnect(); };
            api["state"] = [&session](sol::this_state s) {
                sol::state_view view(s);
                sol::table state(view, sol::create);
                // Always "host": wire.isHost() gates the arbitration, and the
                // server IS the authority. There is no player behind it, which
                // is what PROTO 15's id-0 rules are about.
                state["role"] = session.getRole() == Net::Role::None ? "none" : "host";
                state["connected"] = session.isConnected();
                sol::table peers(view, sol::create);
                int index = 1;
                for (std::uint32_t id : session.getPeers())
                    peers[index++] = id;
                state["peers"] = peers;
                const std::string error = session.getLastError();
                if (!error.empty())
                    state["error"] = error;
                return state;
            };
            api["send"] = [&session](std::uint32_t peer, const sol::object& data,
                              sol::optional<sol::table> options) {
                const bool reliable = isReliable(options);
                session.send(peer, reliable ? sReliableChannel : sUnreliableChannel, reliable,
                    LuaUtil::serialize(data, nullptr));
            };
            api["broadcast"] = [&session](const sol::object& data, sol::optional<sol::table> options) {
                const bool reliable = isReliable(options);
                session.broadcast(reliable ? sReliableChannel : sUnreliableChannel, reliable,
                    LuaUtil::serialize(data, nullptr));
            };
            api["receive"] = [&session](sol::this_state s) {
                sol::state_view view(s);
                sol::table result(view, sol::create);
                int index = 1;
                for (Net::Event& event : session.drainEvents())
                {
                    sol::table entry(view, sol::create);
                    entry["peer"] = event.mPeer;
                    switch (event.mType)
                    {
                        case Net::Event::Type::Connect:
                            entry["type"] = "connect";
                            break;
                        case Net::Event::Type::Disconnect:
                            entry["type"] = "disconnect";
                            break;
                        case Net::Event::Type::Message:
                        {
                            entry["type"] = "message";
                            entry["channel"] = event.mChannel;
                            try
                            {
                                entry["data"] = LuaUtil::deserialize(view, event.mData, nullptr);
                            }
                            catch (const std::exception& e)
                            {
                                std::cerr << "dropping malformed message from peer " << event.mPeer << ": "
                                          << e.what() << std::endl;
                                continue;
                            }
                            break;
                        }
                    }
                    result[index++] = entry;
                }
                return result;
            };
            return api;
        }

        // The same store the client writes, in the same format, at
        // <data>/mp-campaigns/<name>.bin — so a campaign can be carried from a
        // listen host to a server and back by copying one file.
        sol::table campaignPackage(sol::state& lua, const Config& config)
        {
            sol::table api(lua, sol::create);
            const std::filesystem::path dir = config.mDataDir / "mp-campaigns";

            auto fileFor = [dir](std::string_view name) {
                if (name.empty() || name.size() > 64)
                    throw std::runtime_error("campaign: invalid name");
                for (const char c : name)
                {
                    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '-' || c == '_' || c == '@';
                    if (!ok)
                        throw std::runtime_error("campaign: invalid name (a-z, A-Z, 0-9, '-', '_', '@' only)");
                }
                return dir / (std::string(name) + ".bin");
            };

            api["write"] = [fileFor](std::string_view name, const sol::object& data) {
                const std::filesystem::path file = fileFor(name);
                std::filesystem::path tmp = file;
                tmp += ".tmp";
                std::filesystem::create_directories(file.parent_path());
                const std::string binary = LuaUtil::serialize(data, nullptr);
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
            api["read"] = [fileFor](std::string_view name, sol::this_state s) -> sol::object {
                sol::state_view view(s);
                const std::filesystem::path file = fileFor(name);
                if (!std::filesystem::exists(file))
                    return sol::nil;
                std::ifstream in(file, std::ios::binary);
                if (!in)
                    throw std::runtime_error("campaign.read: cannot open " + file.string());
                const std::string binary{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
                return LuaUtil::deserialize(view, binary, nullptr);
            };
            api["list"] = [dir](sol::this_state s) {
                sol::state_view view(s);
                sol::table result(view, sol::create);
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
            api["remove"] = [fileFor](std::string_view name) { std::filesystem::remove(fileFor(name)); };
            return api;
        }

        sol::table corePackage(sol::state& lua, const Config& config)
        {
            sol::table api(lua, sol::create);
            sol::table contentFiles(lua, sol::create);
            sol::table list(lua, sol::create);
            int index = 1;
            for (const std::string& name : config.mContent)
            {
                // Lowercased, because that is how the engine reports its own
                // list and the handshake compares the two element by element.
                // Configured as "Morrowind.esm" and compared against the
                // engine's "morrowind.esm", every client is refused for a
                // difference nobody can see.
                std::string normalized = name;
                for (char& c : normalized)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                list[index++] = normalized;
            }
            contentFiles["list"] = list;
            api["contentFiles"] = contentFiles;
            // Simulation and real time are the same thing here: nothing pauses,
            // and there is no frame to be behind.
            api["getSimulationTime"] = []() { return monotonicSeconds(); };
            api["getRealTime"] = []() { return monotonicSeconds(); };
            // Game time is the SERVER's clock and belongs to Lua (M8.2 owns the
            // calendar). Until then it is honest about not knowing: zero, not a
            // plausible-looking number that would be silently written into
            // every campaign delta as its timestamp.
            api["getGameTime"] = []() { return 0.0; };
            api["API_REVISION"] = 147;
            return api;
        }

        sol::table worldPackage(sol::state& lua)
        {
            sol::table api(lua, sol::create);
            // Empty, and that is the point: there is no player behind the
            // authority. Modules that iterate it simply find nobody, which is
            // the correct answer rather than an error to work around.
            api["players"] = sol::table(lua, sol::create);
            api["getObjectByFormId"] = [](std::string_view) { return sol::nil; };
            api["getCellByName"] = [](std::string_view) { return sol::nil; };
            api["getPausedTags"] = [](sol::this_state s) { return sol::table(sol::state_view(s), sol::create); };
            api["unpause"] = [](sol::optional<std::string_view>) {};
            api["pause"] = [](sol::optional<std::string_view>) {};
            return api;
        }
    }

    void installShim(sol::state& lua, Net::Session& session, const Config& config)
    {
        sol::table loaded = lua["package"]["loaded"];
        loaded["openmw.network"] = networkPackage(lua, session);
        loaded["openmw.campaign"] = campaignPackage(lua, config);
        loaded["openmw.core"] = corePackage(lua, config);
        loaded["openmw.world"] = worldPackage(lua);

        // The mod's logging convention, kept identical so the same greps work
        // against a server log and a client one.
        lua.set_function("print", [](sol::variadic_args args) {
            std::string line;
            for (auto arg : args)
            {
                if (!line.empty())
                    line += '\t';
                line += sol::state_view(arg.lua_state())["tostring"](arg).get<std::string>();
            }
            std::cout << line << std::endl;
        });
    }
}
