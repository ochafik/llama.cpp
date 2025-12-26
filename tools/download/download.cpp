// llama-download: Model download manager for llama.cpp
//
// Downloads models from HuggingFace, Docker Hub, or direct URLs to the llama.cpp cache.
// Supports queueing, resume, disk space checks, and wait-for-network mode.

#include "arg.h"
#include "common.h"
#include "download.h"
#include "log.h"
#include "llama.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

static std::atomic<bool> g_should_stop{false};

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__)) || defined(_WIN32)
static void signal_handler(int) {
    if (g_should_stop.load()) {
        // Second signal - exit immediately
        fprintf(stderr, "\nForce quit\n");
        std::exit(130);
    }
    g_should_stop.store(true);
    fprintf(stderr, "\nInterrupted, finishing current download... (press Ctrl+C again to force quit)\n");
}
#endif

// Queue item status
enum class download_status {
    pending,
    downloading,
    completed,
    failed,
    cancelled
};

static const char * status_to_string(download_status status) {
    switch (status) {
        case download_status::pending:     return "pending";
        case download_status::downloading: return "downloading";
        case download_status::completed:   return "completed";
        case download_status::failed:      return "failed";
        case download_status::cancelled:   return "cancelled";
    }
    return "unknown";
}

static download_status status_from_string(const std::string & s) {
    if (s == "pending")     return download_status::pending;
    if (s == "downloading") return download_status::downloading;
    if (s == "completed")   return download_status::completed;
    if (s == "failed")      return download_status::failed;
    if (s == "cancelled")   return download_status::cancelled;
    return download_status::pending;
}

// Queue item
struct download_item {
    std::string id;           // unique identifier
    std::string source;       // original source (HF repo, URL, or Docker)
    std::string source_type;  // "hf", "url", "docker"
    std::string url;          // resolved download URL
    std::string path;         // local file path
    download_status status = download_status::pending;
    int retries = 0;
    std::string last_error;
    std::string added_at;
    int64_t total_bytes = 0;
    int64_t downloaded_bytes = 0;
};

// Queue file management
struct download_queue {
    int version = 1;
    std::vector<download_item> items;
    std::string queue_path;
    std::mutex mutex;

    void load(const std::string & cache_dir) {
        queue_path = cache_dir + "/download-queue.json";
        std::lock_guard<std::mutex> lock(mutex);

        if (!fs::exists(queue_path)) {
            return;
        }

        try {
            std::ifstream f(queue_path);
            json j = json::parse(f);
            version = j.value("version", 1);
            for (const auto & item_json : j["downloads"]) {
                download_item item;
                item.id = item_json.value("id", "");
                item.source = item_json.value("source", "");
                item.source_type = item_json.value("source_type", "");
                item.url = item_json.value("url", "");
                item.path = item_json.value("path", "");
                item.status = status_from_string(item_json.value("status", "pending"));
                item.retries = item_json.value("retries", 0);
                item.last_error = item_json.value("last_error", "");
                item.added_at = item_json.value("added_at", "");
                item.total_bytes = item_json.value("total_bytes", 0);
                item.downloaded_bytes = item_json.value("downloaded_bytes", 0);

                // Reset downloading status to pending on load (crashed mid-download)
                if (item.status == download_status::downloading) {
                    item.status = download_status::pending;
                }
                items.push_back(item);
            }
        } catch (const std::exception & e) {
            LOG_ERR("Failed to load queue: %s\n", e.what());
        }
    }

    void save() {
        std::lock_guard<std::mutex> lock(mutex);
        json j;
        j["version"] = version;
        j["downloads"] = json::array();

        for (const auto & item : items) {
            json item_json;
            item_json["id"] = item.id;
            item_json["source"] = item.source;
            item_json["source_type"] = item.source_type;
            item_json["url"] = item.url;
            item_json["path"] = item.path;
            item_json["status"] = status_to_string(item.status);
            item_json["retries"] = item.retries;
            item_json["last_error"] = item.last_error;
            item_json["added_at"] = item.added_at;
            item_json["total_bytes"] = item.total_bytes;
            item_json["downloaded_bytes"] = item.downloaded_bytes;
            j["downloads"].push_back(item_json);
        }

        std::ofstream f(queue_path);
        f << j.dump(2);
    }

