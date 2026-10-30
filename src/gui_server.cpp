#include "bcnminer/gui_server.hpp"

#include "bcnminer/auto_config.hpp"
#include "bcnminer/json.hpp"
#include "bcnminer/pool_miner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
using socket_handle = SOCKET;
constexpr socket_handle invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_handle = int;
constexpr socket_handle invalid_socket = -1;
#endif

namespace bcnminer {
namespace {

constexpr std::size_t max_request_bytes = 64 * 1024;
constexpr std::size_t max_header_bytes = 16 * 1024;

volatile std::sig_atomic_t gui_exit_requested = 0;

void handle_gui_signal(const int) {
    gui_exit_requested = 1;
}

class SocketRuntime {
public:
    SocketRuntime() {
#if defined(_WIN32)
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Could not initialize Windows sockets.");
        }
#endif
    }

    ~SocketRuntime() {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};

void close_socket(const socket_handle socket) {
    if (socket == invalid_socket) {
        return;
    }
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

void configure_client_timeout(const socket_handle socket) {
#if defined(_WIN32)
    const DWORD timeout_ms = 5000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    const timeval timeout{5, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string trim_ascii(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

struct HttpRequest {
    std::string method;
    std::string target;
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
};

HttpRequest read_http_request(const socket_handle socket) {
    std::string data;
    std::array<char, 4096> buffer{};
    auto header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const auto amount = recv(
            socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (amount <= 0) {
            throw std::runtime_error("Incomplete HTTP request header.");
        }
        data.append(buffer.data(), static_cast<std::size_t>(amount));
        if (data.size() > max_header_bytes) {
            throw std::invalid_argument("HTTP request headers exceed the size limit.");
        }
        header_end = data.find("\r\n\r\n");
    }

    HttpRequest request;
    std::istringstream headers(data.substr(0, header_end));
    std::string line;
    if (!std::getline(headers, line)) {
        throw std::invalid_argument("HTTP request line is missing.");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::istringstream request_line(line);
    std::string version;
    if (!(request_line >> request.method >> request.target >> version) ||
        version != "HTTP/1.1") {
        throw std::invalid_argument("HTTP request line is invalid.");
    }

    std::size_t content_length = 0;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            throw std::invalid_argument("HTTP header is malformed.");
        }
        auto name = lowercase_ascii(trim_ascii(
            std::string_view(line).substr(0, separator)));
        auto value = trim_ascii(std::string_view(line).substr(separator + 1));
        if (name == "content-length") {
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), content_length);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size()) {
                throw std::invalid_argument("HTTP content length is invalid.");
            }
        }
        request.headers.insert_or_assign(std::move(name), std::move(value));
    }
    if (content_length > max_request_bytes ||
        header_end + 4 + content_length > max_request_bytes) {
        throw std::invalid_argument("HTTP request body exceeds the size limit.");
    }

    const auto body_start = header_end + 4;
    while (data.size() < body_start + content_length) {
        const auto amount = recv(
            socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (amount <= 0) {
            throw std::runtime_error("Incomplete HTTP request body.");
        }
        data.append(buffer.data(), static_cast<std::size_t>(amount));
        if (data.size() > max_request_bytes) {
            throw std::invalid_argument("HTTP request body exceeds the size limit.");
        }
    }
    request.body = data.substr(body_start, content_length);
    return request;
}

std::string header_value(const HttpRequest& request, const std::string_view name) {
    const auto it = request.headers.find(name);
    return it == request.headers.end() ? std::string{} : it->second;
}

bool send_all(const socket_handle socket, const std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto amount = send(
            socket,
            data.data() + offset,
            static_cast<int>(std::min<std::size_t>(
                data.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<int>::max()))),
            0);
        if (amount <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(amount);
    }
    return true;
}

void send_response(
    const socket_handle socket,
    const int status_code,
    const std::string_view content_type,
    const std::string_view body) {
    const auto reason = status_code == 200 ? "OK"
        : status_code == 202 ? "Accepted"
        : status_code == 400 ? "Bad Request"
        : status_code == 403 ? "Forbidden"
        : status_code == 404 ? "Not Found"
        : status_code == 409 ? "Conflict"
        : status_code == 413 ? "Payload Too Large"
        : status_code == 500 ? "Internal Server Error"
        : "Service Unavailable";
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << ' ' << reason << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\n"
             << "X-Content-Type-Options: nosniff\r\n"
             << "Referrer-Policy: no-referrer\r\n"
             << "Cross-Origin-Resource-Policy: same-origin\r\n"
             << "Content-Security-Policy: default-src 'self'; connect-src 'self' http: https:; "
                "style-src 'self'; script-src 'self'; img-src 'self' data:; "
                "base-uri 'none'; frame-ancestors 'none'\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    (void)send_all(socket, response.str());
}

void send_json(
    const socket_handle socket,
    const int status_code,
    const std::string& body) {
    send_response(socket, status_code, "application/json; charset=utf-8", body);
}

std::string json_bool(const bool value) {
    return value ? "true" : "false";
}

std::string json_number(const double value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    std::ostringstream result;
    result << std::setprecision(8) << value;
    return result.str();
}

std::string random_token() {
    std::random_device random;
    std::uniform_int_distribution<std::uint32_t> distribution;
    std::ostringstream token;
    token << std::hex << std::setfill('0');
    for (int part = 0; part < 4; ++part) {
        token << std::setw(8) << distribution(random);
    }
    return token.str();
}

struct GuiSettings {
    std::string pool = "pool.bytecoin.network:3333";
    std::string wallet;
    std::string password = "x";
    std::string threads = "auto";
    std::size_t nonce_offset = 39;
    bool tls = false;
    std::string backend = "cpu";
};

bool contains_line_break(const std::string_view value) {
    return value.find_first_of("\r\n") != std::string_view::npos;
}

bool parse_pool_endpoint(
    const std::string_view endpoint,
    std::string& host,
    std::string& port) {
    if (endpoint.empty() || contains_line_break(endpoint)) {
        return false;
    }
    std::string_view host_view;
    std::string_view port_view;
    if (endpoint.front() == '[') {
        const auto close = endpoint.find(']');
        if (close == std::string_view::npos ||
            close + 1 >= endpoint.size() ||
            endpoint[close + 1] != ':') {
            return false;
        }
        host_view = endpoint.substr(1, close - 1);
        port_view = endpoint.substr(close + 2);
    } else {
        const auto separator = endpoint.rfind(':');
        if (separator == std::string_view::npos || separator == 0) {
            return false;
        }
        host_view = endpoint.substr(0, separator);
        port_view = endpoint.substr(separator + 1);
        if (host_view.find(':') != std::string_view::npos) {
            return false;
        }
    }
    unsigned int numeric_port = 0;
    const auto parsed = std::from_chars(
        port_view.data(), port_view.data() + port_view.size(), numeric_port);
    if (host_view.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != port_view.data() + port_view.size() ||
        numeric_port == 0 || numeric_port > 65535) {
        return false;
    }
    host.assign(host_view);
    port.assign(port_view);
    return true;
}

std::string backend_name(const MiningBackend backend) {
    switch (backend) {
    case MiningBackend::cpu:
        return "cpu";
    case MiningBackend::gpu:
        return "gpu";
    case MiningBackend::both:
        return "both";
    }
    return "cpu";
}

GuiSettings load_saved_settings(const std::filesystem::path& path) {
    std::error_code file_error;
    if (!std::filesystem::exists(path, file_error)) {
        if (file_error) {
            throw std::runtime_error(
                "Could not inspect myconfig.txt: " + file_error.message());
        }
        return {};
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Could not open myconfig.txt.");
    }
    GuiSettings settings;
    bool has_pool = false;
    bool has_wallet = false;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("myconfig.txt contains a line without '='.");
        }
        const auto key = lowercase_ascii(trim_ascii(
            std::string_view(line).substr(0, separator)));
        const auto value = std::string(
            std::string_view(line).substr(separator + 1));
        if (key == "version") {
            if (trim_ascii(value) != "1") {
                throw std::runtime_error(
                    "myconfig.txt has an unsupported version.");
            }
        } else if (key == "pool") {
            settings.pool = trim_ascii(value);
            has_pool = true;
        } else if (key == "wallet") {
            settings.wallet = trim_ascii(value);
            has_wallet = true;
        } else if (key == "password") {
            settings.password = value;
        } else if (key == "threads") {
            settings.threads = lowercase_ascii(trim_ascii(value));
        } else if (key == "nonce_offset") {
            std::size_t offset = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), offset);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size()) {
                throw std::runtime_error(
                    "myconfig.txt has an invalid nonce_offset.");
            }
            settings.nonce_offset = offset;
        } else if (key == "tls") {
            const auto flag = lowercase_ascii(trim_ascii(value));
            if (flag == "true" || flag == "yes") {
                settings.tls = true;
            } else if (flag == "false" || flag == "no") {
                settings.tls = false;
            } else {
                throw std::runtime_error("myconfig.txt has an invalid tls value.");
            }
        } else if (key == "backend") {
            settings.backend = lowercase_ascii(trim_ascii(value));
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Failed while reading myconfig.txt.");
    }
    if (!has_pool || !has_wallet) {
        throw std::runtime_error(
            "myconfig.txt must contain pool and wallet entries.");
    }
    std::string host;
    std::string port;
    if (!parse_pool_endpoint(settings.pool, host, port)) {
        throw std::runtime_error("myconfig.txt contains an invalid pool endpoint.");
    }
    if (settings.wallet.empty() || settings.wallet.size() > 256 ||
        contains_line_break(settings.wallet) ||
        settings.password.size() > 256 ||
        contains_line_break(settings.password)) {
        throw std::runtime_error("myconfig.txt contains invalid account settings.");
    }
    if (settings.backend != "cpu" && settings.backend != "gpu" &&
        settings.backend != "both") {
        throw std::runtime_error("myconfig.txt contains an invalid backend.");
    }
    if (settings.nonce_offset > 1024 ||
        (settings.backend != "cpu" && settings.nonce_offset != 39)) {
        throw std::runtime_error(
            "myconfig.txt contains an invalid nonce offset.");
    }
    if (settings.threads != "auto") {
        std::size_t count = 0;
        const auto parsed = std::from_chars(
            settings.threads.data(),
            settings.threads.data() + settings.threads.size(),
            count);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != settings.threads.data() + settings.threads.size() ||
            count == 0 || count > 256) {
            throw std::runtime_error("myconfig.txt contains invalid CPU threads.");
        }
    }
    return settings;
}

