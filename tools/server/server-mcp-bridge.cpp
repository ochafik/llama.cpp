#include "server-mcp-bridge.h"
#include "log.h"
#include <cstdlib>
#include <filesystem>

// Security limits
static constexpr size_t MCP_MAX_LINE_BUFFER = 10 * 1024 * 1024;  // 10MB max line buffer

// Environment variables deemed safe to inherit for MCP subprocesses.
// Keep in sync with MCP TypeScript SDK:
// https://github.com/modelcontextprotocol/typescript-sdk/blob/main/packages/client/src/client/stdio.ts
#ifdef _WIN32
static const std::vector<std::string> MCP_INHERITED_ENV_VARS = {
    "APPDATA", "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA", "PATH",
    "PROCESSOR_ARCHITECTURE", "SYSTEMDRIVE", "SYSTEMROOT", "TEMP",
    "USERNAME", "USERPROFILE", "PROGRAMFILES"
};
#else
static const std::vector<std::string> MCP_INHERITED_ENV_VARS = {
    "HOME", "LOGNAME", "PATH", "SHELL", "TERM", "USER"
};
#endif

// Helper to convert string vectors to char* arrays for subprocess
static std::vector<const char*> to_cstr_array(const std::vector<std::string> & strings) {
    std::vector<const char*> result;
    result.reserve(strings.size() + 1);
    for (const auto & s : strings) {
        result.push_back(s.c_str());
    }
    result.push_back(nullptr);
    return result;
}

// mcp_subprocess implementation

server_mcp_bridge::mcp_subprocess::~mcp_subprocess() {
    should_stop = true;

    // Terminate process FIRST to unblock any pending reads
    if (proc && subprocess_alive(proc.get())) {
        subprocess_terminate(proc.get());
    }

    // Now safe to join - read will return 0 since process is dead
    if (read_thread.joinable()) {
        read_thread.join();
    }

    if (proc) {
        subprocess_destroy(proc.get());
    }
}

bool server_mcp_bridge::mcp_subprocess::start(const mcp_server_config & config) {
    name = config.name;

    // Build command line: command + args
    std::vector<std::string> cmd_strings;
    cmd_strings.push_back(config.command);
    for (const auto & arg : config.args) {
        cmd_strings.push_back(arg);
    }
    auto argv = to_cstr_array(cmd_strings);

    // Create subprocess with combined stdout/stderr and async reading
    int options = subprocess_option_no_window
                | subprocess_option_combined_stdout_stderr
                | subprocess_option_enable_async
                | subprocess_option_search_user_path;

    // Build safe environment: inherit only safe vars, then merge config env
    std::vector<std::string> env_strings;

    // Inherit only safe environment variables (matching MCP SDK behavior)
    for (const auto & var : MCP_INHERITED_ENV_VARS) {
        const char * val = std::getenv(var.c_str());
        if (val != nullptr) {
            env_strings.push_back(var + "=" + val);
        }
    }

    // Merge with config env vars (config overrides inherited)
    for (const auto & [key, value] : config.env) {
        // Remove any existing entry for this key
        std::string prefix = key + "=";
        env_strings.erase(
            std::remove_if(env_strings.begin(), env_strings.end(),
                [&prefix](const std::string & s) {
                    return s.compare(0, prefix.size(), prefix) == 0;
                }),
            env_strings.end());
        // Add the new value
        env_strings.push_back(key + "=" + value);
    }

    auto envp = to_cstr_array(env_strings);
    int result = subprocess_create_ex(argv.data(), options, envp.data(), proc.get());
    if (result != 0) {
        SRV_ERR("%s: failed to spawn MCP process: %s\n", __func__, name.c_str());
        return false;
    }

    stdin_file = subprocess_stdin(proc.get());
    if (!stdin_file) {
        SRV_ERR("%s: failed to get stdin for MCP process: %s\n", __func__, name.c_str());
        subprocess_terminate(proc.get());
        subprocess_destroy(proc.get());
        return false;
    }

    SRV_INF("%s: started MCP process: %s (cmd: %s)\n",
            __func__, name.c_str(), config.command.c_str());

    return true;
}

bool server_mcp_bridge::mcp_subprocess::write(const std::string & message) {
    if (!stdin_file) {
        return false;
    }

    std::string line = message + "\n";
    size_t written = fwrite(line.c_str(), 1, line.size(), stdin_file);
    fflush(stdin_file);

    if (written != line.size()) {
        SRV_ERR("%s: partial write to MCP process %s: %zu/%zu\n",
                __func__, name.c_str(), written, line.size());
        return false;
    }

    return true;
}

bool server_mcp_bridge::mcp_subprocess::is_running() const {
    return proc && subprocess_alive(proc.get());
}

// server_mcp_bridge implementation

server_mcp_bridge::server_mcp_bridge() {
    SRV_INF("%s: MCP bridge initialized\n", __func__);
}