    std::string generate_id() {
        static const char alphanum[] = "0123456789abcdef";
        std::string id;
        for (int i = 0; i < 8; i++) {
            id += alphanum[rand() % (sizeof(alphanum) - 1)];
        }
        return id;
    }

    std::string add(const std::string & source, const std::string & source_type) {
        download_item item;
        item.id = generate_id();
        item.source = source;
        item.source_type = source_type;
        item.status = download_status::pending;

        // Get current timestamp
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
        item.added_at = ss.str();

        {
            std::lock_guard<std::mutex> lock(mutex);
            items.push_back(item);
        }
        save();
        return item.id;
    }

    void remove_completed() {
        std::lock_guard<std::mutex> lock(mutex);
        items.erase(
            std::remove_if(items.begin(), items.end(),
                [](const download_item & item) {
                    return item.status == download_status::completed;
                }),
            items.end());
    }

    download_item * find_by_id(const std::string & id) {
        for (auto & item : items) {
            if (item.id == id) {
                return &item;
            }
        }
        return nullptr;
    }
};

// Detect source type from string
static std::string detect_source_type(const std::string & source) {
    // URL detection
    if (source.find("://") != std::string::npos) {
        return "url";
    }
    // Docker format: [repo/]model[:tag] - typically no slashes or single slash without dots
    // HF format: user/model[:quant] - always has exactly one slash
    size_t slash_count = std::count(source.begin(), source.end(), '/');
    if (slash_count == 0) {
        // No slash - assume Docker (e.g., "smollm2:135M-Q4_0")
        return "docker";
    } else if (slash_count == 1 && source.find('.') == std::string::npos) {
        // One slash, no dots - could be HF (user/model) or Docker (repo/model)
        // Default to HF as it's more common
        return "hf";
    }
    // Multiple slashes or dots - likely HF
    return "hf";
}

// Get disk space info
struct disk_space_info {
    uint64_t available = 0;
    uint64_t capacity = 0;
    bool valid = false;
};

static disk_space_info get_disk_space(const std::string & path) {
    disk_space_info info;
    std::error_code ec;
    auto space = fs::space(path, ec);
    if (!ec) {
        info.available = space.available;
        info.capacity = space.capacity;
        info.valid = true;
    }
    return info;
}

static std::string format_size(uint64_t bytes) {
    const char * units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024 && unit < 4) {
        size /= 1024;
        unit++;
    }
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << size << " " << units[unit];
    return ss.str();
}

// List cached models and queue status
static void list_cache(const std::string & cache_dir, const download_queue & queue) {
    printf("Cache directory: %s\n\n", cache_dir.c_str());

    // Show disk space
    auto space = get_disk_space(cache_dir);
    if (space.valid) {
        printf("Disk space: %s available / %s total\n\n",
               format_size(space.available).c_str(),
               format_size(space.capacity).c_str());
    }

    // List cached files
    printf("Cached models:\n");
    int model_count = 0;
    try {
        for (const auto & entry : fs::directory_iterator(cache_dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                // Skip queue and metadata files
                if (filename.find(".json") != std::string::npos ||
                    filename.find(".etag") != std::string::npos ||
                    filename.find(".downloadInProgress") != std::string::npos) {
                    continue;
                }
                auto size = fs::file_size(entry.path());
                printf("  %s (%s)\n", filename.c_str(), format_size(size).c_str());
                model_count++;
            }
        }
    } catch (const std::exception & e) {
        printf("  (error reading directory: %s)\n", e.what());
    }
    if (model_count == 0) {
        printf("  (none)\n");
    }

    // Show queue status
    printf("\nDownload queue:\n");
    if (queue.items.empty()) {
        printf("  (empty)\n");
    } else {
        for (const auto & item : queue.items) {
            printf("  [%s] %s - %s", item.id.c_str(), item.source.c_str(), status_to_string(item.status));
            if (!item.last_error.empty()) {
                printf(" (%s)", item.last_error.c_str());
            }
            printf("\n");
        }
    }
}

