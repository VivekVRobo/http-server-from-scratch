#include "vhttp/router/router.hpp"
#include "vhttp/server/epoll_runtime.hpp"
#include "vhttp/server/thread_pool_runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

struct BenchmarkConfig {
    std::string runtime = "threadpool";
    std::uint16_t port = 8081;
    std::size_t workers = 4;
    std::size_t admission_capacity = 260;
    std::uint64_t duration_seconds = 30;
    std::size_t payload_bytes = 128;
    std::optional<std::string> stats_json;
    bool workers_explicit = false;
};

struct NormalizedStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::size_t active = 0;
    std::size_t peak_active = 0;
    std::optional<std::size_t> queued;
};

struct ResourceUsage {
    double cpu_seconds = 0.0;
    std::optional<std::uint64_t> peak_rss_kib;
};

std::uint64_t parse_positive(std::string_view text, std::string_view name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(std::string(text), &consumed, 10);
    if (consumed != text.size() || value == 0) {
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    }
    return value;
}

std::uint16_t parse_port(std::string_view text) {
    const auto value = parse_positive(text, "port");
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::out_of_range("port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(value);
}

std::size_t parse_size(std::string_view text, std::string_view name) {
    const auto value = parse_positive(text, name);
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::out_of_range(std::string(name) + " is too large for this platform");
    }
    return static_cast<std::size_t>(value);
}

std::string require_value(int argc, char** argv, int& index, std::string_view option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }
    ++index;
    return argv[index];
}

void print_usage(std::ostream& out) {
    out << "usage: vhttp_bench_server [options]\n"
        << "\n"
        << "Common options:\n"
        << "  --runtime <threadpool|epoll>  Runtime to benchmark (default: threadpool)\n"
        << "  --port <1-65535>              Loopback TCP port (default: 8081)\n"
        << "  --admission <count>           Total accepted in-flight connection budget (default: 260)\n"
        << "  --duration <seconds>          Fixed server lifetime (default: 30)\n"
        << "  --payload <bytes>             /bench payload, max 1 MiB (default: 128)\n"
        << "  --stats-json <path>           Write server/runtime/resource evidence as JSON\n"
        << "\n"
        << "Thread-pool option:\n"
        << "  --workers <count>             Fixed worker count (default: 4)\n"
        << "                                  admission must be greater than workers;\n"
        << "                                  pending queue = admission - workers\n"
        << "\n"
        << "The epoll runtime is Linux-only. --workers is rejected with --runtime epoll.\n";
}

BenchmarkConfig parse_args(int argc, char** argv) {
    BenchmarkConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--help" || option == "-h") {
            print_usage(std::cout);
            std::exit(0);
        }
        if (option == "--runtime") {
            config.runtime = require_value(argc, argv, index, option);
        } else if (option == "--port") {
            config.port = parse_port(require_value(argc, argv, index, option));
        } else if (option == "--workers") {
            config.workers = parse_size(require_value(argc, argv, index, option), "workers");
            config.workers_explicit = true;
        } else if (option == "--admission") {
            config.admission_capacity =
                parse_size(require_value(argc, argv, index, option), "admission capacity");
        } else if (option == "--duration") {
            config.duration_seconds =
                parse_positive(require_value(argc, argv, index, option), "duration seconds");
        } else if (option == "--payload") {
            config.payload_bytes = parse_size(require_value(argc, argv, index, option), "payload bytes");
        } else if (option == "--stats-json") {
            config.stats_json = require_value(argc, argv, index, option);
            if (config.stats_json->empty()) {
                throw std::invalid_argument("--stats-json path must not be empty");
            }
        } else {
            throw std::invalid_argument("unknown option: " + std::string(option));
        }
    }

    if (config.runtime != "threadpool" && config.runtime != "epoll") {
        throw std::invalid_argument("--runtime must be 'threadpool' or 'epoll'");
    }
    constexpr std::size_t max_benchmark_payload = 1024U * 1024U;
    if (config.payload_bytes > max_benchmark_payload) {
        throw std::out_of_range("payload bytes must be <= 1048576");
    }
    if (config.runtime == "threadpool") {
        if (config.admission_capacity <= config.workers) {
            throw std::invalid_argument(
                "threadpool admission capacity must be greater than workers so the bounded pending queue is non-zero");
        }
    } else {
        if (config.workers_explicit) {
            throw std::invalid_argument("--workers applies only to --runtime threadpool");
        }
        if (!vhttp::server::EpollRuntime::supported()) {
            throw std::runtime_error("--runtime epoll is supported only on Linux");
        }
    }
    return config;
}

