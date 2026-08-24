#include "config.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <boost/program_options.hpp>

namespace bpo = boost::program_options;

namespace MPServer
{
    namespace
    {
        std::string trim(std::string s)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        // key=value, '#' comments, repeated `content=` lines in load order. The
        // same shape openmw.cfg uses, so the list can be copied across without
        // being rewritten.
        void readFile(const std::filesystem::path& path, Config& out)
        {
            std::ifstream in(path);
            if (!in)
                return;
            std::string line;
            while (std::getline(in, line))
            {
                const auto hash = line.find('#');
                if (hash != std::string::npos)
                    line = line.substr(0, hash);
                const auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = trim(line.substr(0, eq));
                std::string value = trim(line.substr(eq + 1));
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                if (key == "port")
                    out.mPort = static_cast<std::uint16_t>(std::stoi(value));
                else if (key == "maxPeers")
                    out.mMaxPeers = static_cast<unsigned>(std::stoi(value));
                else if (key == "campaign")
                    out.mCampaign = value;
                else if (key == "data")
                    out.mDataDir = value;
                else if (key == "scripts")
                    out.mScriptDir = value;
                else if (key == "content")
                    out.mContent.push_back(value);
                else if (key == "password")
                    out.mPassword = value;
                else if (key == "timescale")
                    out.mTimescale = std::stod(value);
                else if (key == "tick")
                    out.mTickHz = static_cast<unsigned>(std::stoi(value));
                else if (key == "revive")
                    out.mRevive = !(value == "false" || value == "0" || value == "no");
            }
        }
    }

    bool Config::load(int argc, char* argv[], Config& out, std::string& error)
    {
        std::string configPath = "mpserver.cfg";
        bpo::options_description desc("Options");
        // clang-format off
        desc.add_options()
            ("help", "print this and exit")
            ("config", bpo::value<std::string>(), "configuration file (default mpserver.cfg)")
            ("port", bpo::value<int>(), "listening port")
            ("campaign", bpo::value<std::string>(), "campaign name (the file under <data>/mp-campaigns)")
            ("data", bpo::value<std::string>(), "directory holding mp-campaigns/")
            ("scripts", bpo::value<std::string>(), "the mp mod directory (its scripts/ is loaded)")
            ("content", bpo::value<std::vector<std::string>>()->composing(), "content file, in load order; repeatable")
            ("password", bpo::value<std::string>(), "join password")
            ("timescale", bpo::value<double>(), "game hours per real hour ratio")
            ("tick", bpo::value<int>(), "server ticks per second")
            ("revive", bpo::value<std::string>(), "death rule: true = downed+revive (default), false = classic temple death");
        // clang-format on

        bpo::variables_map vm;
        try
        {
            bpo::store(bpo::parse_command_line(argc, argv, desc), vm);
            bpo::notify(vm);
        }
        catch (const std::exception& e)
        {
            error = e.what();
            return false;
        }
        if (vm.count("help"))
        {
            std::ostringstream os;
            os << desc;
            error = os.str();
            return false;
        }
        if (vm.count("config"))
            configPath = vm["config"].as<std::string>();
        readFile(configPath, out);

        // The command line wins over the file, so a one-off run needs no edit.
        if (vm.count("port"))
            out.mPort = static_cast<std::uint16_t>(vm["port"].as<int>());
        if (vm.count("campaign"))
            out.mCampaign = vm["campaign"].as<std::string>();
        if (vm.count("data"))
            out.mDataDir = vm["data"].as<std::string>();
        if (vm.count("scripts"))
            out.mScriptDir = vm["scripts"].as<std::string>();
        if (vm.count("content"))
        {
            for (const std::string& c : vm["content"].as<std::vector<std::string>>())
                out.mContent.push_back(c);
        }
        if (vm.count("password"))
            out.mPassword = vm["password"].as<std::string>();
        if (vm.count("timescale"))
            out.mTimescale = vm["timescale"].as<double>();
        if (vm.count("tick"))
            out.mTickHz = static_cast<unsigned>(vm["tick"].as<int>());
        if (vm.count("revive"))
        {
            const std::string v = vm["revive"].as<std::string>();
            out.mRevive = !(v == "false" || v == "0" || v == "no");
        }

        if (out.mScriptDir.empty())
        {
            error = "no script directory: point --scripts at the mp mod (the folder holding scripts/mp)";
            return false;
        }
        if (out.mContent.empty())
        {
            // Refused rather than defaulted: the content list decides what every
            // formId in the campaign MEANS, and guessing it would corrupt a world
            // quietly instead of failing loudly.
            error = "no content list: every client's list must match exactly, so the server must be told it";
            return false;
        }
        if (out.mTickHz == 0 || out.mTickHz > 240)
        {
            error = "tick must be between 1 and 240";
            return false;
        }
        return true;
    }
}