// Resolve source to download URL and path
static bool resolve_source(download_item & item, const std::string & cache_dir, const std::string & hf_token, bool offline) {
    if (item.source_type == "hf") {
        // Validate format - must have a slash
        if (item.source.find('/') == std::string::npos) {
            item.last_error = "Invalid HF repo format, expected user/model[:quant]";
            return false;
        }

        try {
            // common_get_hf_file handles the tag parsing internally
            auto hf_file_info = common_get_hf_file(item.source, hf_token, offline);
            if (hf_file_info.ggufFile.empty()) {
                item.last_error = "No GGUF file found in repository";
                return false;
            }

            const char * hf_endpoint = getenv("MODEL_ENDPOINT");
            if (!hf_endpoint) hf_endpoint = getenv("HF_ENDPOINT");
            if (!hf_endpoint) hf_endpoint = "https://huggingface.co/";

            std::string endpoint(hf_endpoint);
            if (endpoint.back() != '/') endpoint += '/';

            item.url = endpoint + hf_file_info.repo + "/resolve/main/" + hf_file_info.ggufFile;

            // Generate cache filename
            std::string safe_repo = hf_file_info.repo;
            std::replace(safe_repo.begin(), safe_repo.end(), '/', '_');
            item.path = cache_dir + "/" + safe_repo + "_" + hf_file_info.ggufFile;

        } catch (const std::exception & e) {
            item.last_error = e.what();
            return false;
        }
    } else if (item.source_type == "docker") {
        try {
            // Docker resolution downloads and returns the local path
            auto local_path = common_docker_resolve_model(item.source);
            if (local_path.empty()) {
                item.last_error = "Failed to resolve Docker model";
                return false;
            }
            // Docker resolution returns the local path directly (already downloaded)
            item.path = local_path;
            item.url = ""; // Already downloaded by resolve
            item.status = download_status::completed;
        } catch (const std::exception & e) {
            item.last_error = e.what();
            return false;
        }
    } else if (item.source_type == "url") {
        item.url = item.source;
        // Extract filename from URL
        auto last_slash = item.url.rfind('/');
        std::string filename = (last_slash != std::string::npos)
            ? item.url.substr(last_slash + 1)
            : "model.gguf";
        // Remove query string
        auto query_pos = filename.find('?');
        if (query_pos != std::string::npos) {
            filename = filename.substr(0, query_pos);
        }
        item.path = cache_dir + "/" + filename;
    } else {
        item.last_error = "Unknown source type";
        return false;
    }

    return true;
}