server_mcp_bridge::~server_mcp_bridge() {
    std::unique_lock lock(mutex_);
    connections_.clear();
    SRV_INF("%s: MCP bridge destroyed\n", __func__);
}

bool server_mcp_bridge::load_config(const std::string & config_path) {
    auto config = mcp_config::from_file(config_path);
    if (!config) {
        SRV_WRN("%s: failed to load MCP config from: %s\n",
                __func__, config_path.c_str());
        // Use empty config - servers will be looked up dynamically
        mcp_config_ = mcp_config{};
        config_path_.clear();
        last_modified_ = {};
        return false;
    }

    mcp_config_ = std::move(*config);
    config_path_ = config_path;
    last_modified_ = get_file_mtime(config_path);
    SRV_INF("%s: loaded %zu MCP server configurations from: %s\n",
            __func__, mcp_config_.mcp_servers.size(), config_path.c_str());
    return true;
}

std::filesystem::file_time_type server_mcp_bridge::get_file_mtime(const std::string & path) const {
    try {
        return std::filesystem::last_write_time(path);
    } catch (const std::filesystem::filesystem_error &) {
        return {};
    }
}

void server_mcp_bridge::on_connection_opened(std::shared_ptr<server_ws_connection> conn) {
    std::string server_name = get_server_name(conn);

    if (server_name.empty()) {
        SRV_WRN("%s: WebSocket connection missing 'server' query parameter\n", __func__);
        conn->close(1008, "Missing 'server' query parameter");
        return;
    }

    SRV_INF("%s: WebSocket connection opened for MCP server: %s\n",
            __func__, server_name.c_str());

    auto state = std::make_unique<connection_state>();
    state->conn = conn;
    state->server_name = server_name;

    void * conn_ptr = conn.get();
    {
        std::unique_lock lock(mutex_);
        connections_[conn_ptr] = std::move(state);
    }
}

void server_mcp_bridge::on_connection_message(std::shared_ptr<server_ws_connection> conn,
                                              const std::string & message) {
    void * conn_ptr = conn.get();

    connection_state * state = nullptr;
    std::string server_name;

    // Look up connection state (brief lock)
    {
        std::unique_lock lock(mutex_);
        auto it = connections_.find(conn_ptr);
        if (it == connections_.end()) {
            SRV_WRN("%s: message from unknown connection\n", __func__);
            return;
        }
        state = it->second.get();
        server_name = state->server_name;
    }
    // Lock released - forward_to_mcp can take a long time (starting process)

    SRV_DBG("%s: message from %s: %s\n", __func__, server_name.c_str(), message.c_str());

    // Validate JSON and forward to MCP process
    try {
        (void)json::parse(message);  // Validate JSON syntax
        forward_to_mcp(state, message);
    } catch (const std::exception & e) {
        SRV_ERR("%s: failed to parse JSON-RPC message: %s\n",
                __func__, e.what());

        // Send JSON-RPC parse error response
        json error_resp;
        error_resp["jsonrpc"] = "2.0";
        error_resp["id"] = nullptr;
        error_resp["error"]["code"] = -32700;
        error_resp["error"]["message"] = "Parse error";
        conn->send(error_resp.dump());
    }
}

void server_mcp_bridge::on_connection_closed(std::shared_ptr<server_ws_connection> conn) {
    void * conn_ptr = conn.get();

    std::unique_ptr<connection_state> state_to_destroy;
    std::string server_name;

    // Extract connection from map (brief lock)
    {
        std::unique_lock lock(mutex_);
        auto it = connections_.find(conn_ptr);
        if (it != connections_.end()) {
            server_name = it->second->server_name;
            state_to_destroy = std::move(it->second);
            connections_.erase(it);
        }
    }
    // Lock released - subprocess destruction can take time (thread join)

    if (state_to_destroy) {
        SRV_INF("%s: WebSocket connection closed for MCP server: %s\n",
                __func__, server_name.c_str());
        // state_to_destroy goes out of scope here, destroying subprocess
    }
}

std::string server_mcp_bridge::get_server_name(std::shared_ptr<server_ws_connection> conn) {
    return conn->get_query_param("server");
}

std::optional<mcp_server_config> server_mcp_bridge::get_server_config(const std::string & name) {
    // First check loaded config
    auto config = mcp_config_.get_server(name);
    if (config) {
        return config;
    }

    // If not found, return nullopt
    // In the future, we could support dynamic server discovery
    return std::nullopt;
}

