#include "server-context.h"
#include "server-http.h"
#include "server-models.h"
#include "server-ws.h"
#include "server-mcp-stdio.h"

#include "arg.h"
#include "common.h"
#include "llama.h"
#include "log.h"

#include <atomic>
#include <exception>
#include <signal.h>
#include <thread> // for std::thread::hardware_concurrency
#include <algorithm>

#include <cpp-httplib/httplib.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static std::function<void(int)> shutdown_handler;
static std::atomic_flag is_terminating = ATOMIC_FLAG_INIT;

static inline void signal_handler(int signal) {
    if (is_terminating.test_and_set()) {
        // in case it hangs, we can force terminate the server by hitting Ctrl+C twice
        // this is for better developer experience, we can remove when the server is stable enough
        fprintf(stderr, "Received second interrupt, terminating immediately.\n");
        exit(1);
    }

    shutdown_handler(signal);
}

// wrapper function that handles exceptions and logs errors
// this is to make sure handler_t never throws exceptions; instead, it returns an error response
static server_http_context::handler_t ex_wrapper(server_http_context::handler_t func) {
    return [func = std::move(func)](const server_http_req & req) -> server_http_res_ptr {
        std::string message;
        error_type error;
        try {
            return func(req);
        } catch (const std::invalid_argument & e) {
            // treat invalid_argument as invalid request (400)
            error = ERROR_TYPE_INVALID_REQUEST;
            message = e.what();
        } catch (const std::exception & e) {
            // treat other exceptions as server error (500)
            error = ERROR_TYPE_SERVER;
            message = e.what();
        } catch (...) {
            error = ERROR_TYPE_SERVER;
            message = "unknown error";
        }

        auto res = std::make_unique<server_http_res>();
        res->status = 500;
        try {
            json error_data = format_error_response(message, error);
            res->status = json_value(error_data, "code", 500);
            res->data = safe_json_to_str({{ "error", error_data }});
            SRV_WRN("got exception: %s\n", res->data.c_str());
        } catch (const std::exception & e) {
            SRV_ERR("got another exception: %s | while handling exception: %s\n", e.what(), message.c_str());
            res->data = "Internal Server Error";
        }
        return res;
    };
}