void save_settings(
    const std::filesystem::path& path,
    const GuiSettings& settings) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not write myconfig.txt.");
    }
    output << "# BCN Miner configuration version 1\n"
           << "# Pool password is stored as plain text in this file.\n"
           << "version=1\n"
           << "pool=" << settings.pool << '\n'
           << "wallet=" << settings.wallet << '\n'
           << "password=" << settings.password << '\n'
           << "threads=" << settings.threads << '\n'
           << "nonce_offset=" << settings.nonce_offset << '\n'
           << "tls=" << (settings.tls ? "true" : "false") << '\n'
           << "backend=" << settings.backend << '\n';
    output.close();
    if (!output) {
        throw std::runtime_error("Failed while writing myconfig.txt.");
    }
}

GuiSettings parse_settings(const std::string_view body) {
    const auto json = parse_json(body);
    const auto string_field = [&](const std::string_view key) {
        const auto* value = json.get(key);
        if (value == nullptr || value->string() == nullptr) {
            throw std::invalid_argument(
                "Request is missing the '" + std::string(key) + "' field.");
        }
        return *value->string();
    };
    GuiSettings settings;
    settings.pool = string_field("pool");
    settings.wallet = string_field("wallet");
    if (const auto* password = json.get("password")) {
        if (password->string() == nullptr) {
            throw std::invalid_argument("Password must be text.");
        }
        settings.password = password->string_or("x");
    }
    settings.threads = lowercase_ascii(string_field("threads"));
    settings.backend = lowercase_ascii(string_field("backend"));
    if (const auto* tls = json.get("tls")) {
        const auto flag = tls->boolean();
        if (!flag.has_value()) {
            throw std::invalid_argument("TLS setting must be true or false.");
        }
        settings.tls = *flag;
    }
    if (const auto* nonce = json.get("nonceOffset")) {
        const auto number = nonce->number();
        if (!number.has_value() || *number < 0 ||
            *number > 1024 || std::floor(*number) != *number) {
            throw std::invalid_argument(
                "Nonce offset must be an integer from 0 to 1024.");
        }
        settings.nonce_offset = static_cast<std::size_t>(*number);
    }

    if (settings.wallet.empty() || settings.wallet.size() > 256 ||
        contains_line_break(settings.wallet)) {
        throw std::invalid_argument(
            "Wallet must be non-empty, at most 256 characters, and one line.");
    }
    if (settings.password.size() > 256 ||
        contains_line_break(settings.password)) {
        throw std::invalid_argument(
            "Pool password must be at most 256 characters on one line.");
    }
    std::string host;
    std::string port;
    if (!parse_pool_endpoint(settings.pool, host, port)) {
        throw std::invalid_argument(
            "Pool endpoint must be host:port or [IPv6]:port.");
    }
    if (settings.backend != "cpu" && settings.backend != "gpu" &&
        settings.backend != "both") {
        throw std::invalid_argument("Mining backend must be CPU, GPU, or both.");
    }
    if (settings.threads != "auto") {
        std::size_t count = 0;
        const auto parsed = std::from_chars(
            settings.threads.data(),
            settings.threads.data() + settings.threads.size(),
            count);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != settings.threads.data() + settings.threads.size() ||
            count == 0 || count > 256) {
            throw std::invalid_argument(
                "CPU threads must be auto or an integer from 1 to 256.");
        }
    }
    if (settings.backend != "cpu" && settings.nonce_offset != 39) {
        throw std::invalid_argument(
            "GPU mining currently requires nonce offset 39.");
    }
    return settings;
}

