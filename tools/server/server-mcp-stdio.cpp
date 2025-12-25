#include "server-mcp-stdio.h"
#include "log.h"
#include <cstdlib>
#include <cstring>
#include <chrono>

// Environment variables safe to inherit (matches MCP SDK)
#ifdef _WIN32
static const std::vector<std::string> INHERITED_ENV = {
    "APPDATA", "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA", "PATH",
    "PROCESSOR_ARCHITECTURE", "SYSTEMDRIVE", "SYSTEMROOT", "TEMP",
    "USERNAME", "USERPROFILE", "PROGRAMFILES"
};
#else
static const std::vector<std::string> INHERITED_ENV = {
    "HOME", "LOGNAME", "PATH", "SHELL", "TERM", "USER"
};
#endif

static std::vector<const char*> to_cstr_array(const std::vector<std::string> & strings) {
    std::vector<const char*> result;
    result.reserve(strings.size() + 1);
    for (const auto & s : strings) {
        result.push_back(s.c_str());
    }
    result.push_back(nullptr);
    return result;
}

std::shared_ptr<mcp_stdio_process> mcp_stdio_start(
    const mcp_server_config & config,
    std::weak_ptr<server_ws_connection> weak_conn)
{
    auto proc = std::make_shared<mcp_stdio_process>();
    proc->name = config.name;

    // Build command line
    std::vector<std::string> cmd_strings;
    cmd_strings.push_back(config.command);
    for (const auto & arg : config.args) {
        cmd_strings.push_back(arg);
    }
    auto argv = to_cstr_array(cmd_strings);

    // Build environment
    std::vector<std::string> env_strings;
    for (const auto & var : INHERITED_ENV) {
        const char * val = std::getenv(var.c_str());
        if (val) {
            env_strings.push_back(var + "=" + val);
        }
    }
    for (const auto & [key, value] : config.env) {
        std::string prefix = key + "=";
        env_strings.erase(
            std::remove_if(env_strings.begin(), env_strings.end(),
                [&prefix](const std::string & s) {
                    return s.compare(0, prefix.size(), prefix) == 0;
                }),
            env_strings.end());
        env_strings.push_back(key + "=" + value);
    }
    auto envp = to_cstr_array(env_strings);

    // Spawn subprocess
    int options = subprocess_option_no_window
                | subprocess_option_combined_stdout_stderr
                | subprocess_option_enable_async
                | subprocess_option_search_user_path;

    if (subprocess_create_ex(argv.data(), options, envp.data(), &proc->proc) != 0) {
        SRV_ERR("%s: failed to spawn: %s\n", __func__, config.name.c_str());
        return nullptr;
    }

    proc->stdin_file = subprocess_stdin(&proc->proc);
    if (!proc->stdin_file) {
        SRV_ERR("%s: failed to get stdin for: %s\n", __func__, config.name.c_str());
        return nullptr;
    }

    SRV_INF("%s: started MCP process: %s\n", __func__, config.name.c_str());

    // Start read thread - forwards stdout lines to WebSocket
    // Note: Must use subprocess_read_stdout() with subprocess_option_enable_async
    proc->read_thread = std::thread([proc, weak_conn]() {
        char buffer[65536];
        std::string line_buffer;

        while (!proc->should_stop) {
            // Use subprocess_read_stdout for async reading (required with subprocess_option_enable_async)
            unsigned n = subprocess_read_stdout(&proc->proc, buffer, sizeof(buffer) - 1);
            if (n == 0) {
                // No data available - check if process is still alive
                if (!subprocess_alive(&proc->proc)) break;
                // Brief sleep to avoid busy waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            buffer[n] = '\0';
            line_buffer += buffer;

            // Forward complete lines
            size_t pos;
            while ((pos = line_buffer.find('\n')) != std::string::npos) {
                std::string line = line_buffer.substr(0, pos);
                line_buffer.erase(0, pos + 1);

                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) continue;

                // Forward JSON lines to WebSocket
                if (line[0] == '{' || line[0] == '[') {
                    if (auto conn = weak_conn.lock()) {
                        conn->send(line);
                    }
                } else {
                    SRV_WRN("%s: stderr from %s: %s\n", __func__, proc->name.c_str(), line.c_str());
                }
            }
        }
    });

    return proc;
}

bool mcp_stdio_write(mcp_stdio_process * proc, const std::string & message) {
    if (!proc || !proc->stdin_file) return false;

    std::string line = message + "\n";
    size_t written = fwrite(line.c_str(), 1, line.size(), proc->stdin_file);
    fflush(proc->stdin_file);

    return written == line.size();
}
