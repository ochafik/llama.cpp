#pragma once

#include "server-common.h"
#include <map>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <fstream>

// MCP Server configuration (from JSON config file)
struct mcp_server_config {
    std::string name;
    std::string command;              // Command to spawn
    std::vector<std::string> args;    // Command arguments
    std::map<std::string, std::string> env;  // Environment variables

    mcp_server_config() = default;
    mcp_server_config(const std::string & name, const json & j) : name(name) {
        if (j.contains("command")) command = j["command"].get<std::string>();
        if (j.contains("args")) {
            const auto & args_arr = j["args"];
            if (args_arr.is_array()) {
                for (const auto & arg : args_arr) {
                    args.push_back(arg.get<std::string>());
                }
            }
        }
        if (j.contains("env")) {
            const auto & env_obj = j["env"];
            if (env_obj.is_object()) {
                for (auto it = env_obj.begin(); it != env_obj.end(); ++it) {
                    env[it.key()] = it.value().get<std::string>();
                }
            }
        }
    }

    json to_json() const {
        json j;
        j["command"] = command;
        j["args"] = args;
        if (!env.empty()) j["env"] = env;
        return j;
    }
};

// MCP config file structure
// Expected JSON format:
// {
//   "mcpServers": {
//     "filesystem": {
//       "command": "npx",
//       "args": ["-y", "@modelcontextprotocol/server-filesystem", "/allowed/path"]
//     },
//     "brave-search": {
//       "command": "npx",
//       "args": ["-y", "@modelcontextprotocol/server-brave-search"],
//       "env": { "BRAVE_API_KEY": "..." }
//     }
//   }
// }

struct mcp_config {
    std::map<std::string, mcp_server_config> mcp_servers;

    mcp_config() = default;

    // Load from JSON file
    static std::optional<mcp_config> from_file(const std::string & path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            SRV_WRN("%s: failed to open MCP config file: %s\n", __func__, path.c_str());
            return std::nullopt;
        }

        try {
            json j;
            f >> j;

            mcp_config config;
            if (j.contains("mcpServers")) {
                const auto & servers = j["mcpServers"];
                if (servers.is_object()) {
                    for (auto it = servers.begin(); it != servers.end(); ++it) {
                        config.mcp_servers[it.key()] = mcp_server_config(it.key(), it.value());
                    }
                }
            }

            SRV_INF("%s: loaded %zu MCP server configurations from %s\n",
                    __func__, config.mcp_servers.size(), path.c_str());
            return config;
        } catch (const std::exception & e) {
            SRV_ERR("%s: failed to parse MCP config file: %s: %s\n",
                    __func__, path.c_str(), e.what());
            return std::nullopt;
        }
    }

    // Get server config by name
    std::optional<mcp_server_config> get_server(const std::string & name) const {
        auto it = mcp_servers.find(name);
        if (it != mcp_servers.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    json to_json() const {
        json j;
        j["mcpServers"] = json::object();
        for (const auto & [name, config] : mcp_servers) {
            j["mcpServers"][name] = config.to_json();
        }
        return j;
    }
};