PoolMinerOptions make_pool_options(
    const GuiSettings& settings,
    const ProcessorTopology& topology) {
    PoolMinerOptions options;
    if (!parse_pool_endpoint(settings.pool, options.host, options.port)) {
        throw std::invalid_argument("Pool endpoint is invalid.");
    }
    options.wallet = settings.wallet;
    options.password = settings.password;
    options.nonce_offset = settings.nonce_offset;
    options.tls = settings.tls;
    options.gui_controlled = true;
    options.max_threads = topology.logical_processors;
    options.physical_cores = topology.physical_cores;
    options.cpu_ids = topology.preferred_cpu_ids;
    options.backend = settings.backend == "cpu"
        ? MiningBackend::cpu
        : settings.backend == "gpu" ? MiningBackend::gpu : MiningBackend::both;
    if (options.backend == MiningBackend::gpu) {
        options.threads = 0;
    } else if (settings.threads == "auto") {
        options.threads = 0;
    } else {
        const auto parsed = std::from_chars(
            settings.threads.data(),
            settings.threads.data() + settings.threads.size(),
            options.threads);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != settings.threads.data() + settings.threads.size() ||
            options.threads > topology.logical_processors) {
            throw std::invalid_argument(
                "CPU thread count exceeds the detected processor limit.");
        }
    }
    return options;
}

