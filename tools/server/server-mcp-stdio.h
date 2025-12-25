#pragma once

#include "server-ws.h"
#include "server-mcp.h"
#include "vendor/sheredom/subprocess.h"
#include <thread>
#include <atomic>
#include <memory>

// Simple MCP stdio handler - one subprocess per WebSocket connection
// Attach to connection via: conn->user_data = mcp_stdio_start(config, conn)
// Subprocess is killed when user_data is destroyed (via shared_ptr destructor)

struct mcp_stdio_process {
    subprocess_s proc;
    FILE * stdin_file = nullptr;
    std::thread read_thread;
    std::atomic<bool> should_stop{false};
    std::string name;

    ~mcp_stdio_process() {
        should_stop = true;
        if (subprocess_alive(&proc)) {
            subprocess_terminate(&proc);
        }
        if (read_thread.joinable()) {
            read_thread.join();
        }
        subprocess_destroy(&proc);
    }
};

// Start MCP stdio process and attach to connection
// Returns nullptr on failure
std::shared_ptr<mcp_stdio_process> mcp_stdio_start(
    const mcp_server_config & config,
    std::weak_ptr<server_ws_connection> weak_conn);

// Write message to MCP process stdin
bool mcp_stdio_write(mcp_stdio_process * proc, const std::string & message);
