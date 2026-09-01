#ifndef COMPONENTS_LUA_JSONMIRROR_H
#define COMPONENTS_LUA_JSONMIRROR_H

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <sol/sol.hpp>
#include <yaml-cpp/yaml.h>

// A READABLE MIRROR of a Lua table, in JSON.
//
// The multiplayer campaign is stored with the engine's own codec: compact,
// atomic, and able to carry engine types without translating them. All of that
// is right for the file the server LOADS, and none of it helps a human trying to
// see why a quest is stuck, or to fix one corpse by hand. So the same data can be
// emitted a second way, for reading.
//
// yaml-cpp rather than a hand-rolled encoder because the engine ALREADY LINKS IT
// and YAML 1.2 is a superset of JSON: it emits JSON flow style, and -- the half
// that is genuinely hard to write by hand -- it PARSES JSON back, with real error
// reporting instead of a silent wrong answer.
//
// Header-only and shared deliberately: the client and the dedicated server have
// SEPARATE campaign bindings (mwlua/campaignbindings.cpp and
// apps/openmw-mp-server/shim.cpp), and the first version of this shipped to only
// one of them -- the server then reported "this build has no campaign.writeJson
// binding" while the client had it. One copy, included twice.
namespace LuaUtil::JsonMirror
{
    inline void write(const sol::object& value, YAML::Emitter& out, int depth = 0)
    {
        if (depth > 32)
            throw std::runtime_error("json mirror: table nested too deeply");
        switch (value.get_type())
        {
            case sol::type::nil:
            case sol::type::none:
                out << YAML::Null;
                return;
            case sol::type::boolean:
                out << value.as<bool>();
                return;
            case sol::type::number:
            {
                const double d = value.as<double>();
                // Integers as integers: a campaign full of "12.0" reads badly and
                // round-trips into a different Lua value than it started as.
                if (d == static_cast<double>(static_cast<std::int64_t>(d)) && std::abs(d) < 9e15)
                    out << static_cast<std::int64_t>(d);
                else
                    out << d;
                return;
            }
            case sol::type::string:
                out << value.as<std::string>();
                return;
            case sol::type::table:
            {
                const sol::table t = value.as<sol::table>();
                // A Lua table is both a list and a map; JSON is not. The rule: keys
                // exactly 1..n means an array, anything else is an object with
                // stringified keys. Keys are stringified rather than refused because
                // campaign tables are keyed by things like formIds and peer numbers.
                std::size_t arrayLen = 0;
                while (t.get<sol::object>(arrayLen + 1).valid())
                    ++arrayLen;
                std::size_t total = 0;
                for (const auto& kv : t)
                {
                    (void)kv;
                    ++total;
                }
                if (arrayLen == total && total > 0)
                {
                    out << YAML::BeginSeq;
                    for (std::size_t i = 1; i <= arrayLen; ++i)
                        write(t.get<sol::object>(i), out, depth + 1);
                    out << YAML::EndSeq;
                    return;
                }
                out << YAML::BeginMap;
                for (const auto& kv : t)
                {
                    const sol::object& k = kv.first;
                    if (k.get_type() == sol::type::string)
                        out << YAML::Key << k.as<std::string>();
                    else if (k.get_type() == sol::type::number)
                        out << YAML::Key << std::to_string(k.as<double>());
                    else
                        continue; // a key JSON cannot name; skipping beats corrupting
                    out << YAML::Value;
                    write(kv.second, out, depth + 1);
                }
                out << YAML::EndMap;
                return;
            }
            default:
                // Userdata and functions have no JSON form. The binary campaign
                // keeps them; this mirror says so rather than inventing one.
                out << "<unserializable>";
                return;
        }
    }

    inline sol::object read(const YAML::Node& node, sol::state_view& lua, int depth = 0)
    {
        if (depth > 32)
            throw std::runtime_error("json mirror: nested too deeply");
        switch (node.Type())
        {
            case YAML::NodeType::Null:
            case YAML::NodeType::Undefined:
                return sol::nil;
            case YAML::NodeType::Scalar:
            {
                bool b = false;
                if (YAML::convert<bool>::decode(node, b))
                    return sol::make_object(lua, b);
                double d = 0;
                if (YAML::convert<double>::decode(node, d))
                    return sol::make_object(lua, d);
                return sol::make_object(lua, node.Scalar());
            }
            case YAML::NodeType::Sequence:
            {
                sol::table t(lua, sol::create);
                int i = 1;
                for (const auto& child : node)
                    t[i++] = read(child, lua, depth + 1);
                return t;
            }
            case YAML::NodeType::Map:
            {
                sol::table t(lua, sol::create);
                for (const auto& kv : node)
                    t[kv.first.Scalar()] = read(kv.second, lua, depth + 1);
                return t;
            }
        }
        return sol::nil;
    }
}

#endif
