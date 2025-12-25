#pragma once

#include "server-common.h"
#include "server-http.h"
#include <functional>
#include <string>
#include <memory>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

struct common_params;

// Forward declarations
struct sockaddr_in;
class ws_connection_impl;




// @ngxson: this is a demo for how a bi-directional connection between
// the server and frontend can be implemented using SSE + HTTP POST
// I'm reusing the name "WS" here, but this is not a real WebSocket implementation
// the code is 100% written by human, no AI involved
// but this is just a demo, do not use it in practice



struct server_ws_connection;

// hacky: server_ws_connection is a member of this struct because
// we want to have shared_ptr for other handler functions
// in practice, we don't really need this
struct server_ws_sse : server_http_res {
    std::string id;
    std::shared_ptr<server_ws_connection> conn;
    const server_http_req & req;

    std::mutex mutex_send;
    std::condition_variable cv;
    struct msg {
        std::string data;
        bool is_closed = false;
    };
    std::queue<msg> queue_send;

    server_ws_sse(const server_http_req & req, const std::string & id) : id(id), req(req) {
        conn = std::make_shared<server_ws_connection>(*this);

        queue_send.push({
            "data: {\"llamacpp_id\":\"" + id + "\"}", false
        });

        next = [this, &req, id](std::string & output) {
            std::unique_lock<std::mutex> lk(mutex_send);
            constexpr auto poll_interval = std::chrono::milliseconds(500);
            while (true) {
                if (!queue_send.empty()) {
                    output.clear();
                    auto & front = queue_send.front();
                    if (front.is_closed) {
                        return false; // closed
                    }
                    SRV_INF("%s: sending SSE message: %s\n", id.c_str(), front.data.c_str());
                    output = "data: " + front.data + "\n\n";
                    queue_send.pop();
                    return true;
                }
                if (req.should_stop()) {
                    return false; // connection closed
                }
                cv.wait_for(lk, poll_interval);
            }
        };
    }

    std::function<void()> on_close;
    ~server_ws_sse() {
        close();
        if (on_close) {
            on_close();
        }
    }

    void send(const std::string & message) {
        std::lock_guard<std::mutex> lk(mutex_send);
        queue_send.push({message, false});
        cv.notify_all();
    }

    void close() {
        std::lock_guard<std::mutex> lk(mutex_send);
        queue_send.push({"", true});
        cv.notify_all();
    }
};



struct server_ws_connection {
    server_ws_sse & parent;
    server_ws_connection(server_ws_sse & parent) : parent(parent) {}

    // Send a message to the client
    void send(const std::string & message) {
        parent.send(message);
    }

    // Close the connection
    void close(int code = 1000, const std::string & reason = "") {
        SRV_INF("%s: closing connection: code=%d, reason=%s\n",
                __func__, code, reason.c_str());
        parent.close();
    }

    // Get query parameter by key
    std::string get_query_param(const std::string & key) const {
        return parent.req.get_param(key);
    }

    // Get the remote address
    std::string get_remote_address() {
        return parent.id;
    }
};



// SSE + HTTP POST implementation of server_ws_context
struct server_ws_context {
    server_ws_context() = default;
    ~server_ws_context() = default;

    // map ID to connection
    std::mutex mutex;
    std::map<std::string, server_ws_sse *> res_map;

    // SSE endpoint
    server_http_context::handler_t get_mcp = [this](const server_http_req & req) {
        auto id = random_string();
        auto res = std::make_unique<server_ws_sse>(req, id);
        {
            std::unique_lock lock(mutex);
            res_map[id] = res.get();
        }
        SRV_INF("%s: new SSE connection established, ID: %s\n%s", __func__, id.c_str(), req.body.c_str());
        res->id = id;
        res->status = 200;
        res->headers["X-Connection-ID"] = id;
        res->content_type = "text/event-stream";
        // res->next is set in server_ws_sse constructor
        res->on_close = [this, id]() {
            std::unique_lock lock(mutex);
            handler_on_close(res_map[id]->conn);
            res_map.erase(id);
        };
        handler_on_open(res->conn);
        return res;
    };

    // HTTP POST endpoint
    server_http_context::handler_t post_mcp = [this](const server_http_req & req) {
        auto id = req.get_param("llamacpp_id");
        std::shared_ptr<server_ws_connection> conn;
        SRV_INF("%s: received POST for connection ID: %s\n%s", __func__, id.c_str(), req.body.c_str());
        std::unique_lock lock(mutex);
        {
            auto it = res_map.find(id);
            if (it != res_map.end()) {
                conn = it->second->conn;
            }
        }
        if (!conn) {
            SRV_ERR("%s: invalid connection ID: %s\n", __func__, id.c_str());
            auto res = std::make_unique<server_http_res>();
            res->status = 400;
            res->data = "Invalid connection ID";
            return res;
        }
        handler_on_message(conn, req.body);
        auto res = std::make_unique<server_http_res>();
        res->status = 200;
        return res;
    };

    // Called when new connection is established
    using on_open_t = std::function<void(std::shared_ptr<server_ws_connection>)>;
    void on_open(on_open_t handler) { handler_on_open = handler; }

    // Called when message is received from a connection
    using on_message_t = std::function<void(std::shared_ptr<server_ws_connection>, const std::string &)>;
    void on_message(on_message_t handler) { handler_on_message = handler; }

    // Called when connection is closed
    using on_close_t = std::function<void(std::shared_ptr<server_ws_connection>)>;
    void on_close(on_close_t handler) { handler_on_close = handler; }

    on_open_t handler_on_open;
    on_message_t handler_on_message;
    on_close_t handler_on_close;
};