int main(int argc, char ** argv, char ** envp) {
    // own arguments required by this example
    common_params params;

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_SERVER)) {
        return 1;
    }

    // validate batch size for embeddings
    // embeddings require all tokens to be processed in a single ubatch
    // see https://github.com/ggml-org/llama.cpp/issues/12836
    if (params.embedding && params.n_batch > params.n_ubatch) {
        LOG_WRN("%s: embeddings enabled with n_batch (%d) > n_ubatch (%d)\n", __func__, params.n_batch, params.n_ubatch);
        LOG_WRN("%s: setting n_batch = n_ubatch = %d to avoid assertion failure\n", __func__, params.n_ubatch);
        params.n_batch = params.n_ubatch;
    }

    if (params.n_parallel < 0) {
        LOG_INF("%s: n_parallel is set to auto, using n_parallel = 4 and kv_unified = true\n", __func__);

        params.n_parallel = 4;
        params.kv_unified = true;
    }

    // for consistency between server router mode and single-model mode, we set the same model name as alias
    if (params.model_alias.empty() && !params.model.name.empty()) {
        params.model_alias = params.model.name;
    }

    common_init();

    // struct that contains llama context and inference
    server_context ctx_server;

    llama_backend_init();
    llama_numa_init(params.numa);

    LOG_INF("system info: n_threads = %d, n_threads_batch = %d, total_threads = %d\n", params.cpuparams.n_threads, params.cpuparams_batch.n_threads, std::thread::hardware_concurrency());
    LOG_INF("\n");
    LOG_INF("%s\n", common_params_get_system_info(params).c_str());
    LOG_INF("\n");

    server_http_context ctx_http;
    if (!ctx_http.init(params)) {
        LOG_ERR("%s: failed to initialize HTTP server\n", __func__);
        return 1;
    }

    //
    // WebSocket Server (for MCP stdio support) - only if --webui-mcp is enabled
    //

    server_ws_context * ctx_ws = nullptr;

    if (params.webui_mcp) {
        ctx_ws = new server_ws_context(params);
    }

    // Helper function to get MCP config path
    auto get_mcp_config_paths = [&params]() -> std::vector<std::string> {
        std::vector<std::string> paths;

        // First check if --mcp-config was provided
        if (!params.mcp_config.empty()) {
            paths.push_back(params.mcp_config);
            return paths;
        }

        // First check environment variable
        const char * env_path = std::getenv("LLAMA_MCP_CONFIG");
        if (env_path != nullptr) {
            paths.push_back(env_path);
            return paths;
        }

        // Try platform-specific config directory
        const char * home = std::getenv("HOME");
        if (home != nullptr) {
            paths.push_back(std::string(home) + "/.llama.cpp/mcp.json");
        }

#ifdef _WIN32
        // Windows: also try %APPDATA%
        const char * appdata = std::getenv("APPDATA");
        if (appdata != nullptr) {
            paths.push_back(std::string(appdata) + "/llama.cpp/mcp.json");
        }
#endif

        // Fallback to current directory
        paths.push_back("./mcp_config.json");
        paths.push_back("./config/mcp.json");

        return paths;
    };

    // Try to load MCP config from default locations (only if MCP is enabled)
    if (params.webui_mcp) {
        std::vector<std::string> config_paths = get_mcp_config_paths();
        for (const auto & path : config_paths) {
            if (ctx_http.load_mcp_config(path)) {
                LOG_INF("%s: loaded MCP config from: %s\n", __func__, path.c_str());
                break;
            }
        }
        LOG_INF("%s: MCP support enabled (HTTP proxy + WebSocket stdio)\n", __func__);
    }

    //
    // Router
    //

    // register API routes
    server_routes routes(params, ctx_server);

    bool is_router_server = params.model.path.empty();
    std::optional<server_models_routes> models_routes{};
    if (is_router_server) {
        // setup server instances manager
        try {
            models_routes.emplace(params, argc, argv, envp);
        } catch (const std::exception & e) {
            LOG_ERR("%s: failed to initialize router models: %s\n", __func__, e.what());
            return 1;
        }

        // proxy handlers
        // note: routes.get_health stays the same
        routes.get_metrics                 = models_routes->proxy_get;
        routes.post_props                  = models_routes->proxy_post;
        routes.get_api_show                = models_routes->proxy_get;
        routes.post_completions            = models_routes->proxy_post;
        routes.post_completions_oai        = models_routes->proxy_post;
        routes.post_chat_completions       = models_routes->proxy_post;
        routes.post_anthropic_messages     = models_routes->proxy_post;
        routes.post_anthropic_count_tokens = models_routes->proxy_post;
        routes.post_infill                 = models_routes->proxy_post;
        routes.post_embeddings             = models_routes->proxy_post;
        routes.post_embeddings_oai         = models_routes->proxy_post;
        routes.post_rerank                 = models_routes->proxy_post;
        routes.post_tokenize               = models_routes->proxy_post;
        routes.post_detokenize             = models_routes->proxy_post;
        routes.post_apply_template         = models_routes->proxy_post;
        routes.get_lora_adapters           = models_routes->proxy_get;
        routes.post_lora_adapters          = models_routes->proxy_post;
        routes.get_slots                   = models_routes->proxy_get;
        routes.post_slots                  = models_routes->proxy_post;

        // custom routes for router
        routes.get_props  = models_routes->get_router_props;
        routes.get_models = models_routes->get_router_models;
        ctx_http.post("/models/load",   ex_wrapper(models_routes->post_router_models_load));
        ctx_http.post("/models/unload", ex_wrapper(models_routes->post_router_models_unload));
    }

    ctx_http.get ("/health",              ex_wrapper(routes.get_health)); // public endpoint (no API key check)
    ctx_http.get ("/v1/health",           ex_wrapper(routes.get_health)); // public endpoint (no API key check)
    ctx_http.get ("/metrics",             ex_wrapper(routes.get_metrics));
    ctx_http.get ("/props",               ex_wrapper(routes.get_props));
    ctx_http.post("/props",               ex_wrapper(routes.post_props));
    ctx_http.post("/api/show",            ex_wrapper(routes.get_api_show));
    ctx_http.get ("/models",              ex_wrapper(routes.get_models)); // public endpoint (no API key check)
    ctx_http.get ("/v1/models",           ex_wrapper(routes.get_models)); // public endpoint (no API key check)
    ctx_http.get ("/api/tags",            ex_wrapper(routes.get_models)); // ollama specific endpoint. public endpoint (no API key check)
    ctx_http.post("/completion",          ex_wrapper(routes.post_completions)); // legacy
    ctx_http.post("/completions",         ex_wrapper(routes.post_completions));
    ctx_http.post("/v1/completions",      ex_wrapper(routes.post_completions_oai));
    ctx_http.post("/chat/completions",    ex_wrapper(routes.post_chat_completions));
    ctx_http.post("/v1/chat/completions", ex_wrapper(routes.post_chat_completions));
    ctx_http.post("/api/chat",            ex_wrapper(routes.post_chat_completions)); // ollama specific endpoint
    ctx_http.post("/v1/messages",         ex_wrapper(routes.post_anthropic_messages)); // anthropic messages API
    ctx_http.post("/v1/messages/count_tokens", ex_wrapper(routes.post_anthropic_count_tokens)); // anthropic token counting
    ctx_http.post("/infill",              ex_wrapper(routes.post_infill));
    ctx_http.post("/embedding",           ex_wrapper(routes.post_embeddings)); // legacy
    ctx_http.post("/embeddings",          ex_wrapper(routes.post_embeddings));
    ctx_http.post("/v1/embeddings",       ex_wrapper(routes.post_embeddings_oai));
    ctx_http.post("/rerank",              ex_wrapper(routes.post_rerank));
    ctx_http.post("/reranking",           ex_wrapper(routes.post_rerank));
    ctx_http.post("/v1/rerank",           ex_wrapper(routes.post_rerank));
    ctx_http.post("/v1/reranking",        ex_wrapper(routes.post_rerank));
    ctx_http.post("/tokenize",            ex_wrapper(routes.post_tokenize));
    ctx_http.post("/detokenize",          ex_wrapper(routes.post_detokenize));
    ctx_http.post("/apply-template",      ex_wrapper(routes.post_apply_template));
    // LoRA adapters hotswap
    ctx_http.get ("/lora-adapters",       ex_wrapper(routes.get_lora_adapters));
    ctx_http.post("/lora-adapters",       ex_wrapper(routes.post_lora_adapters));
    // Save & load slots
    ctx_http.get ("/slots",               ex_wrapper(routes.get_slots));
    ctx_http.post("/slots/:id_slot",      ex_wrapper(routes.post_slots));

    // MCP servers (only if --webui-mcp is enabled)
    if (params.webui_mcp) {
        // HTTP proxy for remote MCP servers with CORS support
        // Note: stdio servers use WebSocket (port + 1), not this HTTP endpoint
        auto proxy_mcp_handler = [&ctx_http](const server_http_req & req, const std::string & method) -> server_http_res_ptr {
            std::string server_name = req.get_param("server");
            if (server_name.empty()) {
                auto res = std::make_unique<server_http_res>();
                res->status = 400;
                res->data = json{{"error", "Missing server parameter"}}.dump();
                return res;
            }

            auto server_config = ctx_http.get_mcp_server(server_name);
            if (!server_config) {
                auto res = std::make_unique<server_http_res>();
                res->status = 404;
                res->data = json{{"error", "Server not found: " + server_name}}.dump();
                return res;
            }

            // Check if this is a stdio server (should use WebSocket instead)
            if (server_config->is_stdio()) {
                auto res = std::make_unique<server_http_res>();
                res->status = 400;
                res->data = json{{"error", "Server '" + server_name + "' is a stdio server. Use WebSocket (port + 1) instead."}}.dump();
                return res;
            }

            // Must be a remote HTTP server
            if (!server_config->is_remote()) {
                auto res = std::make_unique<server_http_res>();
                res->status = 400;
                res->data = json{{"error", "Server '" + server_name + "' has no url or command configured."}}.dump();
                return res;
            }

            auto url = server_config->parsed_url();
            if (!url.valid()) {
                auto res = std::make_unique<server_http_res>();
                res->status = 400;
                res->data = json{{"error", url.error}}.dump();
                return res;
            }

            SRV_INF("%s: Proxying to %s (server: %s)\n", __func__, server_config->url.c_str(), server_name.c_str());

            // Copy request headers, apply config headers (which take precedence)
            std::map<std::string, std::string> headers(req.headers);
            // Host/Content-Length: set automatically by httplib for target server
            // Connection: hop-by-hop header, not forwarded through proxies
            headers.erase("Host");
            headers.erase("Connection");
            headers.erase("Content-Length");
            for (const auto & [k, v] : server_config->headers) {
                headers[k] = v;
            }

            // Stream response from remote MCP server (supports HTTP and HTTPS)
            auto res = std::make_unique<server_http_proxy>(method, url.scheme_host_port, url.path, headers, req.body, req.should_stop);

            // Add CORS headers
            res->headers["Access-Control-Expose-Headers"] = "mcp-session-id";
            auto origin_it = req.headers.find("Origin");
            if (origin_it != req.headers.end()) {
                res->headers["Access-Control-Allow-Origin"] = origin_it->second;
            }

            return res;
        };

        ctx_http.get ("/mcp", ex_wrapper([&](const server_http_req & req) -> server_http_res_ptr {
            return proxy_mcp_handler(req, "GET");
        }));
        ctx_http.post("/mcp", ex_wrapper([&](const server_http_req & req) -> server_http_res_ptr {
            return proxy_mcp_handler(req, "POST");
        }));

        // List available MCP servers from config with their types
        ctx_http.get("/mcp/servers", ex_wrapper([&ctx_http](const server_http_req &) -> server_http_res_ptr {
            auto res = std::make_unique<server_http_res>();
            res->status = 200;
            json servers = json::array();
            for (const auto & name : ctx_http.get_mcp_server_names()) {
                auto config = ctx_http.get_mcp_server(name);
                std::string type = "unknown";
                if (config) {
                    if (config->is_stdio()) {
                        type = "stdio";
                    } else if (config->is_remote()) {
                        type = "http";
                    }
                }
                servers.push_back({{"name", name}, {"type", type}});
            }
            res->data = json{{"servers", servers}}.dump();
            return res;
        }));
    }

    //
    // Start the server
    //

    std::function<void()> clean_up;

    // Register WebSocket handlers for MCP stdio (only if --webui-mcp is enabled)
    if (params.webui_mcp && ctx_ws) {
        ctx_ws->on_open([&ctx_http](auto conn) {
            std::string server_name = conn->get_query_param("server");
            if (server_name.empty()) {
                conn->close(1008, "Missing 'server' query parameter");
                return;
            }
            auto config = ctx_http.get_mcp_server(server_name);
            if (!config || !config->is_stdio()) {
                conn->close(1008, "Unknown or non-stdio server: " + server_name);
                return;
            }
            // Start subprocess and attach to connection
            conn->user_data = mcp_stdio_start(*config, conn);
            if (!conn->user_data) {
                conn->close(1011, "Failed to start MCP process");
            }
        });

        ctx_ws->on_message([](auto conn, const std::string & msg) {
            auto * proc = static_cast<mcp_stdio_process*>(conn->user_data.get());
            if (proc) {
                mcp_stdio_write(proc, msg);
            }
        });

        ctx_ws->on_close([](auto conn) {
            conn->user_data.reset();  // Destructor kills subprocess
        });
    }

    if (is_router_server) {
        LOG_INF("%s: starting router server, no model will be loaded in this process\n", __func__);

        clean_up = [&models_routes, &ctx_ws]() {
            SRV_INF("%s: cleaning up before exit...\n", __func__);
            if (ctx_ws) {
                ctx_ws->stop();
                delete ctx_ws;
            }
            if (models_routes.has_value()) {
                models_routes->models.unload_all();
            }
            llama_backend_free();
        };

        if (!ctx_http.start()) {
            clean_up();
            LOG_ERR("%s: exiting due to HTTP server error\n", __func__);
            return 1;
        }
        ctx_http.is_ready.store(true);

        // Start WebSocket server - only if --webui-mcp is enabled
        if (params.webui_mcp && ctx_ws) {
            if (!ctx_ws->start()) {
                clean_up();
                LOG_ERR("%s: exiting due to WebSocket server error\n", __func__);
                return 1;
            }
            LOG_INF("%s: WebSocket server started on port %d\n", __func__, ctx_ws->get_actual_port());
        }

        shutdown_handler = [&](int) {
            if (ctx_ws) {
                ctx_ws->stop();
            }
            ctx_http.stop();
        };

    } else {
        // setup clean up function, to be called before exit
        clean_up = [&ctx_http, &ctx_ws, &ctx_server]() {
            SRV_INF("%s: cleaning up before exit...\n", __func__);
            if (ctx_ws) {
                ctx_ws->stop();
                delete ctx_ws;
            }
            ctx_http.stop();
            ctx_server.terminate();
            llama_backend_free();
        };

        // start the HTTP server before loading the model to be able to serve /health requests
        if (!ctx_http.start()) {
            clean_up();
            LOG_ERR("%s: exiting due to HTTP server error\n", __func__);
            return 1;
        }

        // Start WebSocket server - only if --webui-mcp is enabled
        if (params.webui_mcp && ctx_ws) {
            if (!ctx_ws->start()) {
                clean_up();
                LOG_ERR("%s: exiting due to WebSocket server error\n", __func__);
                return 1;
            }
            LOG_INF("%s: WebSocket server started on port %d\n", __func__, ctx_ws->get_actual_port());
        }

        // load the model
        LOG_INF("%s: loading model\n", __func__);

        if (!ctx_server.load_model(params)) {
            clean_up();
            if (ctx_http.thread.joinable()) {
                ctx_http.thread.join();
            }
            LOG_ERR("%s: exiting due to model loading error\n", __func__);
            return 1;
        }

        routes.update_meta(ctx_server);
        ctx_http.is_ready.store(true);

        LOG_INF("%s: model loaded\n", __func__);

        shutdown_handler = [&](int) {
            if (ctx_ws) {
                ctx_ws->stop();
            }
            ctx_server.terminate();
        };
    }

    // TODO: refactor in common/console
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
    struct sigaction sigint_action;
    sigint_action.sa_handler = signal_handler;
    sigemptyset (&sigint_action.sa_mask);
    sigint_action.sa_flags = 0;
    sigaction(SIGINT, &sigint_action, NULL);
    sigaction(SIGTERM, &sigint_action, NULL);