std::string settings_json(const GuiSettings& settings) {
    std::ostringstream json;
    json << "{\"pool\":" << json_escape(settings.pool)
         << ",\"wallet\":" << json_escape(settings.wallet)
         << ",\"password\":" << json_escape(settings.password)
         << ",\"threads\":" << json_escape(settings.threads)
         << ",\"nonceOffset\":" << settings.nonce_offset
         << ",\"tls\":" << json_bool(settings.tls)
         << ",\"backend\":" << json_escape(settings.backend) << '}';
    return json.str();
}

std::string status_json(
    const PoolMinerStatus& status,
    const bool running) {
    std::ostringstream json;
    json << "{\"running\":" << json_bool(running)
         << ",\"connectionStatus\":" << json_escape(status.connection_status)
         << ",\"pool\":" << json_escape(status.pool)
         << ",\"jobId\":" << json_escape(status.job_id)
         << ",\"event\":" << json_escape(status.event)
         << ",\"uptimeSeconds\":" << status.uptime_seconds
         << ",\"hashes\":" << status.hashes
         << ",\"difficulty\":" << status.difficulty
         << ",\"accepted\":" << status.accepted
         << ",\"ignored\":" << status.ignored
         << ",\"rejected\":" << status.rejected
         << ",\"pending\":" << status.pending
         << ",\"reconnections\":" << status.reconnections
         << ",\"responseMs\":" << status.response_ms
         << ",\"hashrate\":" << json_number(status.hash_rate)
         << ",\"cpuHashrate\":" << json_number(status.cpu_hash_rate)
         << ",\"gpuHashrate\":" << json_number(status.gpu_hash_rate)
         << ",\"threads\":" << status.threads
         << ",\"maxThreads\":" << status.max_threads
         << ",\"physicalCores\":" << status.physical_cores
         << ",\"mining\":" << json_bool(status.mining)
         << ",\"pauseAll\":" << json_bool(status.pause_all)
         << ",\"cpuPaused\":" << json_bool(status.cpu_paused)
         << ",\"gpuPaused\":" << json_bool(status.gpu_paused)
         << ",\"gpuBackendEnabled\":"
         << json_bool(status.gpu_backend_enabled)
         << ",\"paused\":"
         << json_bool(status.cpu_paused || status.gpu_paused)
         << ",\"gpus\":[";
    for (std::size_t index = 0; index < status.gpus.size(); ++index) {
        if (index != 0) {
            json << ',';
        }
        const auto& gpu = status.gpus[index];
        json << "{\"name\":" << json_escape(gpu.name)
             << ",\"vendor\":" << json_escape(gpu.vendor)
             << ",\"processor\":" << json_escape(gpu.processor)
             << ",\"pciAddress\":" << json_escape(gpu.pci_address)
             << ",\"hashrate\":" << json_number(gpu.hash_rate)
             << ",\"enabled\":" << json_bool(gpu.enabled)
             << ",\"paused\":" << json_bool(gpu.paused) << '}';
    }
    json << "]}";
    return json.str();
}