std::string compiler_description() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return std::string("MSVC ") + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown";
#endif
}

std::string platform_description() {
#if defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

std::string git_revision() {
#ifdef VHTTP_GIT_REVISION
    return VHTTP_GIT_REVISION;
#else
    return "unknown";
#endif
}

std::string build_configuration() {
#ifdef VHTTP_BUILD_CONFIGURATION
    return VHTTP_BUILD_CONFIGURATION;
#else
    return "unknown";
#endif
}

std::string json_escape(std::string_view value) {
    std::ostringstream out;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20U) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(character) << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(character);
                }
        }
    }
    return out.str();
}

ResourceUsage capture_resource_usage(std::clock_t cpu_started) {
    ResourceUsage usage;
    const std::clock_t cpu_finished = std::clock();
    if (cpu_started != static_cast<std::clock_t>(-1) &&
        cpu_finished != static_cast<std::clock_t>(-1)) {
        usage.cpu_seconds = static_cast<double>(cpu_finished - cpu_started) /
                            static_cast<double>(CLOCKS_PER_SEC);
    }
#if defined(__linux__)
    rusage process_usage{};
    if (::getrusage(RUSAGE_SELF, &process_usage) == 0 && process_usage.ru_maxrss >= 0) {
        usage.peak_rss_kib = static_cast<std::uint64_t>(process_usage.ru_maxrss);
    }
#endif
    return usage;
}

void write_stats_json(const std::string& path,
                      const BenchmarkConfig& config,
                      const NormalizedStats& stats,
                      const ResourceUsage& resources) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to open stats JSON path: " + path);
    }

    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"runtime\": \"" << json_escape(config.runtime) << "\",\n"
           << "  \"git_revision\": \"" << json_escape(git_revision()) << "\",\n"
           << "  \"build_configuration\": \"" << json_escape(build_configuration()) << "\",\n"
           << "  \"compiler\": \"" << json_escape(compiler_description()) << "\",\n"
           << "  \"platform\": \"" << json_escape(platform_description()) << "\",\n"
           << "  \"configuration\": {\n"
           << "    \"port\": " << config.port << ",\n"
           << "    \"admission_capacity\": " << config.admission_capacity << ",\n"
           << "    \"duration_seconds\": " << config.duration_seconds << ",\n"
           << "    \"payload_bytes\": " << config.payload_bytes << ",\n";
    if (config.runtime == "threadpool") {
        output << "    \"workers\": " << config.workers << ",\n"
               << "    \"pending_queue\": " << (config.admission_capacity - config.workers) << "\n";
    } else {
        output << "    \"workers\": null,\n"
               << "    \"pending_queue\": null\n";
    }
    output << "  },\n"
           << "  \"runtime_stats\": {\n"
           << "    \"accepted\": " << stats.accepted << ",\n"
           << "    \"rejected\": " << stats.rejected << ",\n"
           << "    \"completed\": " << stats.completed << ",\n"
           << "    \"failed\": " << stats.failed << ",\n"
           << "    \"active\": " << stats.active << ",\n"
           << "    \"peak_active\": " << stats.peak_active << ",\n"
           << "    \"queued\": ";
    if (stats.queued.has_value()) {
        output << *stats.queued;
    } else {
        output << "null";
    }
    output << "\n  },\n"
           << "  \"resources\": {\n"
           << "    \"process_cpu_seconds\": " << std::fixed << std::setprecision(6)
           << resources.cpu_seconds << ",\n"
           << "    \"peak_rss_kib\": ";
    if (resources.peak_rss_kib.has_value()) {
        output << *resources.peak_rss_kib;
    } else {
        output << "null";
    }
    output << "\n  }\n}\n";

    if (!output) {
        throw std::runtime_error("failed while writing stats JSON path: " + path);
    }
}