server_mcp_bridge::mcp_subprocess * server_mcp_bridge::get_or_create_process(connection_state * state) {
    if (state->process && state->process->is_running()) {
        SRV_INF("%s: reusing existing MCP process: %s\n",
                __func__, state->server_name.c_str());
        return state->process.get();
    }

    // Get server configuration
    auto config = get_server_config(state->server_name);
    if (!config) {
        SRV_ERR("%s: no configuration found for MCP server: %s\n",
                __func__, state->server_name.c_str());
        return nullptr;
    }

    // Create and start subprocess
    state->process = std::make_unique<mcp_subprocess>();

    SRV_INF("%s: starting MCP process: %s\n",
            __func__, state->server_name.c_str());

    if (!state->process->start(*config)) {
        SRV_ERR("%s: failed to start MCP process: %s\n",
                __func__, state->server_name.c_str());
        state->process.reset();
        return nullptr;
    }

    // Start read thread to forward stdout to WebSocket
    // Capture weak_ptr to connection to avoid preventing cleanup
    std::weak_ptr<server_ws_connection> weak_conn = state->conn;
    mcp_subprocess * proc = state->process.get();

    proc->read_thread = std::thread([proc, weak_conn]() {
        std::string buffer;
        buffer.resize(4096);
        std::string line_buffer;

        SRV_INF("%s: read thread started for %s\n", __func__, proc->name.c_str());

        while (!proc->should_stop) {
            unsigned bytes_read = subprocess_read_stdout(proc->proc.get(),
                                                         buffer.data(),
                                                         static_cast<unsigned>(buffer.size() - 1));
            if (bytes_read == 0) {
                if (!subprocess_alive(proc->proc.get())) {
                    SRV_INF("%s: MCP process %s exited\n", __func__, proc->name.c_str());
                    break;
                }
                // No data yet, brief sleep to avoid busy loop
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            buffer[bytes_read] = '\0';

            // Prevent unbounded buffer growth from malicious MCP servers
            if (line_buffer.size() + bytes_read > MCP_MAX_LINE_BUFFER) {
                SRV_ERR("%s: line buffer overflow from %s, dropping data\n",
                        __func__, proc->name.c_str());
                line_buffer.clear();
            }

            line_buffer += std::string(buffer.data(), bytes_read);

            // Process complete lines
            size_t pos;
            while ((pos = line_buffer.find('\n')) != std::string::npos) {
                std::string line = line_buffer.substr(0, pos);
                line_buffer.erase(0, pos + 1);

                // Trim \r if present
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.empty()) continue;

                // Check if this looks like JSON
                bool is_json = line[0] == '{' || line[0] == '[';

                if (is_json) {
                    SRV_INF("%s: JSON from %s: %.200s%s\n",
                            __func__, proc->name.c_str(), line.c_str(),
                            line.length() > 200 ? "..." : "");

                    // Forward to WebSocket if connection still alive
                    if (auto conn = weak_conn.lock()) {
                        conn->send(line);
                    }
                } else {
                    // Log non-JSON output (likely stderr)
                    SRV_WRN("%s: stderr from %s: %s\n",
                            __func__, proc->name.c_str(), line.c_str());
                }
            }
        }

        SRV_INF("%s: read thread ended for %s\n", __func__, proc->name.c_str());
    });

    SRV_INF("%s: successfully started MCP process: %s\n",
            __func__, state->server_name.c_str());

    return state->process.get();
}

void server_mcp_bridge::forward_to_mcp(connection_state * state, const std::string & message) {
    mcp_subprocess * proc = get_or_create_process(state);
    if (!proc) {
        SRV_ERR("%s: no MCP process available for: %s\n",
                __func__, state->server_name.c_str());

        // Send error response if this was a request (has id)
        try {
            json j = json::parse(message);
            if (j.contains("id")) {
                json error_resp;
                error_resp["jsonrpc"] = "2.0";
                error_resp["id"] = j["id"];
                error_resp["error"]["code"] = -32000;
                error_resp["error"]["message"] = "MCP process not available";
                if (state->conn) {
                    state->conn->send(error_resp.dump());
                }
            }
        } catch (...) {
            // Ignore parse errors here
        }
        return;
    }

    // Write to MCP process
    SRV_INF("%s: writing to %s: %s\n", __func__, state->server_name.c_str(), message.c_str());
    if (!proc->write(message)) {
        SRV_ERR("%s: failed to write to MCP process: %s\n",
                __func__, state->server_name.c_str());
    } else {
        SRV_INF("%s: successfully wrote to %s\n", __func__, state->server_name.c_str());
    }
}

void server_mcp_bridge::forward_to_ws(connection_state * state, const std::string & message) {
    if (!state->conn) {
        SRV_WRN("%s: no connection for %s, cannot forward message\n",
                __func__, state->server_name.c_str());
        return;
    }

    SRV_INF("%s: to %s: %s\n", __func__, state->server_name.c_str(), message.c_str());

    state->conn->send(message);
}


std::vector<std::string> server_mcp_bridge::get_available_servers() {
    // Check if config file has been modified and reload if needed
    if (!config_path_.empty()) {
        auto current_mtime = get_file_mtime(config_path_);
        if (current_mtime != last_modified_) {
            SRV_INF("%s: config file changed, reloading from: %s\n",
                    __func__, config_path_.c_str());
            load_config(config_path_);
        }
    }

    std::vector<std::string> servers;
    servers.reserve(mcp_config_.mcp_servers.size());

    for (const auto & [name, config] : mcp_config_.mcp_servers) {
        servers.push_back(name);
    }

    return servers;
}