std::string devices_json() {
    const auto devices = detect_devices();
    const auto topology = detect_processor_topology();
    std::ostringstream json;
    json << "{\"logicalProcessors\":" << topology.logical_processors
         << ",\"physicalCores\":" << topology.physical_cores
         << ",\"gpus\":[";
    bool first = true;
    for (const auto& device : devices) {
        if (device.backend != Backend::opencl || !device.gpu.has_value()) {
            continue;
        }
        if (!first) {
            json << ',';
        }
        first = false;
        json << "{\"name\":" << json_escape(device.name)
             << ",\"vendor\":" << json_escape(device.gpu->vendor)
             << ",\"processor\":" << json_escape(device.gpu->processor)
             << ",\"detail\":" << json_escape(device.detail)
             << ",\"computeUnits\":" << device.gpu->compute_units
             << ",\"globalMemoryBytes\":"
             << device.gpu->global_memory_bytes << '}';
    }
    json << "]}";
    return json.str();
}

class DashboardServer {
public:
    DashboardServer(
        std::filesystem::path asset_directory,
        std::filesystem::path config_path)
        : asset_directory_(std::move(asset_directory)),
          config_path_(std::move(config_path)),
          token_(random_token()) {}

    ~DashboardServer() {
        stop_miner();
    }

    int run() {
        const auto listener = create_listener();
        if (listener == invalid_socket) {
            throw std::runtime_error(
                "Could not bind the local dashboard to 127.0.0.1.");
        }
        listener_ = listener;
        origin_ = "http://127.0.0.1:" + std::to_string(port_);
        const auto url = origin_ + '/';
        std::cout << "BCN Miner dashboard: " << url
                  << "\nThis local page controls the miner. Press Ctrl+C to exit.\n"
                  << std::flush;
        open_browser(url);

        while (!gui_exit_requested && !server_exit_requested_) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener_, &readable);
            timeval timeout{1, 0};
#if defined(_WIN32)
            const auto ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
            const auto ready = select(listener_ + 1, &readable, nullptr, nullptr, &timeout);
#endif
            if (ready == 0) {
                continue;
            }
            if (ready < 0) {
                continue;
            }
            sockaddr_in client_address{};
#if defined(_WIN32)
            int address_size = sizeof(client_address);
#else
            socklen_t address_size = sizeof(client_address);
#endif
            const auto client = accept(
                listener_,
                reinterpret_cast<sockaddr*>(&client_address),
                &address_size);
            if (client == invalid_socket) {
                continue;
            }
            configure_client_timeout(client);
            handle_client(client);
            close_socket(client);
        }

        stop_miner();
        close_socket(listener_);
        listener_ = invalid_socket;
        return 0;
    }

