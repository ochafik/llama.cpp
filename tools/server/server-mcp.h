#pragma once

#include "server-common.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Parsed URL components for MCP proxy
struct mcp_parsed_url {
    std::string scheme_host_port;  // e.g. "http://localhost:8080" or "https://api.example.com"
    std::string path;              // e.g. "/mcp"
    std::string error;             // Non-empty if parsing failed

    bool valid() const { return error.empty(); }

    // Parse URL into scheme_host_port and path
    // e.g. "http://localhost:8080/mcp" -> scheme_host_port="http://localhost:8080", path="/mcp"
    static mcp_parsed_url parse(const std::string & url) {
        mcp_parsed_url result;

        size_t protocol_pos = url.find("://");
        if (protocol_pos == std::string::npos) {
            result.error = "Invalid URL format (missing ://)";
            return result;
        }

        // Find path start (first / after ://)
        size_t host_start = protocol_pos + 3;
        size_t path_pos = url.find("/", host_start);

        if (path_pos != std::string::npos) {
            result.scheme_host_port = url.substr(0, path_pos);
            result.path = url.substr(path_pos);
        } else {
            result.scheme_host_port = url;
            result.path = "/";
        }

        return result;
    }
};

// MCP Server configuration (from JSON config file)
// Supports remote HTTP MCP servers (proxied with CORS support)
struct mcp_server_config {
    std::string name;
    std::string url;                  // URL of remote MCP server
    std::map<std::string, std::string> headers;  // Custom headers (e.g., Authorization)

    mcp_server_config() = default;
    mcp_server_config(const std::string & name, const json & j) : name(name) {
        // Parse remote HTTP server configuration
        if (j.contains("url")) url = j["url"].get<std::string>();
        if (j.contains("headers")) {
            const auto & headers_obj = j["headers"];
            if (headers_obj.is_object()) {
                for (auto it = headers_obj.begin(); it != headers_obj.end(); ++it) {
                    headers[it.key()] = it.value().get<std::string>();
                }
            }
        }
    }

    // Parse URL into components
    mcp_parsed_url parsed_url() const {
        return mcp_parsed_url::parse(url);
    }

    json to_json() const {
        json j;
        if (!url.empty()) j["url"] = url;
        if (!headers.empty()) j["headers"] = headers;
        return j;
    }
};

// MCP config file structure
// Expected JSON format:
// {
//   "mcpServers": {
//     "brave-search": {
//       "url": "http://127.0.0.1:38180/mcp"
//     },
//     "python": {
//       "url": "http://127.0.0.1:38181/mcp",
//       "headers": {
//         "Authorization": "Bearer YOUR_TOKEN"
//       }
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

// Thread-safe MCP config with auto-reload on file changes
// Takes config path as constructor argument and handles loading/reloading
class llama_mcp_config {
public:
    explicit llama_mcp_config(const std::string & config_path)
        : config_path_(config_path) {
        load();
    }

    // Get full MCP server config by name (with auto-reload if file changed)
    std::optional<mcp_server_config> get_server(const std::string & name) {
        std::lock_guard<std::mutex> lock(mutex_);
        check_reload();
        return mcp_config_.get_server(name);
    }

    // Get all available server names (with auto-reload if file changed)
    std::vector<std::string> get_available_servers() {
        std::lock_guard<std::mutex> lock(mutex_);
        check_reload();
        std::vector<std::string> servers;
        servers.reserve(mcp_config_.mcp_servers.size());
        for (const auto & [name, _] : mcp_config_.mcp_servers) {
            servers.push_back(name);
        }
        return servers;
    }

private:
    mutable std::mutex mutex_;
    mcp_config mcp_config_;
    std::string config_path_;
    std::filesystem::file_time_type last_modified_;

    void load() {
        auto config = mcp_config::from_file(config_path_);
        if (config) {
            mcp_config_ = std::move(*config);
            last_modified_ = get_file_mtime();
        } else {
            mcp_config_ = mcp_config{};
            last_modified_ = {};
        }
    }

    void check_reload() {
        auto current_mtime = get_file_mtime();
        if (current_mtime != last_modified_) {
            SRV_INF("%s: config file changed, reloading from: %s\n",
                    __func__, config_path_.c_str());
            load();
        }
    }

    std::filesystem::file_time_type get_file_mtime() const {
        try {
            return std::filesystem::last_write_time(config_path_);
        } catch (const std::filesystem::filesystem_error &) {
            return {};
        }
    }
};

