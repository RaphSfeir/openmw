#include "networkbindings.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include <sol/sol.hpp>

#include <components/debug/debuglog.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/serialization.hpp>
#include <components/net/session.hpp>

#include "context.hpp"
#include "luamanagerimp.hpp"

namespace MWLua
{
    namespace
    {
        constexpr std::uint8_t sReliableChannel = 0;
        constexpr std::uint8_t sUnreliableChannel = 1;

        std::string_view roleToString(Net::Role role)
        {
            switch (role)
            {
                case Net::Role::Host:
                    return "host";
                case Net::Role::Client:
                    return "client";
                default:
                    return "none";
            }
        }

        bool isReliable(const sol::optional<sol::table>& options)
        {
            if (options)
                return options->get_or("reliable", true);
            return true;
        }

        void addControlFunctions(sol::table& api, Net::Session* session)
        {
            api["host"] = [session](sol::optional<int> port, sol::optional<int> maxPeers) {
                int portValue = port.value_or(25565);
                if (portValue <= 0 || portValue > 65535)
                    throw std::runtime_error("network.host: invalid port");
                session->requestHost(static_cast<std::uint16_t>(portValue), std::max(1, maxPeers.value_or(8)));
            };
            api["connect"] = [session](std::string_view address, sol::optional<int> port) {
                int portValue = port.value_or(25565);
                if (address.empty())
                    throw std::runtime_error("network.connect: address is required");
                if (portValue <= 0 || portValue > 65535)
                    throw std::runtime_error("network.connect: invalid port");
                session->requestConnect(std::string(address), static_cast<std::uint16_t>(portValue));
            };
            api["disconnect"] = [session]() { session->requestDisconnect(); };
            api["state"] = [session](sol::this_state s) {
                sol::state_view lua(s);
                sol::table state(lua, sol::create);
                state["role"] = roleToString(session->getRole());
                state["connected"] = session->isConnected();
                sol::table peers(lua, sol::create);
                int index = 1;
                for (std::uint32_t id : session->getPeers())
                    peers[index++] = id;
                state["peers"] = peers;
                std::string error = session->getLastError();
                if (!error.empty())
                    state["error"] = error;
                return state;
            };
        }
    }

    sol::table initNetworkPackage(const Context& context)
    {
        sol::table api(context.sol(), sol::create);
        Net::Session* session = &context.mLuaManager->netSession();
        const LuaUtil::UserdataSerializer* serializer = context.mSerializer;

        addControlFunctions(api, session);

        api["send"] = [session, serializer](
                          std::uint32_t peer, const sol::object& data, sol::optional<sol::table> options) {
            bool reliable = isReliable(options);
            session->send(peer, reliable ? sReliableChannel : sUnreliableChannel, reliable,
                LuaUtil::serialize(data, serializer));
        };
        api["broadcast"] = [session, serializer](const sol::object& data, sol::optional<sol::table> options) {
            bool reliable = isReliable(options);
            session->broadcast(
                reliable ? sReliableChannel : sUnreliableChannel, reliable, LuaUtil::serialize(data, serializer));
        };
        api["receive"] = [session, serializer](sol::this_state s) {
            sol::state_view lua(s);
            sol::table result(lua, sol::create);
            int index = 1;
            for (Net::Event& event : session->drainEvents())
            {
                sol::table entry(lua, sol::create);
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
                            entry["data"] = LuaUtil::deserialize(lua, event.mData, serializer);
                        }
                        catch (const std::exception& e)
                        {
                            Log(Debug::Warning)
                                << "openmw.network: dropping malformed message from peer " << event.mPeer << ": "
                                << e.what();
                            continue;
                        }
                        break;
                    }
                }
                result[index++] = entry;
            }
            return result;
        };

        return LuaUtil::makeReadOnly(api);
    }

    sol::table initNetworkControlPackage(const Context& context)
    {
        sol::table api(context.sol(), sol::create);
        Net::Session* session = &context.mLuaManager->netSession();
        addControlFunctions(api, session);
        return LuaUtil::makeReadOnly(api);
    }
}