template <class Runtime>
void run_for_duration(Runtime& runtime,
                      const BenchmarkConfig& config,
                      std::exception_ptr& server_failure) {
    std::thread server_thread([&] {
        try {
            runtime.run("127.0.0.1", config.port);
        } catch (...) {
            server_failure = std::current_exception();
        }
    });

    if (!runtime.wait_until_listening(std::chrono::seconds(5))) {
        runtime.request_stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (server_failure) {
            std::rethrow_exception(server_failure);
        }
        throw std::runtime_error("benchmark server did not begin listening within 5 seconds");
    }

    std::cout << "vhttp benchmark server\n"
              << "  runtime: " << config.runtime << '\n'
              << "  endpoint: http://127.0.0.1:" << runtime.bound_port() << "/bench\n"
              << "  admission capacity: " << config.admission_capacity << '\n';
    if (config.runtime == "threadpool") {
        std::cout << "  workers: " << config.workers << '\n'
                  << "  pending queue: " << (config.admission_capacity - config.workers) << '\n';
    }
    std::cout << "  payload bytes: " << config.payload_bytes << '\n'
              << "  duration seconds: " << config.duration_seconds << '\n'
              << "  git revision: " << git_revision() << '\n'
              << "  build: " << build_configuration() << '\n'
              << "  compiler: " << compiler_description() << '\n'
              << std::flush;

    std::this_thread::sleep_for(std::chrono::seconds(config.duration_seconds));
    runtime.request_stop();
    server_thread.join();

    if (server_failure) {
        std::rethrow_exception(server_failure);
    }
}

NormalizedStats run_threadpool(const BenchmarkConfig& config,
                               const vhttp::server::Handler& handler) {
    vhttp::server::ThreadPoolConfig pool_config;
    pool_config.worker_count = config.workers;
    pool_config.max_pending_connections = config.admission_capacity - config.workers;
    pool_config.listen_backlog = static_cast<int>(
        std::min<std::size_t>(config.admission_capacity, static_cast<std::size_t>(std::numeric_limits<int>::max())));

    vhttp::server::ThreadPoolRuntime runtime(handler, {}, pool_config);
    std::exception_ptr server_failure;
    run_for_duration(runtime, config, server_failure);

    const auto source = runtime.stats();
    return {source.accepted,
            source.rejected,
            source.completed,
            source.failed,
            source.active,
            source.peak_active,
            source.queued};
}

NormalizedStats run_epoll(const BenchmarkConfig& config,
                          const vhttp::server::Handler& handler) {
    vhttp::server::EpollConfig epoll_config;
    epoll_config.max_connections = config.admission_capacity;
    epoll_config.listen_backlog = static_cast<int>(
        std::min<std::size_t>(config.admission_capacity, static_cast<std::size_t>(std::numeric_limits<int>::max())));

    vhttp::server::EpollRuntime runtime(handler, {}, epoll_config);
    std::exception_ptr server_failure;
    run_for_duration(runtime, config, server_failure);

    const auto source = runtime.stats();
    return {source.accepted,
            source.rejected,
            source.completed,
            source.failed,
            source.active,
            source.peak_active,
            std::nullopt};
}

void print_final_stats(const NormalizedStats& stats, const ResourceUsage& resources) {
    std::cout << "final runtime stats\n"
              << "  accepted: " << stats.accepted << '\n'
              << "  rejected: " << stats.rejected << '\n'
              << "  completed: " << stats.completed << '\n'
              << "  failed: " << stats.failed << '\n'
              << "  active: " << stats.active << '\n'
              << "  peak active: " << stats.peak_active << '\n';
    if (stats.queued.has_value()) {
        std::cout << "  queued: " << *stats.queued << '\n';
    }
    std::cout << "resource evidence\n"
              << "  process CPU seconds: " << std::fixed << std::setprecision(6)
              << resources.cpu_seconds << '\n'
              << "  peak RSS KiB: ";
    if (resources.peak_rss_kib.has_value()) {
        std::cout << *resources.peak_rss_kib;
    } else {
        std::cout << "unavailable";
    }
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const BenchmarkConfig config = parse_args(argc, argv);
        const std::string payload(config.payload_bytes, 'x');

        vhttp::router::Router router;
        router.get("/health", [](const auto&) {
            return vhttp::http::HttpResponse::text(200, "OK", "healthy\n");
        });
        router.get("/bench", [&payload](const auto&) {
            return vhttp::http::HttpResponse::text(200, "OK", payload);
        });

        const vhttp::server::Handler handler =
            [&router](const auto& request) { return router.dispatch(request); };

        const std::clock_t cpu_started = std::clock();
        const NormalizedStats stats = config.runtime == "threadpool"
                                          ? run_threadpool(config, handler)
                                          : run_epoll(config, handler);
        const ResourceUsage resources = capture_resource_usage(cpu_started);

        print_final_stats(stats, resources);
        if (config.stats_json.has_value()) {
            write_stats_json(*config.stats_json, config, stats, resources);
            std::cout << "server stats JSON: " << *config.stats_json << '\n';
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        print_usage(std::cerr);
        return 1;
    }
}