private:
    socket_handle create_listener() {
        const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == invalid_socket) {
            return invalid_socket;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(
                listener,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0 ||
            listen(listener, SOMAXCONN) != 0) {
            close_socket(listener);
            return invalid_socket;
        }
#if defined(_WIN32)
        int address_size = sizeof(address);
#else
        socklen_t address_size = sizeof(address);
#endif
        if (getsockname(
                listener,
                reinterpret_cast<sockaddr*>(&address),
                &address_size) != 0) {
            close_socket(listener);
            return invalid_socket;
        }
        port_ = ntohs(address.sin_port);
        return listener;
    }

    void open_browser(const std::string& url) const {
#if defined(_WIN32)
        const auto wide_url = std::filesystem::path(url).wstring();
        const auto result = reinterpret_cast<std::intptr_t>(
            ShellExecuteW(
                nullptr, L"open", wide_url.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL));
        if (result <= 32) {
            std::cerr << "Could not open a browser automatically. Navigate to "
                      << url << ".\n";
        }
#elif defined(__APPLE__)
        const auto child = fork();
        if (child == 0) {
            execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
#else
        const auto child = fork();
        if (child == 0) {
            execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
#endif
    }

    void handle_client(const socket_handle client) {
        try {
            const auto request = read_http_request(client);
            const auto query_start = request.target.find('?');
            const auto path = request.target.substr(0, query_start);
            if (request.method == "GET") {
                handle_get(client, path);
            } else if (request.method == "POST") {
                if (!authorized(request)) {
                    send_json(client, 403, "{\"error\":\"Local request authorization failed.\"}");
                    return;
                }
                handle_post(client, path, request.body);
            } else {
                send_json(client, 400, "{\"error\":\"Only GET and POST are supported.\"}");
            }
        } catch (const std::invalid_argument& error) {
            send_json(
                client,
                400,
                "{\"error\":" + json_escape(error.what()) + "}");
        } catch (const std::exception& error) {
            send_json(
                client,
                500,
                "{\"error\":" + json_escape(error.what()) + "}");
        }
    }

    bool authorized(const HttpRequest& request) const {
        return header_value(request, "origin") == origin_ &&
            header_value(request, "x-bcn-token") == token_;
    }

    void handle_get(const socket_handle client, const std::string& path) {
        if (path == "/api/bootstrap") {
            const auto settings = load_saved_settings(config_path_);
            send_json(
                client,
                200,
                "{\"token\":" + json_escape(token_) +
                    ",\"config\":" + settings_json(settings) + "}");
            return;
        }
        if (path == "/api/status") {
            std::scoped_lock lock(status_mutex_);
            send_json(client, 200, status_json(status_, miner_running_));
            return;
        }
        if (path == "/api/devices") {
            send_json(client, 200, devices_json());
            return;
        }
        const std::string_view asset_name = path == "/" ? "index.html"
            : path == "/styles.css" ? "styles.css"
            : path == "/app.js" ? "app.js"
            : path == "/favicon.svg" ? "favicon.svg"
            : path == "/favicon.ico" ? "favicon.ico"
            : "";
        if (asset_name.empty()) {
            send_json(client, 404, "{\"error\":\"Not found.\"}");
            return;
        }
        const auto asset_path = asset_directory_ / std::string(asset_name);
        std::ifstream input(asset_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "Dashboard asset is missing: " + asset_path.string());
        }
        const std::string body{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        const auto content_type = asset_name.ends_with(".css")
            ? "text/css; charset=utf-8"
            : asset_name.ends_with(".js")
                ? "text/javascript; charset=utf-8"
                : asset_name.ends_with(".svg")
                    ? "image/svg+xml"
                    : asset_name.ends_with(".ico")
                        ? "image/x-icon"
                        : "text/html; charset=utf-8";
        send_response(client, 200, content_type, body);
    }

    void handle_post(
        const socket_handle client,
        const std::string& path,
        const std::string& body) {
        if (path == "/api/start") {
            start_miner(client, body);
            return;
        }
        if (path == "/api/stop") {
            std::scoped_lock lock(status_mutex_);
            if (!miner_running_) {
                send_json(client, 409, "{\"error\":\"Miner is not running.\"}");
                return;
            }
            shutdown_requested_.store(true, std::memory_order_relaxed);
            status_.connection_status = "STOPPING";
            status_.event = "Stop requested from the dashboard.";
            send_json(client, 202, "{\"ok\":true}");
            return;
        }
        if (path == "/api/control") {
            handle_control(client, body);
            return;
        }
        if (path == "/api/quit") {
            server_exit_requested_ = true;
            shutdown_requested_.store(true, std::memory_order_relaxed);
            send_json(client, 202, "{\"ok\":true}");
            return;
        }
        send_json(client, 404, "{\"error\":\"Not found.\"}");
    }

    void start_miner(
        const socket_handle client,
        const std::string_view body) {
        const auto settings = parse_settings(body);
        const auto topology = detect_processor_topology();
        auto options = make_pool_options(settings, topology);
        bool already_running = false;
        {
            std::scoped_lock lock(status_mutex_);
            already_running = miner_running_;
        }
        if (already_running) {
            send_json(client, 409, "{\"error\":\"Miner is already running.\"}");
            return;
        }
        save_settings(config_path_, settings);

        if (miner_thread_.joinable()) {
            miner_thread_.join();
        }
        {
            std::scoped_lock lock(status_mutex_);
            miner_running_ = true;
            status_ = PoolMinerStatus{};
            status_.pool = settings.pool;
            status_.connection_status =
                settings.threads == "auto" &&
                    options.backend != MiningBackend::gpu
                ? "TUNING"
                : "CONNECTING";
            status_.event = status_.connection_status == "TUNING"
                ? "Measuring CPU configurations."
                : "Preparing the mining session.";
            status_.max_threads = topology.logical_processors;
            status_.physical_cores = topology.physical_cores;
        }
        shutdown_requested_.store(false, std::memory_order_relaxed);
        control_.pause_all.store(false, std::memory_order_relaxed);
        control_.pause_cpu.store(false, std::memory_order_relaxed);
        control_.clear_gpu_pauses();
        const bool auto_threads =
            settings.threads == "auto" &&
            options.backend != MiningBackend::gpu;
        miner_thread_ = std::jthread(
            [this, options = std::move(options), topology, auto_threads](
                const std::stop_token) mutable {
                try {
                    if (auto_threads) {
                        const auto safe_limit = std::max<std::size_t>(
                            1, (topology.logical_processors * 3) / 4);
                        const auto tuned = auto_tune_cpu(safe_limit);
                        options.threads = tuned.cpu_threads;
                        options.cpu_multi_hash = tuned.multi_hash;
                    }
                    {
                        std::scoped_lock lock(status_mutex_);
                        status_.connection_status = "CONNECTING";
                        status_.threads = options.threads;
                        status_.event = "Connecting to " +
                            options.host + ':' + options.port + '.';
                    }
                    (void)run_pool_miner(
                        options,
                        shutdown_requested_,
                        [this](const PoolMinerStatus& status) {
                            std::scoped_lock lock(status_mutex_);
                            status_ = status;
                            if (status.connection_status == "STOPPED") {
                                miner_running_ = false;
                            }
                        },
                        &control_);
                    std::scoped_lock lock(status_mutex_);
                    miner_running_ = false;
                    if (status_.connection_status != "ERROR") {
                        status_.connection_status = "STOPPED";
                    }
                } catch (const std::exception& error) {
                    std::scoped_lock lock(status_mutex_);
                    miner_running_ = false;
                    status_.connection_status = "ERROR";
                    status_.event = error.what();
                }
            });
        send_json(client, 202, "{\"ok\":true}");
    }

    void handle_control(
        const socket_handle client,
        const std::string_view body) {
        const auto json = parse_json(body);
        const auto* action_value = json.get("action");
        if (action_value == nullptr || action_value->string() == nullptr) {
            throw std::invalid_argument("Control action is required.");
        }
        const auto& action = *action_value->string();
        {
            std::scoped_lock lock(status_mutex_);
            if (!miner_running_) {
                send_json(client, 409, "{\"error\":\"Miner is not running.\"}");
                return;
            }
        }
        if (action == "pause-all") {
            control_.pause_all.store(true, std::memory_order_relaxed);
            control_.pause_cpu.store(false, std::memory_order_relaxed);
        } else if (action == "resume-all") {
            control_.pause_all.store(false, std::memory_order_relaxed);
            control_.pause_cpu.store(false, std::memory_order_relaxed);
        } else if (action == "pause-cpu") {
            control_.pause_cpu.store(true, std::memory_order_relaxed);
        } else if (action == "resume-cpu") {
            control_.pause_cpu.store(false, std::memory_order_relaxed);
        } else if (action == "pause-gpu" || action == "resume-gpu") {
            const auto* index_value = json.get("index");
            const auto index_number = index_value == nullptr
                ? std::nullopt
                : index_value->number();
            if (!index_number.has_value() || *index_number < 0 ||
                std::floor(*index_number) != *index_number ||
                *index_number >
                    static_cast<double>(
                        std::numeric_limits<std::uint32_t>::max())) {
                throw std::invalid_argument(
                    "GPU control requires a non-negative integer index.");
            }
            const auto gpu_index = static_cast<std::size_t>(*index_number);
            {
                std::scoped_lock lock(status_mutex_);
                if (gpu_index >= status_.gpus.size() ||
                    !status_.gpus[gpu_index].enabled) {
                    send_json(
                        client,
                        409,
                        "{\"error\":\"That GPU is not active in this mining session.\"}");
                    return;
                }
            }
            const auto paused = action == "pause-gpu";
            control_.set_gpu_paused(gpu_index, paused);
            send_json(
                client,
                200,
                "{\"ok\":true,\"paused\":" + json_bool(paused) + "}");
            return;
        } else {
            throw std::invalid_argument("Unknown miner control action.");
        }
        send_json(client, 200, "{\"ok\":true}");
    }

    void stop_miner() {
        shutdown_requested_.store(true, std::memory_order_relaxed);
        if (miner_thread_.joinable()) {
            miner_thread_.join();
        }
    }

    std::filesystem::path asset_directory_;
    std::filesystem::path config_path_;
    std::string token_;
    std::string origin_;
    std::uint16_t port_ = 0;
    socket_handle listener_ = invalid_socket;
    std::mutex status_mutex_;
    PoolMinerStatus status_;
    bool miner_running_ = false;
    bool server_exit_requested_ = false;
    std::jthread miner_thread_;
    std::atomic<bool> shutdown_requested_{false};
    PoolMinerControl control_;
};

class GuiSignalScope {
public:
    GuiSignalScope()
        : previous_interrupt_(std::signal(SIGINT, handle_gui_signal)),
          previous_terminate_(std::signal(SIGTERM, handle_gui_signal)) {
        gui_exit_requested = 0;
    }

    ~GuiSignalScope() {
        std::signal(SIGINT, previous_interrupt_);
        std::signal(SIGTERM, previous_terminate_);
    }

private:
    using SignalHandler = void (*)(int);
    SignalHandler previous_interrupt_;
    SignalHandler previous_terminate_;
};

}

int run_gui_server(const std::filesystem::path& executable_path) {
    SocketRuntime socket_runtime;
    std::error_code path_error;
    const auto absolute_executable =
        std::filesystem::absolute(executable_path, path_error);
    if (path_error) {
        throw std::runtime_error(
            "Could not resolve the miner executable path: " +
            path_error.message());
    }
    const auto executable_directory = absolute_executable.parent_path();
    const auto asset_directory = executable_directory / "web";
    if (!std::filesystem::is_regular_file(asset_directory / "index.html")) {
        throw std::runtime_error(
            "Web dashboard assets are missing beside the executable.");
    }

    GuiSignalScope signal_scope;
    DashboardServer server(
        asset_directory, executable_directory / "myconfig.txt");
    return server.run();
}
}