#elif defined (_WIN32)
    auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
        return (ctrl_type == CTRL_C_EVENT) ? (signal_handler(SIGINT), true) : false;
    };
    SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif

    if (is_router_server) {
        LOG_INF("%s: router server is listening on %s\n", __func__, ctx_http.listening_address.c_str());
        LOG_INF("%s: NOTE: router mode is experimental\n", __func__);
        LOG_INF("%s:       it is not recommended to use this mode in untrusted environments\n", __func__);
        if (ctx_http.thread.joinable()) {
            ctx_http.thread.join(); // keep the main thread alive
        }

        // when the HTTP server stops, clean up and exit
        clean_up();
    } else {
        LOG_INF("%s: server is listening on %s\n", __func__, ctx_http.listening_address.c_str());
        LOG_INF("%s: starting the main loop...\n", __func__);

        // optionally, notify router server that this instance is ready
        const char * router_port = std::getenv("LLAMA_SERVER_ROUTER_PORT");
        std::thread monitor_thread;
        if (router_port != nullptr) {
            monitor_thread = server_models::setup_child_server(shutdown_handler);
        }

        // this call blocks the main thread until queue_tasks.terminate() is called
        ctx_server.start_loop();

        clean_up();
        if (ctx_http.thread.joinable()) {
            ctx_http.thread.join();
        }
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }

        auto * ll_ctx = ctx_server.get_llama_context();
        if (ll_ctx != nullptr) {
            llama_memory_breakdown_print(ll_ctx);
        }
    }

    return 0;
}