// Download a single item with retry logic
static bool download_item_with_retry(download_item & item, const common_params & params, const std::string & cache_dir) {
    int max_retries = params.download_retry_max;
    int retry_delay = params.download_retry_delay;

    for (int attempt = 0; attempt <= max_retries || params.download_wait_net; attempt++) {
        if (g_should_stop.load()) {
            item.status = download_status::pending; // Will resume later
            return false;
        }

        // Check disk space before download
        auto space = get_disk_space(cache_dir);
        if (space.valid && space.available < static_cast<uint64_t>(params.download_min_space_mb) * 1024 * 1024) {
            LOG_WRN("Low disk space: %s available, minimum %lld MB required. Pausing...\n",
                    format_size(space.available).c_str(), (long long)params.download_min_space_mb);
            // Wait for space or interrupt
            while (!g_should_stop.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(30));
                space = get_disk_space(cache_dir);
                if (space.valid && space.available >= static_cast<uint64_t>(params.download_min_space_mb) * 1024 * 1024) {
                    break;
                }
            }
            if (g_should_stop.load()) {
                item.status = download_status::pending;
                return false;
            }
        }

        item.status = download_status::downloading;

        try {
            // Build a common_params_model for the download API
            common_params_model model;
            if (item.source_type == "hf") {
                model.hf_repo = item.source;
            } else if (item.source_type == "docker") {
                model.docker_repo = item.source;
            } else {
                model.url = item.url;
            }

            bool success = common_download_model(model, params.hf_token, params.offline);
            if (success) {
                // Update the path from the model struct (it gets set during download)
                if (!model.path.empty()) {
                    item.path = model.path;
                }
                item.status = download_status::completed;
                item.last_error.clear();
                return true;
            }
        } catch (const std::exception & e) {
            item.last_error = e.what();
            LOG_WRN("Download failed: %s\n", e.what());
        }

        item.retries = attempt + 1;

        if (attempt < max_retries || params.download_wait_net) {
            // Exponential backoff with cap
            int delay = std::min(retry_delay * (1 << std::min(attempt, 6)), 3600);
            LOG_INF("Retrying in %d seconds (attempt %d)...\n", delay, attempt + 2);

            for (int i = 0; i < delay && !g_should_stop.load(); i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    item.status = download_status::failed;
    return false;
}

static void print_usage(int, char ** argv) {
    printf("Usage: %s [options] [sources...]\n", argv[0]);
    printf("\n");
    printf("Download models to the llama.cpp cache directory.\n");
    printf("Sources can be HuggingFace repos (user/model:quant), Docker repos, or URLs.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s unsloth/phi-4-GGUF:q4_k_m              # Download from HuggingFace\n", argv[0]);
    printf("  %s -dr smollm2:135M-Q4_0                  # Download from Docker Hub\n", argv[0]);
    printf("  %s -hf repo1/model,repo2/model           # Multiple HF repos (comma-separated)\n", argv[0]);
    printf("  %s -f models.txt --wait-for-network      # Batch download, wait for net\n", argv[0]);
    printf("  %s --list                                # Show cache and queue status\n", argv[0]);
    printf("  %s --resume                              # Resume pending downloads\n", argv[0]);
    printf("  %s --update                              # Check cached models for updates\n", argv[0]);
    printf("  %s --dry-run -hf user/model              # Preview download without downloading\n", argv[0]);
    printf("\n");
}

// Check if --dry-run or -n is in arguments before full parsing
static bool has_dry_run_arg(int argc, char ** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0 || strcmp(argv[i], "-n") == 0) {
            return true;
        }
    }
    return false;
}

// Silent log callback for suppressing output during initialization
static void silent_log_callback(ggml_log_level, const char *, void *) {
    // Do nothing - suppress all output
}

