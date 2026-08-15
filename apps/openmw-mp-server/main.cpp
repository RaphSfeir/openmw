// The dedicated server: transport, arbitration and the campaign registry, with
// no game engine behind any of it.
//
// It runs the SAME Lua the listen host runs. That is the whole design: the
// arbitration was rewritten (M8.0) to decide from its own ledgers instead of
// from a loaded world, so the modules can be loaded here unchanged and this
// binary is "the host's decisions, headless". Anything that still needs a world
// stays delegated to a client.
//
// What is NOT here, deliberately: no VFS, no ScriptsContainer, no game data.
// The engine's Lua sandboxing exists to isolate mods from each other and from
// the engine; a server that runs one trusted entry script needs none of it, and
// taking it would drag in the very dependencies this binary exists without.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <sol/sol.hpp>

#include <components/lua/serialization.hpp>
#include <components/net/session.hpp>

#include "config.hpp"
#include "shim.hpp"

namespace
{
    std::atomic_bool sStopping{ false };

    void onSignal(int)
    {
        // Only a flag: the shutdown itself (flushing the campaign, saying
        // goodbye to every peer) has to happen on the tick thread, where the
        // Lua state can safely be touched.
        sStopping.store(true);
    }
}

int main(int argc, char* argv[])
{
    MPServer::Config config;
    std::string error;
    if (!MPServer::Config::load(argc, argv, config, error))
    {
        std::cerr << error << std::endl;
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Net::Session session;
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math,
        sol::lib::os, sol::lib::package, sol::lib::coroutine, sol::lib::debug);

    // The mod is required by module path exactly as the engine requires it, so
    // scripts.mp.wire resolves to <scripts>/scripts/mp/wire.lua and the files
    // are the ones the client loads — not a copy that can drift.
    const std::string root = config.mScriptDir.string();
    lua["package"]["path"] = root + "/?.lua;" + root + "/?/init.lua";

    try
    {
        MPServer::installShim(lua, session, config);
    }
    catch (const std::exception& e)
    {
        std::cerr << "shim: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    sol::table server;
    try
    {
        sol::protected_function_result loaded = lua.safe_script("return require('scripts.mp.server_main')");
        if (!loaded.valid())
        {
            sol::error err = loaded;
            throw std::runtime_error(err.what());
        }
        server = loaded;
    }
    catch (const std::exception& e)
    {
        std::cerr << "server_main: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    // The campaign is opened before a single peer is answered: a client that
    // connected first and started changing things would be writing into a
    // registry that was not open yet.
    sol::protected_function start = server["start"];
    if (start.valid())
    {
        sol::protected_function_result opened = start(config.mCampaign, config.mTimescale, config.mPassword);
        if (!opened.valid())
        {
            sol::error err = opened;
            std::cerr << "start: " << err.what() << std::endl;
            return EXIT_FAILURE;
        }
        if (opened.get_type() == sol::type::boolean && !opened.get<bool>())
        {
            // A campaign that refused to open (wrong content list, wrong
            // format) must stop the server rather than quietly begin a
            // different world under the same name.
            std::cerr << "refusing to run: the campaign could not be opened" << std::endl;
            return EXIT_FAILURE;
        }
    }

    sol::protected_function update = server["update"];
    sol::protected_function shutdown = server["shutdown"];
    if (!update.valid())
    {
        std::cerr << "server_main must return a table with an update(dt) function" << std::endl;
        return EXIT_FAILURE;
    }

    session.requestHost(config.mPort, config.mMaxPeers);
    std::cout << "openmw-mp-server: listening on port " << config.mPort << ", campaign \""
              << config.mCampaign << "\", " << config.mContent.size() << " content file(s)" << std::endl;

    const auto period = std::chrono::duration<double>(1.0 / static_cast<double>(config.mTickHz));
    auto previous = std::chrono::steady_clock::now();
    while (!sStopping.load())
    {
        const auto start = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(start - previous).count();
        previous = start;

        // Networking first, so the Lua tick reacts to what arrived rather than
        // to what arrived last time round.
        session.pump();

        sol::protected_function_result result = update(dt);
        if (!result.valid())
        {
            // A throwing tick is reported and survived: one broken domain must
            // not take a running world down with it, the same rule global.lua
            // applies per module.
            sol::error err = result;
            std::cerr << "tick: " << err.what() << std::endl;
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < period)
            std::this_thread::sleep_for(period - elapsed);
    }

    std::cout << "openmw-mp-server: stopping" << std::endl;
    if (shutdown.valid())
    {
        sol::protected_function_result result = shutdown();
        if (!result.valid())
        {
            sol::error err = result;
            std::cerr << "shutdown: " << err.what() << std::endl;
        }
    }
    session.requestDisconnect();
    session.pump();
    return EXIT_SUCCESS;
}