int main(int argc, char ** argv) {
    common_params params;

    // Check for dry-run early to suppress backend initialization output
    bool is_dry_run = has_dry_run_arg(argc, argv);
    if (is_dry_run) {
        // Suppress ggml backend initialization output (Metal, CUDA, etc.)
        llama_log_set(silent_log_callback, nullptr);
    }

    // Set up signal handlers
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#elif defined(_WIN32)
    signal(SIGINT, signal_handler);
#endif

    // Parse arguments
    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_DOWNLOAD, print_usage)) {
        return 1;
    }

    // For dry-run, suppress INFO-level logging (only show warnings and errors)
    // For non-dry-run, restore normal logging and show build info
    if (params.download_dry_run) {
        // Restore logging but at reduced verbosity
        llama_log_set(common_log_default_callback, nullptr);
        common_log_set_verbosity_thold(LOG_LEVEL_WARN);
    } else {
        common_init();
    }

    // Get cache directory
    std::string cache_dir = fs::path(fs_get_cache_directory()).string();
    if (!fs::exists(cache_dir)) {
        fs::create_directories(cache_dir);
    }

    // Load queue
    download_queue queue;
    queue.load(cache_dir);

    // Handle --list
    if (params.download_list) {
        list_cache(cache_dir, queue);
        return 0;
    }

    // Handle --clear
    if (params.download_clear) {
        queue.remove_completed();
        queue.save();
        printf("Cleared completed downloads from queue.\n");
        return 0;
    }

    // Handle --cancel
    if (params.download_cancel) {
        auto * item = queue.find_by_id(params.download_cancel_id);
        if (item) {
            item->status = download_status::cancelled;
            queue.save();
            printf("Cancelled download: %s\n", item->source.c_str());
        } else {
            fprintf(stderr, "Download not found: %s\n", params.download_cancel_id.c_str());
            return 1;
        }
        return 0;
    }

    // Handle --update: check cached models for updates and add to queue
    if (params.download_update) {
        printf("Checking cached models for updates...\n");
        int added = 0;

        // 1. Docker models from manifests
        auto cached_models = common_list_cached_models();
        for (const auto & model : cached_models) {
            std::string source = model.to_string();
            // Check if already in queue
            bool exists = false;
            for (const auto & item : queue.items) {
                if (item.source == source) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                std::string id = queue.add(source, "docker");
                printf("  [docker] %s [%s]\n", source.c_str(), id.c_str());
                added++;
            }
        }

        // 2. HuggingFace models - scan for .etag files
        // Cache filename pattern: {user}_{repo}_{file}.gguf with slashes replaced by underscores
        // Challenge: repo names can contain underscores (e.g., "cerebras_GLM-4.5-GGUF")
        // Heuristic: Most GGUF repos end with "-GGUF", so look for that pattern
        std::set<std::string> seen_sources;
        try {
            for (const auto & entry : fs::directory_iterator(cache_dir)) {
                if (!entry.is_regular_file()) continue;
                std::string filename = entry.path().filename().string();

                // Look for .etag files
                if (filename.size() < 6 || filename.substr(filename.size() - 5) != ".etag") continue;

                // Get the model filename (without .etag)
                std::string model_file = filename.substr(0, filename.size() - 5);

                // Skip if not a GGUF file
                if (model_file.size() < 5 || model_file.substr(model_file.size() - 5) != ".gguf") continue;

                // Parse pattern: {user}_{repo}_{file}.gguf
                // First underscore separates user from rest
                auto first_underscore = model_file.find('_');
                if (first_underscore == std::string::npos) continue;

                std::string user = model_file.substr(0, first_underscore);
                std::string rest = model_file.substr(first_underscore + 1);

                // Look for "-GGUF_" or "_GGUF_" pattern to find end of repo name
                // This handles repos like "cerebras_GLM-4.5-Air-REAP-82B-A12B-GGUF"
                std::string repo;
                size_t gguf_pos = rest.find("-GGUF_");
                if (gguf_pos == std::string::npos) {
                    gguf_pos = rest.find("_GGUF_");
                }
                if (gguf_pos != std::string::npos) {
                    repo = rest.substr(0, gguf_pos + 5); // Include "-GGUF" or "_GGUF"
                } else {
                    // Fallback: use first underscore as separator
                    auto second_underscore = rest.find('_');
                    if (second_underscore == std::string::npos) continue;
                    repo = rest.substr(0, second_underscore);
                }

                // Reconstruct HF source: user/repo
                std::string source = user + "/" + repo;

                // Skip if already seen or in queue
                if (seen_sources.count(source)) continue;
                seen_sources.insert(source);

                bool exists = false;
                for (const auto & item : queue.items) {
                    if (item.source == source) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    std::string id = queue.add(source, "hf");
                    printf("  [hf] %s [%s]\n", source.c_str(), id.c_str());
                    added++;
                }
            }
        } catch (const std::exception & e) {
            LOG_WRN("Error scanning cache for HF models: %s\n", e.what());
        }

        if (added == 0) {
            printf("No models need updating (or all already in queue).\n");
        } else {
            printf("Added %d model(s) to queue for update check.\n", added);
        }
        // Fall through to process the queue
    }

    // Collect sources to download
    std::vector<std::pair<std::string, std::string>> sources; // (source, type)

    // From command line positional arguments (in download_sources)
    for (const auto & src : params.download_sources) {
        sources.push_back({src, detect_source_type(src)});
    }

    // From --hf-repo / --docker-repo / --model-url (reuse existing args)
    // Support comma-separated values for batch downloads
    if (!params.model.hf_repo.empty()) {
        for (const auto & repo : string_split<std::string>(params.model.hf_repo, ',')) {
            if (!repo.empty()) {
                sources.push_back({repo, "hf"});
            }
        }
    }
    if (!params.model.docker_repo.empty()) {
        for (const auto & repo : string_split<std::string>(params.model.docker_repo, ',')) {
            if (!repo.empty()) {
                sources.push_back({repo, "docker"});
            }
        }
    }
    if (!params.model.url.empty()) {
        for (const auto & url : string_split<std::string>(params.model.url, ',')) {
            if (!url.empty()) {
                sources.push_back({url, "url"});
            }
        }
    }

    // From input file
    if (!params.download_input_file.empty()) {
        std::ifstream f(params.download_input_file);
        if (!f) {
            fprintf(stderr, "Error: cannot open input file: %s\n", params.download_input_file.c_str());
            return 1;
        }
        std::string line;
        while (std::getline(f, line)) {
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') continue;
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            size_t end = line.find_last_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            line = line.substr(start, end - start + 1);
            sources.push_back({line, detect_source_type(line)});
        }
    }

    // Count pending items in queue
    size_t pending_count = 0;
    for (const auto & item : queue.items) {
        if (item.status == download_status::pending) {
            pending_count++;
        }
    }

    // Handle --resume: process queue without adding new sources
    if (params.download_resume) {
        if (pending_count == 0) {
            printf("No pending downloads in queue.\n");
            printf("Use --list to see cached models, or add new sources to download.\n");
            return 0;
        }
        printf("Resuming %zu pending download(s)...\n", pending_count);
        sources.clear(); // Don't add any new sources
    } else if (sources.empty() && !params.download_update) {
        // No sources specified and not --update - check queue
        if (pending_count == 0) {
            fprintf(stderr, "No models to download.\n");
            fprintf(stderr, "Use --help for usage, or --list to see cached models.\n");
            // Check if there are any completed items to hint at --update
            bool has_completed = false;
            for (const auto & item : queue.items) {
                if (item.status == download_status::completed) {
                    has_completed = true;
                    break;
                }
            }
            if (has_completed) {
                fprintf(stderr, "Tip: Use --update to check for model updates.\n");
            }
            return 1;
        }
        fprintf(stderr, "No sources specified, but queue has %zu pending download(s).\n", pending_count);
        fprintf(stderr, "Use --resume to continue, or add new sources to download.\n");
        return 1;
    } else if (sources.empty() && params.download_update && pending_count == 0) {
        // --update was used but no models to check
        printf("No models to update or download.\n");
        return 0;
    }

    if (!sources.empty()) {
        // Add new sources to queue
        for (const auto & [src, type] : sources) {
            // Check if already in queue
            bool exists = false;
            for (const auto & item : queue.items) {
                if (item.source == src) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                std::string id = queue.add(src, type);
                printf("Added to queue: %s [%s]\n", src.c_str(), id.c_str());
            } else {
                printf("Already in queue: %s\n", src.c_str());
            }
        }
    }

    // Dry-run or preflight: resolve sources and show what would be downloaded
    if (params.download_dry_run || params.download_preflight) {
        printf("\n%s\n", params.download_dry_run ? "Dry run - resolving sources..." : "Preflight check...");
        printf("Cache directory: %s\n\n", cache_dir.c_str());

        int resolve_count = 0;
        int resolve_errors = 0;

        // Track sizes for disk space calculation
        int64_t total_remote_size = 0;      // Total size to download
        int64_t total_partial_size = 0;     // Already partially downloaded
        int64_t total_cached_size = 0;      // Already fully cached
        int64_t total_update_overhead = 0;  // Extra space for updates (old + new)
        int unknown_sizes = 0;

        for (auto & item : queue.items) {
            if (item.status != download_status::pending) continue;

            // Resolve source to URL and path
            if (item.url.empty()) {
                if (!resolve_source(item, cache_dir, params.hf_token, params.offline)) {
                    if (params.download_dry_run) {
                        printf("[%s] %s\n", item.id.c_str(), item.source.c_str());
                        printf("  ERROR: %s\n\n", item.last_error.c_str());
                    }
                    resolve_errors++;
                    continue;
                }
            }

            resolve_count++;

            // Query remote file size
            int64_t remote_size = -1;
            if (!item.url.empty()) {
                remote_size = common_get_remote_file_size(item.url, params.hf_token);
                item.total_bytes = remote_size;
            }

            bool is_cached = fs::exists(item.path);
            bool has_partial = fs::exists(item.path + ".downloadInProgress");
            int64_t cached_size = is_cached ? static_cast<int64_t>(fs::file_size(item.path)) : 0;
            int64_t partial_size = has_partial ? static_cast<int64_t>(fs::file_size(item.path + ".downloadInProgress")) : 0;

            if (params.download_dry_run) {
                printf("[%s] %s\n", item.id.c_str(), item.source.c_str());
                printf("  Type: %s\n", item.source_type.c_str());
                if (!item.url.empty()) {
                    printf("  URL:  %s\n", item.url.c_str());
                }
                printf("  Path: %s\n", item.path.c_str());

                // Show remote size
                if (remote_size > 0) {
                    printf("  Size: %s\n", format_size(static_cast<uint64_t>(remote_size)).c_str());
                } else if (remote_size == 0) {
                    printf("  Size: 0 B (empty file)\n");
                } else {
                    printf("  Size: (unknown)\n");
                }

                // Check if file already exists
                if (is_cached) {
                    printf("  Status: Already cached (%s)", format_size(static_cast<uint64_t>(cached_size)).c_str());
                    // Check if it needs updating (size mismatch)
                    if (remote_size > 0 && cached_size != remote_size) {
                        printf(" -> will update");
                    }
                    printf("\n");
                } else if (has_partial) {
                    printf("  Status: Partial download (%s", format_size(static_cast<uint64_t>(partial_size)).c_str());
                    if (remote_size > 0) {
                        int pct = static_cast<int>((partial_size * 100) / remote_size);
                        printf(" / %s, %d%%", format_size(static_cast<uint64_t>(remote_size)).c_str(), pct);
                    }
                    printf(")\n");
                } else {
                    printf("  Status: Will download\n");
                }
                printf("\n");
            }

            // Calculate download requirements
            if (remote_size > 0) {
                if (is_cached) {
                    total_cached_size += cached_size;
                    // For updates, we need space for both old and new temporarily
                    if (cached_size != remote_size) {
                        total_update_overhead += cached_size;  // Old file stays until new is complete
                        total_remote_size += remote_size;
                    }
                } else if (has_partial) {
                    total_partial_size += partial_size;
                    total_remote_size += (remote_size - partial_size);  // Only need remaining
                } else {
                    total_remote_size += remote_size;
                }
            } else {
                unknown_sizes++;
            }
        }

        // Show disk space info with projections
        auto space = get_disk_space(cache_dir);
        printf("Disk space:\n");
        if (space.valid) {
            printf("  Available:        %s\n", format_size(space.available).c_str());
            printf("  Total capacity:   %s\n", format_size(space.capacity).c_str());
        }

        printf("\nDownload estimate:\n");
        if (total_remote_size > 0 || total_partial_size > 0) {
            printf("  To download:      %s\n", format_size(static_cast<uint64_t>(total_remote_size)).c_str());
            if (total_partial_size > 0) {
                printf("  Already partial:  %s (will resume)\n", format_size(static_cast<uint64_t>(total_partial_size)).c_str());
            }
            if (total_update_overhead > 0) {
                printf("  Update overhead:  %s (old files kept until new complete)\n",
                       format_size(static_cast<uint64_t>(total_update_overhead)).c_str());
            }
            if (unknown_sizes > 0) {
                printf("  Unknown sizes:    %d item(s) - cannot estimate\n", unknown_sizes);
            }

            // Calculate peak space needed and final space after completion
            int64_t peak_needed = total_remote_size + total_update_overhead;
            int64_t net_change = total_remote_size - total_update_overhead;  // Updates replace old with new

            if (space.valid) {
                int64_t space_after = static_cast<int64_t>(space.available) - net_change;
                int64_t space_during = static_cast<int64_t>(space.available) - peak_needed;

                printf("\n");
                printf("  Peak space needed:     %s (during updates)\n", format_size(static_cast<uint64_t>(peak_needed)).c_str());
                printf("  Space during download: %s\n",
                       space_during >= 0 ? format_size(static_cast<uint64_t>(space_during)).c_str() : "(insufficient!)");
                printf("  Space after download:  %s\n",
                       space_after >= 0 ? format_size(static_cast<uint64_t>(space_after)).c_str() : "(insufficient!)");

                if (space_during < static_cast<int64_t>(params.download_min_space_mb) * 1024 * 1024) {
                    fprintf(stderr, "\nWARNING: Insufficient disk space! Need %s peak, have %s.\n",
                            format_size(static_cast<uint64_t>(peak_needed)).c_str(),
                            format_size(space.available).c_str());
                }
            }
        } else if (total_cached_size > 0) {
            printf("  All items already cached (%s total)\n", format_size(static_cast<uint64_t>(total_cached_size)).c_str());
        } else {
            printf("  Nothing to download\n");
        }

        if (params.download_dry_run) {
            printf("\nSummary: %d item(s) to process", resolve_count);
            if (resolve_errors > 0) {
                printf(", %d error(s)", resolve_errors);
            }
            printf("\n");
            printf("Use without --dry-run to start downloading.\n");
            return resolve_errors > 0 ? 1 : 0;
        }

        // Preflight mode: prompt if low space
        if (space.valid) {
            int64_t peak_needed = total_remote_size + total_update_overhead;
            int64_t space_during = static_cast<int64_t>(space.available) - peak_needed;
            if (space_during < static_cast<int64_t>(params.download_min_space_mb) * 1024 * 1024) {
                fprintf(stderr, "Continue anyway? [y/N] ");
                char c = getchar();
                if (c != 'y' && c != 'Y') {
                    return 1;
                }
            }
        }
    }

    // Process queue
    printf("\nStarting downloads...\n\n");

    int completed = 0;
    int failed = 0;

    for (auto & item : queue.items) {
        if (g_should_stop.load()) break;

        if (item.status != download_status::pending) {
            if (item.status == download_status::completed) completed++;
            if (item.status == download_status::failed) failed++;
            continue;
        }

        printf("[%s] %s\n", item.id.c_str(), item.source.c_str());

        // Resolve source to URL and path
        if (item.url.empty() && item.status != download_status::completed) {
            if (!resolve_source(item, cache_dir, params.hf_token, params.offline)) {
                item.status = download_status::failed;
                printf("  Error: %s\n", item.last_error.c_str());
                failed++;
                queue.save();
                continue;
            }
            queue.save();
        }

        // Docker models are resolved directly
        if (item.status == download_status::completed) {
            printf("  Downloaded: %s\n", item.path.c_str());
            completed++;
            queue.save();
            continue;
        }

        // Download
        printf("  Downloading: %s\n", item.url.c_str());
        printf("  To: %s\n", item.path.c_str());

        if (download_item_with_retry(item, params, cache_dir)) {
            printf("  Completed!\n");
            completed++;
        } else if (item.status == download_status::failed) {
            printf("  Failed: %s\n", item.last_error.c_str());
            failed++;
        } else {
            printf("  Interrupted (will resume later)\n");
        }

        queue.save();
        printf("\n");
    }

    // Summary
    printf("Downloads: %d completed, %d failed\n", completed, failed);

    if (g_should_stop.load()) {
        printf("Interrupted. Run again to resume.\n");
    }

    return failed > 0 ? 1 : 0;
}
