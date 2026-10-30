#include "bcnminer/pool_miner.hpp"

#include "bcnminer/auto_config.hpp"
#include "bcnminer/json.hpp"
#include "bcnminer/opencl_miner.hpp"
#include "bcnminer/pool_protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <deque>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <conio.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socket_handle = SOCKET;
constexpr socket_handle invalid_socket = INVALID_SOCKET;
#else
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
using socket_handle = int;
constexpr socket_handle invalid_socket = -1;
#endif

#if defined(BCNMINER_HAS_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace bcnminer {
namespace {

constexpr std::size_t max_line_size = 1024 * 1024;
constexpr std::uint64_t nonce_space = std::uint64_t{1} << 32;
constexpr std::uint64_t nonce_chunk_size = 32;
constexpr std::size_t max_queued_shares = 64;

std::string socket_error_message(const char* operation) {
#if defined(_WIN32)
    return std::string(operation) + " failed with socket error " +
        std::to_string(WSAGetLastError()) + ".";
#else
    return std::string(operation) + " failed.";
#endif
}

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

void configure_socket_timeouts(const socket_handle socket) {
#if defined(_WIN32)
    const DWORD timeout_ms = 15000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    const timeval timeout{15, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
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

class PoolConnection {
public:
    PoolConnection(const std::string& host, const std::string& port, const bool tls)
        : tls_(tls) {
        if (tls_ && !tls_supported()) {
            throw std::runtime_error(
                "TLS was requested, but this build does not include OpenSSL.");
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* addresses = nullptr;
        const auto lookup = getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
        if (lookup != 0) {
            throw std::runtime_error("Could not resolve pool address.");
        }

        std::string last_error = "Could not connect to pool.";
        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            const auto candidate = socket(
                address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate == invalid_socket) {
                last_error = socket_error_message("socket");
                continue;
            }
            configure_socket_timeouts(candidate);
            if (connect(candidate, address->ai_addr,
                        static_cast<int>(address->ai_addrlen)) == 0) {
                socket_ = candidate;
                break;
            }
            last_error = socket_error_message("connect");
            close_socket(candidate);
        }
        freeaddrinfo(addresses);
        if (socket_ == invalid_socket) {
            throw std::runtime_error(last_error);
        }

        if (tls_) {
            try {
                initialize_tls(host);
            } catch (...) {
                cleanup_tls();
                close_socket(socket_);
                socket_ = invalid_socket;
                throw;
            }
        }
    }

    PoolConnection(const PoolConnection&) = delete;
    PoolConnection& operator=(const PoolConnection&) = delete;

    ~PoolConnection() {
        cleanup_tls();
        close_socket(socket_);
    }

    void cleanup_tls() {
#if defined(BCNMINER_HAS_OPENSSL)
        if (ssl_ != nullptr) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (ssl_context_ != nullptr) {
            SSL_CTX_free(ssl_context_);
        }
#endif
    }

    void write_line(const std::string& line) {
        std::string wire = line;
        wire.push_back('\n');
        std::size_t written = 0;
        while (written < wire.size()) {
#if defined(BCNMINER_HAS_OPENSSL)
            if (tls_) {
                const auto amount = SSL_write(
                    ssl_, wire.data() + written,
                    static_cast<int>(std::min<std::size_t>(
                        wire.size() - written, std::numeric_limits<int>::max())));
                if (amount <= 0) {
                    throw_tls_error("TLS write");
                }
                written += static_cast<std::size_t>(amount);
                continue;
            }
#endif
            const auto amount = send(
                socket_, wire.data() + written,
                static_cast<int>(std::min<std::size_t>(
                    wire.size() - written, std::numeric_limits<int>::max())), 0);
            if (amount <= 0) {
                throw std::runtime_error(socket_error_message("send"));
            }
            written += static_cast<std::size_t>(amount);
        }
    }

    std::optional<std::string> read_line(const std::chrono::milliseconds timeout) {
        while (true) {
            const auto newline = read_buffer_.find('\n');
            if (newline != std::string::npos) {
                auto line = read_buffer_.substr(0, newline);
                read_buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return line;
            }
            if (read_buffer_.size() > max_line_size) {
                throw std::runtime_error("Pool response exceeded the 1 MiB line limit.");
            }
            if (!wait_readable(timeout)) {
                return std::nullopt;
            }

            std::array<char, 4096> buffer{};
            int amount = 0;
#if defined(BCNMINER_HAS_OPENSSL)
            if (tls_) {
                amount = SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()));
                if (amount <= 0) {
                    const auto error = SSL_get_error(ssl_, amount);
                    if (error == SSL_ERROR_ZERO_RETURN) {
                        throw std::runtime_error("Pool closed the TLS connection.");
                    }
                    throw_tls_error("TLS read");
                }
            } else
#endif
            {
                amount = recv(socket_, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (amount == 0) {
                    throw std::runtime_error("Pool closed the connection.");
                }
                if (amount < 0) {
                    throw std::runtime_error(socket_error_message("recv"));
                }
            }
            read_buffer_.append(buffer.data(), static_cast<std::size_t>(amount));
        }
    }

private:
    static bool tls_supported() {
#if defined(BCNMINER_HAS_OPENSSL)
        return true;
#else
        return false;
#endif
    }

#if defined(BCNMINER_HAS_OPENSSL)
    [[noreturn]] static void throw_tls_error(const char* operation) {
        const auto error = ERR_get_error();
        std::array<char, 256> message{};
        if (error != 0) {
            ERR_error_string_n(error, message.data(), message.size());
            throw std::runtime_error(std::string(operation) + " failed: " +
                                     message.data());
        }
        throw std::runtime_error(std::string(operation) + " failed.");
    }

    void initialize_tls(const std::string& host) {
        ssl_context_ = SSL_CTX_new(TLS_client_method());
        if (ssl_context_ == nullptr) {
            throw_tls_error("TLS context creation");
        }
        SSL_CTX_set_verify(ssl_context_, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(ssl_context_) != 1) {
            throw_tls_error("TLS CA certificate loading");
        }

        ssl_ = SSL_new(ssl_context_);
        if (ssl_ == nullptr) {
            throw_tls_error("TLS session creation");
        }
        if (SSL_set_tlsext_host_name(ssl_, host.c_str()) != 1 ||
            SSL_set1_host(ssl_, host.c_str()) != 1 ||
            SSL_set_fd(ssl_, static_cast<int>(socket_)) != 1) {
            throw_tls_error("TLS peer configuration");
        }
        if (SSL_connect(ssl_) != 1) {
            throw_tls_error("TLS handshake");
        }
        if (SSL_get_verify_result(ssl_) != X509_V_OK) {
            throw std::runtime_error("TLS certificate verification failed.");
        }
    }

    SSL_CTX* ssl_context_ = nullptr;
    SSL* ssl_ = nullptr;
#endif

    bool wait_readable(const std::chrono::milliseconds timeout) {
#if defined(BCNMINER_HAS_OPENSSL)
        if (tls_ && SSL_pending(ssl_) > 0) {
            return true;
        }
#endif
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket_, &read_set);
        const auto timeout_count = std::max<std::int64_t>(0, timeout.count());
        timeval interval{
            static_cast<long>(timeout_count / 1000),
            static_cast<long>((timeout_count % 1000) * 1000)
        };
#if defined(_WIN32)
        const auto ready = select(0, &read_set, nullptr, nullptr, &interval);
#else
        const auto ready = select(socket_ + 1, &read_set, nullptr, nullptr, &interval);
#endif
        if (ready == 0) {
            return false;
        }
        if (ready < 0) {
            throw std::runtime_error(socket_error_message("select"));
        }
        return true;
    }

    socket_handle socket_ = invalid_socket;
    bool tls_ = false;
    std::string read_buffer_;
};

std::string make_login_request(
    const PoolMinerOptions& options,
    const std::uint64_t request_id) {
    return "{\"id\":" + std::to_string(request_id) +
        ",\"jsonrpc\":\"2.0\",\"method\":\"login\",\"params\":{\"login\":" +
        json_escape(options.wallet) + ",\"pass\":" +
        json_escape(options.password) +
        ",\"agent\":\"bcnminer/0.1.0\",\"algo\":[\"cn/0\"]}}";
}

struct Share {
    std::string job_id;
    std::uint32_t nonce = 0;
    SlowHash hash{};
    bool gpu = false;
    std::size_t device_index = 0;
};

SlowHash target_hash(const std::uint64_t target) {
    SlowHash expanded{};
    for (std::size_t i = 0; i < sizeof(target); ++i) {
        expanded[24 + i] = static_cast<std::uint8_t>(target >> (8 * i));
    }
    return expanded;
}

class JobWorkers {
public:
    JobWorkers(
        const std::size_t thread_count,
        const std::size_t max_thread_count,
        std::vector<std::size_t> cpu_ids,
        std::shared_ptr<std::atomic<std::uint64_t>> nonce_allocator,
        const bool cpu_multi_hash,
        const bool enabled)
        : thread_count_(enabled ? thread_count : 0),
          max_thread_count_(enabled ? max_thread_count : 0),
          cpu_ids_(std::move(cpu_ids)),
          nonce_allocator_(std::move(nonce_allocator)),
          cpu_multi_hash_(cpu_multi_hash) {
        thread_hashes_.reserve(max_thread_count_);
        affinity_status_.reserve(max_thread_count_);
        for (std::size_t i = 0; i < max_thread_count_; ++i) {
            thread_hashes_.push_back(
                std::make_shared<std::atomic<std::uint64_t>>(0));
            affinity_status_.push_back(
                std::make_shared<std::atomic<int>>(-1));
        }
    }

    ~JobWorkers() {
        stop();
    }

    void start(PoolJob job) {
        stop_workers(true);
        {
            std::scoped_lock lock(shares_mutex_);
            shares_.clear();
        }
        current_job_ = std::make_shared<const PoolJob>(std::move(job));
        launch_workers();
    }

    void stop() {
        stop_workers(true);
        current_job_.reset();
    }

    void set_paused(const bool paused) {
        paused_.store(paused, std::memory_order_relaxed);
        pause_changed_.notify_all();
    }

    bool paused() const {
        return paused_.load(std::memory_order_relaxed);
    }

    bool has_job() const {
        return current_job_ != nullptr;
    }

    bool adjust_thread_count(const int adjustment) {
        if (max_thread_count_ == 0) {
            return false;
        }
        const auto current = static_cast<long long>(thread_count_);
        const auto desired = std::clamp(
            current + adjustment,
            1LL,
            static_cast<long long>(max_thread_count_));
        const auto new_count = static_cast<std::size_t>(desired);
        if (new_count == thread_count_) {
            return false;
        }
        thread_count_ = new_count;
        if (current_job_ != nullptr) {
            stop_workers(false);
            launch_workers();
        }
        return true;
    }

    void stop_workers(const bool cancel_current_job) {
        if (cancel_current_job) {
            stop_source_.request_stop();
        }
        pause_changed_.notify_all();
        shares_changed_.notify_all();
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        workers_.clear();
    }

    void launch_workers() {
        if (current_job_ == nullptr) {
            return;
        }
        stop_source_ = std::stop_source{};
        const auto shared_job = current_job_;
        const auto target = target_hash(shared_job->target);
        for (std::size_t i = 0; i < thread_count_; ++i) {
            const auto cpu_id = i < cpu_ids_.size() ? cpu_ids_[i] : i;
            affinity_status_[i]->store(-1, std::memory_order_relaxed);
            workers_.emplace_back(
                [this, shared_job, target, cpu_id, i](
                    const std::stop_token thread_stop) {
                    mine(
                        shared_job, target, thread_stop, cpu_id,
                        thread_hashes_[i], affinity_status_[i]);
                });
        }
    }

    std::optional<Share> take_share(
        const std::chrono::milliseconds wait_duration) {
        std::unique_lock lock(shares_mutex_);
        shares_changed_.wait_for(lock, wait_duration, [&] {
            return !shares_.empty();
        });
        if (shares_.empty()) {
            return std::nullopt;
        }
        auto share = std::move(shares_.front());
        shares_.pop_front();
        shares_changed_.notify_all();
        return share;
    }

    std::size_t thread_count() const {
        return thread_count_;
    }

    std::uint64_t hashes_computed() const {
        std::uint64_t total = 0;
        for (const auto& counter : thread_hashes_) {
            total += counter->load(std::memory_order_relaxed);
        }
        return total;
    }

    std::vector<std::uint64_t> per_thread_hashes() const {
        std::vector<std::uint64_t> values;
        values.reserve(thread_hashes_.size());
        for (const auto& counter : thread_hashes_) {
            values.push_back(counter->load(std::memory_order_relaxed));
        }
        return values;
    }

    std::vector<int> affinity_status() const {
        std::vector<int> values;
        values.reserve(affinity_status_.size());
        for (const auto& status : affinity_status_) {
            values.push_back(status->load(std::memory_order_relaxed));
        }
        return values;
    }

private:
    void mine(
        const std::shared_ptr<const PoolJob>& job,
        const SlowHash& target,
        const std::stop_token thread_stop,
        const std::size_t cpu_id,
        const std::shared_ptr<std::atomic<std::uint64_t>>& thread_hashes,
        const std::shared_ptr<std::atomic<int>>& affinity_status) {
        affinity_status->store(
            pin_current_thread_to_cpu(cpu_id) ? 1 : 0,
            std::memory_order_relaxed);
        const auto job_stop = stop_source_.get_token();
        while (!thread_stop.stop_requested() && !job_stop.stop_requested()) {
            if (paused()) {
                std::unique_lock lock(pause_mutex_);
                pause_changed_.wait(lock, [&] {
                    return !paused() || thread_stop.stop_requested() ||
                        job_stop.stop_requested();
                });
                continue;
            }
            const auto first_nonce = nonce_allocator_->fetch_add(
                nonce_chunk_size, std::memory_order_relaxed);
            if (first_nonce >= nonce_space) {
                return;
            }
            const auto count = std::min(nonce_chunk_size, nonce_space - first_nonce);
            const auto solution = scan_nonce_range(
                job->hashing_blob,
                job->nonce_offset,
                static_cast<std::uint32_t>(first_nonce),
                count,
                target,
                job_stop,
                thread_hashes.get(),
                cpu_multi_hash_);
            if (!solution.has_value()) {
                continue;
            }

            std::unique_lock lock(shares_mutex_);
            shares_changed_.wait(lock, job_stop, [&] {
                return shares_.size() < max_queued_shares ||
                    thread_stop.stop_requested();
            });
            if (job_stop.stop_requested() || thread_stop.stop_requested()) {
                return;
            }
            shares_.push_back(Share{
                job->job_id,
                solution->nonce,
                solution->hash
            });
            shares_changed_.notify_all();
        }
    }

    std::size_t thread_count_;
    const std::size_t max_thread_count_;
    const std::vector<std::size_t> cpu_ids_;
    std::shared_ptr<const PoolJob> current_job_;
    std::shared_ptr<std::atomic<std::uint64_t>> nonce_allocator_;
    const bool cpu_multi_hash_;
    std::vector<std::shared_ptr<std::atomic<std::uint64_t>>> thread_hashes_;
    std::vector<std::shared_ptr<std::atomic<int>>> affinity_status_;
    std::atomic<bool> paused_{false};
    std::stop_source stop_source_;
    std::vector<std::jthread> workers_;
    std::mutex shares_mutex_;
    std::condition_variable_any shares_changed_;
    std::deque<Share> shares_;
    std::mutex pause_mutex_;
    std::condition_variable pause_changed_;
};

class GpuWorkers {
public:
    GpuWorkers(
        const MiningBackend backend,
        std::shared_ptr<std::atomic<std::uint64_t>> nonce_allocator)
        : nonce_allocator_(std::move(nonce_allocator)) {
        if (backend == MiningBackend::cpu) {
            return;
        }

        const auto devices = list_opencl_gpu_devices();
        if (devices.empty()) {
            throw std::runtime_error(
                "GPU mining was requested, but no OpenCL GPU devices were "
                "detected. Install a supported GPU driver or use "
                "--backend cpu.");
        }
        engines_.resize(devices.size());
        for (std::size_t i = 0; i < devices.size(); ++i) {
            hashes_.push_back(
                std::make_shared<std::atomic<std::uint64_t>>(0));
            device_paused_.push_back(
                std::make_shared<std::atomic<bool>>(false));
        }
        std::cout << "Initializing and verifying OpenCL CryptoNight kernels...\n";
        std::string disabled_devices;
        std::size_t enabled_devices = 0;
        for (const auto& device : devices) {
            try {
                auto engine = std::make_unique<OpenClGpuMiner>(device);
                std::cout << "  GPU " << device.ordinal << ": " << device.name
                          << " (" << device.vendor << ") ready, batch "
                          << engine->intensity() << " hashes\n";
                engines_[device.ordinal] = std::move(engine);
                ++enabled_devices;
            } catch (const std::exception& error) {
                const auto message =
                    "GPU " + std::to_string(device.ordinal) + " (" +
                    device.name + ") disabled: " + error.what();
                std::cerr << message << '\n';
                if (!disabled_devices.empty()) {
                    disabled_devices += "\n";
                }
                disabled_devices += message;
            }
        }
        if (enabled_devices == 0) {
            throw std::runtime_error(
                "No detected OpenCL GPU passed CryptoNight startup "
                "verification. " + disabled_devices);
        }
    }

    GpuWorkers(const GpuWorkers&) = delete;
    GpuWorkers& operator=(const GpuWorkers&) = delete;

    ~GpuWorkers() {
        stop();
    }

    std::size_t device_count() const {
        return engines_.size();
    }

    std::size_t enabled_device_count() const {
        return static_cast<std::size_t>(std::count_if(
            engines_.begin(), engines_.end(),
            [](const auto& engine) { return engine != nullptr; }));
    }

    bool device_enabled(const std::size_t index) const {
        return index < engines_.size() && engines_[index] != nullptr;
    }

    bool has_job() const {
        return current_job_ != nullptr;
    }

    bool paused() const {
        return paused_.load(std::memory_order_relaxed);
    }

    void set_paused(const bool paused) {
        paused_.store(paused, std::memory_order_relaxed);
        pause_changed_.notify_all();
    }

    bool toggle_device(const std::size_t index) {
        if (index >= device_paused_.size() || !device_enabled(index)) {
            return false;
        }
        const auto was_paused =
            device_paused_[index]->load(std::memory_order_relaxed);
        device_paused_[index]->store(!was_paused, std::memory_order_relaxed);
        pause_changed_.notify_all();
        return !was_paused;
    }

    void set_device_paused(const std::size_t index, const bool paused) {
        if (index >= device_paused_.size() || !device_enabled(index)) {
            return;
        }
        device_paused_[index]->store(paused, std::memory_order_relaxed);
        pause_changed_.notify_all();
    }

    bool device_paused(const std::size_t index) const {
        return index < device_paused_.size() &&
            device_paused_[index]->load(std::memory_order_relaxed);
    }

    void start(PoolJob job) {
        stop_workers();
        {
            std::scoped_lock lock(shares_mutex_);
            shares_.clear();
        }
        clear_failure();
        current_job_ = std::make_shared<const PoolJob>(std::move(job));
        stop_source_ = std::stop_source{};
        const auto shared_job = current_job_;
        const auto job_stop = stop_source_.get_token();
        for (std::size_t i = 0; i < engines_.size(); ++i) {
            if (engines_[i] == nullptr) {
                continue;
            }
            auto* engine = engines_[i].get();
            workers_.emplace_back(
                [this, shared_job, job_stop, engine, i](
                    const std::stop_token thread_stop) {
                    mine(shared_job, job_stop, thread_stop, engine, i);
                });
        }
    }

    void stop() {
        stop_workers();
        current_job_.reset();
    }

    void reset_hashes() {
        for (const auto& counter : hashes_) {
            counter->store(0, std::memory_order_relaxed);
        }
    }

    std::uint64_t hashes_computed() const {
        std::uint64_t total = 0;
        for (const auto& counter : hashes_) {
            total += counter->load(std::memory_order_relaxed);
        }
        return total;
    }

    std::vector<std::uint64_t> per_device_hashes() const {
        std::vector<std::uint64_t> result;
        result.reserve(hashes_.size());
        for (const auto& counter : hashes_) {
            result.push_back(counter->load(std::memory_order_relaxed));
        }
        return result;
    }

    std::optional<Share> take_share(
        const std::chrono::milliseconds wait_duration) {
        std::unique_lock lock(shares_mutex_);
        shares_changed_.wait_for(lock, wait_duration, [&] {
            return !shares_.empty();
        });
        if (shares_.empty()) {
            return std::nullopt;
        }
        auto share = std::move(shares_.front());
        shares_.pop_front();
        shares_changed_.notify_all();
        return share;
    }

    std::string failure() const {
        std::scoped_lock lock(failure_mutex_);
        return failure_;
    }

private:
    void stop_workers() {
        stop_source_.request_stop();
        pause_changed_.notify_all();
        shares_changed_.notify_all();
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        workers_.clear();
    }

    void set_failure(const std::string& message) {
        std::scoped_lock lock(failure_mutex_);
        if (failure_.empty()) {
            failure_ = message;
        }
    }

    void clear_failure() {
        std::scoped_lock lock(failure_mutex_);
        failure_.clear();
    }

    void enqueue_share(
        Share share,
        const std::stop_token job_stop,
        const std::stop_token thread_stop) {
        std::unique_lock lock(shares_mutex_);
        shares_changed_.wait(lock, job_stop, [&] {
            return shares_.size() < max_queued_shares ||
                thread_stop.stop_requested();
        });
        if (job_stop.stop_requested() || thread_stop.stop_requested()) {
            return;
        }
        shares_.push_back(std::move(share));
        shares_changed_.notify_all();
    }

    void mine(
        const std::shared_ptr<const PoolJob>& job,
        const std::stop_token job_stop,
        const std::stop_token thread_stop,
        OpenClGpuMiner* engine,
        const std::size_t device_index) {
        try {
            engine->set_job(
                job->hashing_blob, job->nonce_offset, job->target);
            while (!thread_stop.stop_requested() && !job_stop.stop_requested()) {
                if (paused() || device_paused(device_index)) {
                    std::unique_lock lock(pause_mutex_);
                    pause_changed_.wait_for(
                        lock, std::chrono::milliseconds(100), [&] {
                            return (!paused() && !device_paused(device_index)) ||
                                thread_stop.stop_requested() ||
                                job_stop.stop_requested();
                        });
                    continue;
                }

                const auto batch_size =
                    static_cast<std::uint64_t>(engine->intensity());
                const auto first_nonce = nonce_allocator_->fetch_add(
                    batch_size, std::memory_order_relaxed);
                if (first_nonce >= nonce_space) {
                    return;
                }
                const auto count = static_cast<std::uint32_t>(
                    std::min(batch_size, nonce_space - first_nonce));
                const auto candidates = engine->scan(
                    static_cast<std::uint32_t>(first_nonce), count);
                hashes_[device_index]->fetch_add(
                    count, std::memory_order_relaxed);

                for (const auto nonce : candidates) {
                    if (thread_stop.stop_requested() ||
                        job_stop.stop_requested()) {
                        return;
                    }
                    auto hashing_blob = job->hashing_blob;
                    for (std::size_t i = 0; i < sizeof(nonce); ++i) {
                        hashing_blob[job->nonce_offset + i] =
                            static_cast<std::uint8_t>(nonce >> (8 * i));
                    }
                    const auto hash = bytecoin_slow_hash(hashing_blob);
                    if (!hash_meets_pool_target(hash, job->target)) {
                        continue;
                    }
                    enqueue_share(
                        Share{job->job_id, nonce, hash, true, device_index},
                        job_stop, thread_stop);
                }
            }
        } catch (const std::exception& error) {
            set_failure(
                "GPU " + std::to_string(device_index) +
                " OpenCL worker failed: " + error.what());
        }
    }

    std::shared_ptr<std::atomic<std::uint64_t>> nonce_allocator_;
    std::vector<std::unique_ptr<OpenClGpuMiner>> engines_;
    std::vector<std::shared_ptr<std::atomic<std::uint64_t>>> hashes_;
    std::vector<std::shared_ptr<std::atomic<bool>>> device_paused_;
    std::shared_ptr<const PoolJob> current_job_;
    std::atomic<bool> paused_{false};
    std::stop_source stop_source_;
    std::vector<std::jthread> workers_;
    mutable std::mutex failure_mutex_;
    std::string failure_;
    std::mutex shares_mutex_;
    std::condition_variable_any shares_changed_;
    std::deque<Share> shares_;
    std::mutex pause_mutex_;
    std::condition_variable pause_changed_;
};

struct PendingShare {
    std::string job_id;
    std::uint32_t nonce = 0;
    std::chrono::steady_clock::time_point submitted_at;
};

std::string format_memory(const std::uint64_t bytes) {
    std::ostringstream result;
    if (bytes >= 1024 * 1024) {
        result << std::fixed << std::setprecision(0)
               << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
    } else if (bytes >= 1024) {
        result << std::fixed << std::setprecision(0)
               << static_cast<double>(bytes) / 1024.0 << " KB";
    } else {
        result << bytes << " B";
    }
    return result.str();
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool is_ignored_response(const std::string& response) {
    const auto normalized = lowercase_ascii(response);
    return normalized.find("ignored") != std::string::npos ||
        normalized.find("stale") != std::string::npos;
}

std::string format_local_time() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm local_time{};
#if defined(_WIN32)
    if (localtime_s(&local_time, &now) != 0) {
        throw std::runtime_error("Unable to read the local system time.");
    }
#else
    if (localtime_r(&now, &local_time) == nullptr) {
        throw std::runtime_error("Unable to read the local system time.");
    }
#endif
    std::ostringstream result;
    result << std::put_time(&local_time, "%H:%M:%S");
    return result.str();
}

struct DashboardSnapshot {
    std::string pool;
    std::string transport;
    std::string connection_status;
    std::string job_id;
    std::string event;
    std::vector<DeviceInfo> gpus;
    std::vector<std::size_t> cpu_ids;
    std::vector<double> thread_hash_rates;
    std::vector<double> gpu_hash_rates;
    std::vector<bool> gpu_paused_devices;
    std::vector<int> thread_affinity;
    std::uint64_t reconnections = 0;
    std::uint64_t uptime_seconds = 0;
    std::uint64_t hashes = 0;
    std::uint64_t difficulty = 0;
    std::uint64_t submitted = 0;
    std::uint64_t accepted = 0;
    std::uint64_t ignored = 0;
    std::uint64_t rejected = 0;
    std::uint64_t pending = 0;
    std::uint64_t response_ms = 0;
    double hash_rate = 0;
    double cpu_hash_rate = 0;
    double gpu_hash_rate = 0;
    double effective_hash_rate = 0;
    std::size_t threads = 0;
    std::size_t max_threads = 0;
    std::size_t physical_cores = 0;
    bool mining = false;
    bool pause_all = false;
    bool paused = false;
    bool gpu_enabled = false;
    bool gpu_backend_enabled = false;
    std::vector<bool> gpu_enabled_devices;
    bool gpu_paused = false;
    bool show_help = false;
    bool show_thread_rates = false;
    bool full_report = false;
};

PoolMinerStatus to_pool_miner_status(const DashboardSnapshot& snapshot) {
    PoolMinerStatus status;
    status.pool = snapshot.pool;
    status.connection_status = snapshot.connection_status;
    status.job_id = snapshot.job_id;
    status.event = snapshot.event;
    status.uptime_seconds = snapshot.uptime_seconds;
    status.hashes = snapshot.hashes;
    status.difficulty = snapshot.difficulty;
    status.accepted = snapshot.accepted;
    status.ignored = snapshot.ignored;
    status.rejected = snapshot.rejected;
    status.pending = snapshot.pending;
    status.reconnections = snapshot.reconnections;
    status.response_ms = snapshot.response_ms;
    status.hash_rate = snapshot.hash_rate;
    status.cpu_hash_rate = snapshot.cpu_hash_rate;
    status.gpu_hash_rate = snapshot.gpu_hash_rate;
    status.threads = snapshot.threads;
    status.max_threads = snapshot.max_threads;
    status.physical_cores = snapshot.physical_cores;
    status.mining = snapshot.mining;
    status.pause_all = snapshot.pause_all;
    status.cpu_paused = snapshot.paused;
    status.gpu_paused = snapshot.gpu_paused;
    status.gpu_backend_enabled = snapshot.gpu_backend_enabled;
    for (std::size_t index = 0; index < snapshot.gpus.size(); ++index) {
        const auto& device = snapshot.gpus[index];
        if (!device.gpu.has_value()) {
            continue;
        }
        status.gpus.push_back(PoolMinerGpuStatus{
            device.name,
            device.gpu->vendor,
            device.gpu->processor,
            device.gpu->pci_address,
            index < snapshot.gpu_hash_rates.size()
                ? snapshot.gpu_hash_rates[index]
                : 0.0,
            index < snapshot.gpu_enabled_devices.size() &&
                snapshot.gpu_enabled_devices[index],
            snapshot.gpu_paused ||
                (index < snapshot.gpu_paused_devices.size() &&
                 snapshot.gpu_paused_devices[index])
        });
    }
    return status;
}

class ConsoleDashboard {
public:
    explicit ConsoleDashboard(const bool enabled = true) {
        if (!enabled) {
            return;
        }
#if defined(_WIN32)
        const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (output != INVALID_HANDLE_VALUE &&
            GetConsoleMode(output, &mode) != 0 &&
            _isatty(_fileno(stdin)) != 0) {
            output_handle_ = output;
            original_output_mode_ = mode;
            output_mode_changed_ = SetConsoleMode(
                output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
            interactive_ = output_mode_changed_;
        }
#else
        if (isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0 &&
            tcgetattr(STDIN_FILENO, &original_terminal_) == 0) {
            auto terminal = original_terminal_;
            terminal.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            terminal.c_cc[VMIN] = 0;
            terminal.c_cc[VTIME] = 0;
            interactive_ =
                tcsetattr(STDIN_FILENO, TCSANOW, &terminal) == 0;
            terminal_mode_changed_ = interactive_;
        }
#endif
        if (interactive_) {
            input_thread_ = std::jthread(
                [this](const std::stop_token stop) { read_keys(stop); });
        }
    }

    ConsoleDashboard(const ConsoleDashboard&) = delete;
    ConsoleDashboard& operator=(const ConsoleDashboard&) = delete;

    ~ConsoleDashboard() {
        input_thread_.request_stop();
        if (input_thread_.joinable()) {
            input_thread_.join();
        }
        if (!interactive_) {
            return;
        }
        std::cout << "\x1b[0m\n" << std::flush;
#if defined(_WIN32)
        if (output_mode_changed_) {
            SetConsoleMode(output_handle_, original_output_mode_);
        }
#else
        if (terminal_mode_changed_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal_);
        }
#endif
    }

    bool interactive() const {
        return interactive_;
    }

    std::optional<char> take_key() {
        std::scoped_lock lock(keys_mutex_);
        if (keys_.empty()) {
            return std::nullopt;
        }
        const auto key = keys_.front();
        keys_.pop_front();
        return key;
    }

    void render(const DashboardSnapshot& state) const {
        if (!interactive_) {
            return;
        }
        const auto hours = state.uptime_seconds / 3600;
        const auto minutes = (state.uptime_seconds / 60) % 60;
        const auto seconds = state.uptime_seconds % 60;
        const auto timestamp = format_local_time();
        const auto status_color = [](const std::string_view status) {
            if (status == "CONNECTED" || status == "MINING") {
                return "\x1b[1;32m";
            }
            if (status == "PAUSED") {
                return "\x1b[1;33m";
            }
            if (status == "UNSUPPORTED" || status == "REJECTED") {
                return "\x1b[1;31m";
            }
            return "\x1b[1;36m";
        };
        const auto event_color =
            state.event.find("error") != std::string::npos ||
                    state.event.find("rejected") != std::string::npos
                ? "\x1b[1;31m"
                : state.event.find("accepted") != std::string::npos
                    ? "\x1b[1;32m"
                    : state.event.find("paused") != std::string::npos
                        ? "\x1b[1;33m"
                        : "\x1b[1;36m";

        std::ostringstream screen;
        screen << "\x1b[2J\x1b[H"
               << "\x1b[1;36m BCN MINER \x1b[0m"
               << "\x1b[1;37m | BYTECOIN \x1b[0m"
               << "\x1b[1;33m/ CryptoNight\x1b[0m\n"
               << "\x1b[90m============================================================"
                  "\x1b[0m\n"
               << "\x1b[1;36mPOOL\x1b[0m  "
               << status_color(state.connection_status)
               << state.connection_status << "\x1b[0m"
               << "  \x1b[97m" << state.pool << "\x1b[0m  "
               << "\x1b[90m[" << state.transport << "]\x1b[0m\n"
               << "\x1b[1;34m GENERAL \x1b[0m\n";
        const auto metric = [&](const std::string_view label,
                                const auto& value) {
            screen << "\x1b[90m" << timestamp << "\x1b[0m | "
                   << "\x1b[36m" << label << "\x1b[0m"
                   << "\x1b[97m" << value << "\x1b[0m\n";
        };
        metric("Reconnections: ", state.reconnections);
        metric("Currency: ", "Cryptonote");
        metric("Current pool difficulty (approx.): ", state.difficulty);
        metric("Accepted shares: ", state.accepted);
        metric("Total hashes: ", state.hashes);
        {
            std::ostringstream uptime;
            uptime << hours << ':' << std::setfill('0') << std::setw(2)
                   << minutes << ':' << std::setw(2) << seconds;
            metric("Miner uptime: ", uptime.str());
        }
        screen << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[36mEffective hashrate (session avg.): \x1b[0m"
               << "\x1b[1;37m" << std::fixed << std::setprecision(2)
               << state.effective_hash_rate << " H/s\x1b[0m\n"
               << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[36mLive hashrate: \x1b[0m"
               << "\x1b[1;32m" << std::setprecision(1) << state.hash_rate
               << " H/s\x1b[0m\n"
               << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[36mPool network hashrate: \x1b[0m"
               << "\x1b[90mN/A (not provided by the pool protocol)\x1b[0m\n"
               << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[36mCurrent job: \x1b[0m"
               << "\x1b[97m" << state.job_id << "\x1b[0m\n"
               << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[36mShares: \x1b[0m" << state.submitted
               << " submitted / \x1b[1;32m" << state.accepted
               << " accepted\x1b[0m / \x1b[33m" << state.ignored
               << " ignored\x1b[0m / \x1b[1;31m" << state.rejected
               << " rejected\x1b[0m / \x1b[36m" << state.pending
               << " pending\x1b[0m\n"
               << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[36mLast share response: \x1b[0m"
               << "\x1b[97m" << state.response_ms << " ms\x1b[0m\n"
               << "\x1b[90m------------------------------------------------------------"
                  "\x1b[0m\n"
               << "\x1b[1;34m DEVICES \x1b[0m"
               << "\x1b[90m Shares Accepted/Ignored/Rejected - Hashrate"
                  "\x1b[0m\n"
               << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[1;34m* CPUs\x1b[0m - " << state.accepted << '/'
               << state.ignored << '/' << state.rejected << " - "
               << "\x1b[1;36m" << std::fixed << std::setprecision(2)
               << state.cpu_hash_rate << " H/s\x1b[0m ["
               << status_color(state.paused ? "PAUSED" :
                   (state.mining ? "MINING" : "WAITING FOR JOB"))
               << (state.paused ? "PAUSED" :
                   (state.mining ? "MINING" : "WAITING FOR JOB"))
               << "\x1b[0m, " << state.threads << '/' << state.max_threads
               << " workers, " << state.physical_cores << " cores / "
               << state.max_threads << " logical CPUs]\n"
               << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[34m* CPU temperature: N/A "
                  "(sensor interface unavailable)\x1b[0m\n";
        for (std::size_t worker = 0; worker < state.threads; ++worker) {
            if (state.show_thread_rates || state.full_report) {
                const auto cpu = worker < state.cpu_ids.size()
                    ? state.cpu_ids[worker]
                    : worker;
                const auto affinity = worker < state.thread_affinity.size()
                    ? state.thread_affinity[worker]
                    : -1;
                const auto rate = worker < state.thread_hash_rates.size()
                    ? state.thread_hash_rates[worker]
                    : 0.0;
                screen << "\x1b[90m" << timestamp << "\x1b[0m |   Thread "
                       << "\x1b[36m" << std::setw(2) << worker
                       << "\x1b[0m on CPU \x1b[36m" << std::setw(3) << cpu
                       << "\x1b[0m"
                       << (affinity == 1 ? " pinned" :
                           affinity == 0 ? " scheduler" : " pending")
                       << " - \x1b[97m" << std::fixed
                       << std::setprecision(1) << rate
                       << " H/s\x1b[0m\n";
            }
        }
        screen << "\x1b[1;35m GPU \x1b[0m\n";
        std::size_t listed_gpus = 0;
        for (const auto& device : state.gpus) {
            if (device.name == "OpenCL GPUs") {
                continue;
            }
            const auto gpu_rate = listed_gpus < state.gpu_hash_rates.size()
                ? state.gpu_hash_rates[listed_gpus]
                : 0.0;
            const auto gpu_status = !state.gpu_enabled
                ? "DETECTED, idle"
                : listed_gpus >= state.gpu_enabled_devices.size() ||
                        !state.gpu_enabled_devices[listed_gpus]
                    ? "UNSUPPORTED"
                : state.gpu_paused ||
                        (listed_gpus < state.gpu_paused_devices.size() &&
                         state.gpu_paused_devices[listed_gpus])
                    ? "PAUSED"
                    : state.mining
                        ? "MINING"
                        : "WAITING FOR JOB";
            screen << "\x1b[90m" << timestamp << "\x1b[0m | "
                   << "\x1b[1;35m* GPU " << listed_gpus << "\x1b[0m"
                   << " - \x1b[90mN/A/N/A/N/A\x1b[0m - "
                   << "\x1b[1;35m" << std::fixed << std::setprecision(2)
                   << gpu_rate << " H/s\x1b[0m ["
                   << status_color(gpu_status) << gpu_status << "\x1b[0m]"
                   << " - \x1b[97m" << device.name << "\x1b[0m\n"
                   << "\x1b[90m" << timestamp << "\x1b[0m |   GPU "
                   << listed_gpus
                   << " temperature: N/A (sensor interface unavailable)\n";
            ++listed_gpus;
            if (state.full_report && device.gpu.has_value()) {
                screen << "     \x1b[90mVendor: " << device.gpu->vendor
                       << " | Processor: " << device.gpu->processor
                       << " | PCI: " << device.gpu->pci_address << '\n'
                       << "     Compute units: " << device.gpu->compute_units
                       << " | Cache: "
                       << format_memory(device.gpu->cache_memory_bytes)
                       << " | Local memory: "
                       << format_memory(device.gpu->local_memory_bytes)
                       << " | Global memory: "
                       << format_memory(device.gpu->global_memory_bytes)
                       << " | Addressing: " << device.gpu->address_bits
                       << "-bit\x1b[0m\n";
            }
        }
        if (listed_gpus == 0) {
            screen << "\x1b[90m" << timestamp << "\x1b[0m | "
                   << "\x1b[1;31m* GPUs - none detected; GPU mining "
                      "unavailable\x1b[0m\n";
        }
        screen << "\x1b[90m------------------------------------------------------------"
                  "\x1b[0m\n"
               << "\x1b[90m" << timestamp << "\x1b[0m | "
               << "\x1b[36mEvent: \x1b[0m" << event_color << state.event
               << "\x1b[0m\n";
        if (state.show_help) {
            screen << "\x1b[1;33mKEYBOARD HELP\x1b[0m\n"
                   << "  \x1b[1;33mH\x1b[0m thread rates | "
                      "\x1b[1;33mR\x1b[0m full report | "
                      "\x1b[1;33mP\x1b[0m pause all | "
                      "\x1b[1;33mU\x1b[0m pause CPUs\n"
                   << "  \x1b[1;33m0-F\x1b[0m pause/resume GPU | "
                      "\x1b[1;33mT\x1b[0m temperatures\n"
                   << "  \x1b[1;33m+ or =\x1b[0m increase workers | "
                      "\x1b[1;33m-\x1b[0m decrease | "
                      "\x1b[1;33m?\x1b[0m this help\n"
                   << "  \x1b[1;33mQ or Ctrl+C\x1b[0m quits\n";
        } else {
            screen << "\x1b[1;36mKEYS\x1b[0m  "
                  "\x1b[1;33m[H]\x1b[0m Thread rates  "
                  "\x1b[1;33m[R]\x1b[0m Report  "
                  "\x1b[1;33m[P]\x1b[0m Pause all  "
                  "\x1b[1;33m[U]\x1b[0m Pause CPU  "
                  "\x1b[1;33m[T]\x1b[0m Temps  "
                  "\x1b[1;33m[+ or = / -]\x1b[0m Threads  "
                  "\x1b[1;33m[?]\x1b[0m Help  "
                  "\x1b[1;33m[Q]\x1b[0m Quit\n";
        }
        std::cout << screen.str() << std::flush;
    }

private:
    void enqueue_key(const char key) {
        std::scoped_lock lock(keys_mutex_);
        if (keys_.size() == 32) {
            keys_.pop_front();
        }
        keys_.push_back(key);
    }

    void read_keys(const std::stop_token stop) {
#if defined(_WIN32)
        while (!stop.stop_requested()) {
            if (_kbhit() != 0) {
                auto key = _getch();
                if (key == 0 || key == 224) {
                    if (_kbhit() != 0) {
                        (void)_getch();
                    }
                } else {
                    if (key >= 'A' && key <= 'Z') {
                        key = key - 'A' + 'a';
                    }
                    enqueue_key(static_cast<char>(key));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        }
#else
        while (!stop.stop_requested()) {
            fd_set input_set;
            FD_ZERO(&input_set);
            FD_SET(STDIN_FILENO, &input_set);
            timeval timeout{0, 100000};
            const auto ready =
                select(STDIN_FILENO + 1, &input_set, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                continue;
            }
            char key = 0;
            if (read(STDIN_FILENO, &key, 1) == 1) {
                if (key >= 'A' && key <= 'Z') {
                    key = static_cast<char>(key - 'A' + 'a');
                }
                enqueue_key(key);
            }
        }
#endif
    }

    bool interactive_ = false;
    mutable std::mutex keys_mutex_;
    std::deque<char> keys_;
    std::jthread input_thread_;
#if defined(_WIN32)
    HANDLE output_handle_ = INVALID_HANDLE_VALUE;
    DWORD original_output_mode_ = 0;
    bool output_mode_changed_ = false;
#else
    termios original_terminal_{};
    bool terminal_mode_changed_ = false;
#endif
};

void send_share(
    PoolConnection& connection,
    const Share& share,
    const std::string& session_id,
    const std::uint64_t request_id,
    std::unordered_map<std::uint64_t, PendingShare>& pending) {
    std::array<std::uint8_t, 4> nonce_bytes{
        static_cast<std::uint8_t>(share.nonce),
        static_cast<std::uint8_t>(share.nonce >> 8),
        static_cast<std::uint8_t>(share.nonce >> 16),
        static_cast<std::uint8_t>(share.nonce >> 24)
    };
    const auto request =
        "{\"id\":" + std::to_string(request_id) +
        ",\"jsonrpc\":\"2.0\",\"method\":\"submit\",\"params\":{\"id\":" +
        json_escape(session_id) + ",\"job_id\":" +
        json_escape(share.job_id) + ",\"nonce\":" +
        json_escape(encode_hex(nonce_bytes)) + ",\"result\":" +
        json_escape(encode_hex(share.hash)) + "}}";
    connection.write_line(request);
    pending.emplace(request_id, PendingShare{
        share.job_id, share.nonce, std::chrono::steady_clock::now()});
}

void run_connected_session(
    const PoolMinerOptions& options,
    std::atomic<bool>& shutdown,
    const std::uint64_t reconnections,
    GpuWorkers& gpu_workers,
    const std::shared_ptr<std::atomic<std::uint64_t>>& nonce_allocator,
    const PoolMinerStatusCallback& status_callback,
    PoolMinerControl* control) {
    ConsoleDashboard dashboard(!options.gui_controlled);
    std::vector<DeviceInfo> gpu_devices;
    for (auto& device : detect_devices()) {
        if (device.backend == Backend::opencl) {
            gpu_devices.push_back(std::move(device));
        }
    }
    DashboardSnapshot starting_screen;
    starting_screen.pool = options.host + ':' + options.port;
    starting_screen.transport = options.tls ? "verified TLS" : "TCP";
    starting_screen.connection_status = "CONNECTING";
    starting_screen.job_id = "waiting";
    starting_screen.event = "Starting pool connection.";
    starting_screen.gpus = gpu_devices;
    starting_screen.gpu_backend_enabled =
        options.backend != MiningBackend::cpu;
    starting_screen.pause_all = control != nullptr &&
        control->pause_all.load(std::memory_order_relaxed);
    starting_screen.paused = starting_screen.pause_all ||
        (control != nullptr &&
         control->pause_cpu.load(std::memory_order_relaxed));
    starting_screen.gpu_enabled_devices.resize(gpu_workers.device_count());
    starting_screen.gpu_paused_devices.resize(gpu_workers.device_count());
    for (std::size_t index = 0;
         index < gpu_workers.device_count(); ++index) {
        starting_screen.gpu_enabled_devices[index] =
            gpu_workers.device_enabled(index);
        starting_screen.gpu_paused_devices[index] =
            gpu_workers.device_paused(index);
    }
    starting_screen.gpu_enabled =
        gpu_workers.enabled_device_count() != 0;
    starting_screen.gpu_paused = starting_screen.pause_all;
    starting_screen.cpu_ids = options.cpu_ids;
    starting_screen.threads = options.threads;
    starting_screen.max_threads = options.max_threads;
    starting_screen.physical_cores = options.physical_cores;
    dashboard.render(starting_screen);
    if (status_callback) {
        status_callback(to_pool_miner_status(starting_screen));
    }

    PoolConnection connection(options.host, options.port, options.tls);
    const auto transport = options.tls ? "verified TLS" : "TCP";
    if (!dashboard.interactive() && !status_callback) {
        std::cout << "Connected to pool " << options.host << ':' << options.port
                  << " using " << transport << ".\n";
    }
    connection.write_line(make_login_request(options, 1));
    if (status_callback) {
        starting_screen.connection_status = "CONNECTED";
        starting_screen.event = "Connected to the pool; authenticating.";
        status_callback(to_pool_miner_status(starting_screen));
    }

    const bool cpu_enabled = options.backend != MiningBackend::gpu;
    JobWorkers workers(
        options.threads, options.max_threads, options.cpu_ids,
        nonce_allocator, options.cpu_multi_hash, cpu_enabled);
    gpu_workers.reset_hashes();
    std::string session_id;
    std::uint64_t next_request_id = 2;
    bool logged_in = false;
    std::unordered_map<std::uint64_t, PendingShare> pending;
    std::string current_job_id = "waiting";
    std::uint64_t current_difficulty = 0;
    std::uint64_t submitted_shares = 0;
    std::uint64_t accepted_shares = 0;
    std::uint64_t ignored_shares = 0;
    std::uint64_t rejected_shares = 0;
    std::uint64_t received_jobs = 0;
    std::chrono::milliseconds last_share_latency{0};
    std::string last_event = "Waiting for pool login.";
    bool show_help = false;
    bool show_thread_rates = false;
    bool full_report = false;
    const auto session_started = std::chrono::steady_clock::now();
    auto last_status = session_started;
    auto last_rate_sample = session_started;
    std::uint64_t last_cpu_hash_sample = 0;
    std::uint64_t last_gpu_hash_sample = 0;
    auto last_thread_hashes = workers.per_thread_hashes();
    double current_hash_rate = 0;
    double current_cpu_hash_rate = 0;
    double current_gpu_hash_rate = 0;
    std::vector<double> thread_hash_rates(last_thread_hashes.size(), 0.0);
    auto last_gpu_hashes = gpu_workers.per_device_hashes();
    std::vector<double> gpu_hash_rates(last_gpu_hashes.size(), 0.0);
    constexpr auto rate_sample_period = std::chrono::seconds(3);

    const auto emit_event = [&](std::string event) {
        last_event = std::move(event);
        if (!dashboard.interactive() && !status_callback) {
            std::cout << last_event << '\n';
        }
    };

    const auto refresh_dashboard = [&](const bool force) {
        if (!logged_in) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_rate_sample >= rate_sample_period) {
            const auto cpu_hashes = workers.hashes_computed();
            const auto gpu_hashes = gpu_workers.hashes_computed();
            const auto elapsed =
                std::chrono::duration<double>(now - last_rate_sample).count();
            const auto thread_hashes = workers.per_thread_hashes();
            const auto device_hashes = gpu_workers.per_device_hashes();
            if (elapsed > 0) {
                current_cpu_hash_rate =
                    static_cast<double>(
                        cpu_hashes - last_cpu_hash_sample) / elapsed;
                current_gpu_hash_rate =
                    static_cast<double>(
                        gpu_hashes - last_gpu_hash_sample) / elapsed;
                current_hash_rate =
                    current_cpu_hash_rate + current_gpu_hash_rate;
                if (thread_hash_rates.size() != thread_hashes.size()) {
                    thread_hash_rates.resize(thread_hashes.size(), 0.0);
                    last_thread_hashes.resize(thread_hashes.size(), 0);
                }
                for (std::size_t i = 0; i < thread_hashes.size(); ++i) {
                    thread_hash_rates[i] =
                        static_cast<double>(
                            thread_hashes[i] - last_thread_hashes[i]) /
                        elapsed;
                }
                if (gpu_hash_rates.size() != device_hashes.size()) {
                    gpu_hash_rates.resize(device_hashes.size(), 0.0);
                    last_gpu_hashes.resize(device_hashes.size(), 0);
                }
                for (std::size_t i = 0; i < device_hashes.size(); ++i) {
                    gpu_hash_rates[i] =
                        static_cast<double>(
                            device_hashes[i] - last_gpu_hashes[i]) / elapsed;
                }
            }
            last_cpu_hash_sample = cpu_hashes;
            last_gpu_hash_sample = gpu_hashes;
            last_thread_hashes = thread_hashes;
            last_gpu_hashes = device_hashes;
            last_rate_sample = now;
        }

        const auto refresh_period =
            dashboard.interactive() || status_callback
            ? std::chrono::seconds(1)
            : std::chrono::seconds(5);
        if ((!force || !dashboard.interactive()) &&
            now - last_status < refresh_period) {
            return;
        }
        const auto total_hashes =
            workers.hashes_computed() + gpu_workers.hashes_computed();
        const auto uptime = std::chrono::duration_cast<
            std::chrono::seconds>(now - session_started).count();
        DashboardSnapshot snapshot;
        snapshot.pool = options.host + ':' + options.port;
        snapshot.transport = transport;
        snapshot.connection_status = "CONNECTED";
        snapshot.job_id = current_job_id;
        snapshot.event = last_event;
        snapshot.gpus = gpu_devices;
        snapshot.cpu_ids = options.cpu_ids;
        snapshot.thread_hash_rates = thread_hash_rates;
        snapshot.gpu_hash_rates = gpu_hash_rates;
        snapshot.thread_affinity = workers.affinity_status();
        snapshot.reconnections = reconnections;
        snapshot.uptime_seconds = static_cast<std::uint64_t>(uptime);
        snapshot.hashes = total_hashes;
        snapshot.difficulty = current_difficulty;
        snapshot.submitted = submitted_shares;
        snapshot.accepted = accepted_shares;
        snapshot.ignored = ignored_shares;
        snapshot.rejected = rejected_shares;
        snapshot.pending = pending.size();
        snapshot.response_ms =
            static_cast<std::uint64_t>(last_share_latency.count());
        snapshot.hash_rate = current_hash_rate;
        snapshot.cpu_hash_rate = current_cpu_hash_rate;
        snapshot.gpu_hash_rate = current_gpu_hash_rate;
        snapshot.effective_hash_rate = uptime > 0
            ? static_cast<double>(total_hashes) /
                  static_cast<double>(uptime)
            : 0.0;
        snapshot.threads = workers.thread_count();
        snapshot.max_threads = options.max_threads;
        snapshot.physical_cores = options.physical_cores;
        snapshot.mining = workers.has_job() || gpu_workers.has_job();
        snapshot.pause_all = control != nullptr &&
            control->pause_all.load(std::memory_order_relaxed);
        snapshot.paused = workers.paused();
        snapshot.gpu_enabled =
            gpu_workers.enabled_device_count() != 0;
        snapshot.gpu_backend_enabled =
            options.backend != MiningBackend::cpu;
        snapshot.gpu_enabled_devices.resize(gpu_workers.device_count());
        snapshot.gpu_paused_devices.resize(gpu_workers.device_count());
        for (std::size_t i = 0;
             i < snapshot.gpu_enabled_devices.size(); ++i) {
            snapshot.gpu_enabled_devices[i] =
                gpu_workers.device_enabled(i);
            snapshot.gpu_paused_devices[i] =
                gpu_workers.device_paused(i);
        }
        snapshot.gpu_paused = gpu_workers.paused();
        snapshot.show_help = show_help;
        snapshot.show_thread_rates = show_thread_rates;
        snapshot.full_report = full_report;
        if (status_callback) {
            status_callback(to_pool_miner_status(snapshot));
        }
        if (dashboard.interactive()) {
            dashboard.render(snapshot);
        } else if (!status_callback) {
            std::cout << "[status " << uptime << "s] CPU "
                      << std::fixed << std::setprecision(1)
                      << current_cpu_hash_rate << " H/s | GPU "
                      << current_gpu_hash_rate << " H/s | "
                      << current_hash_rate << " H/s total | " << total_hashes
                      << " hashes | difficulty " << current_difficulty
                      << " | job " << current_job_id << " | shares "
                      << submitted_shares << " submitted, " << accepted_shares
                      << " accepted, " << ignored_shares << " ignored, "
                      << rejected_shares << " rejected, " << pending.size()
                      << " pending";
            if (accepted_shares != 0 || rejected_shares != 0) {
                std::cout << " | last response "
                          << last_share_latency.count() << " ms";
            }
            std::cout << '\n' << std::flush;
        }
        last_status = now;
    };

    while (!shutdown.load(std::memory_order_relaxed)) {
        if (control != nullptr) {
            const auto pause_all =
                control->pause_all.load(std::memory_order_relaxed);
            const auto pause_cpu =
                pause_all ||
                control->pause_cpu.load(std::memory_order_relaxed);
            if (workers.paused() != pause_cpu) {
                workers.set_paused(pause_cpu);
            }
            if (gpu_workers.paused() != pause_all) {
                gpu_workers.set_paused(pause_all);
            }
            for (std::size_t index = 0;
                 index < gpu_workers.device_count(); ++index) {
                const auto pause_device = control->gpu_paused(index);
                if (gpu_workers.device_paused(index) != pause_device) {
                    gpu_workers.set_device_paused(index, pause_device);
                }
            }
        }
        const auto gpu_failure = gpu_workers.failure();
        if (!gpu_failure.empty()) {
            throw std::runtime_error(gpu_failure);
        }
        bool redraw = false;
        while (const auto key = dashboard.take_key()) {
            switch (*key) {
            case 'p': {
                const bool should_pause =
                    !(workers.paused() &&
                      (gpu_workers.device_count() == 0 || gpu_workers.paused()));
                workers.set_paused(should_pause);
                gpu_workers.set_paused(should_pause);
                emit_event(should_pause
                    ? "All active mining workers paused."
                    : "All mining workers resumed.");
                redraw = true;
                break;
            }
            case 'u':
                workers.set_paused(!workers.paused());
                emit_event(workers.paused()
                    ? "CPU mining paused by user."
                    : "CPU mining resumed.");
                redraw = true;
                break;
            case '+':
            case '=':
                emit_event(workers.adjust_thread_count(1)
                    ? "CPU worker count increased to " +
                          std::to_string(workers.thread_count()) + '.'
                    : "CPU worker count is already at its configured limit.");
                redraw = true;
                break;
            case '-':
            case '_':
                emit_event(workers.adjust_thread_count(-1)
                    ? "CPU worker count decreased to " +
                          std::to_string(workers.thread_count()) + '.'
                    : "At least one CPU worker must remain active.");
                redraw = true;
                break;
            case 'h':
                show_thread_rates = !show_thread_rates;
                redraw = true;
                break;
            case 'r':
                full_report = !full_report;
                redraw = true;
                break;
            case 't':
                emit_event(
                    "Temperature readings unavailable: no supported sensor "
                    "provider is installed.");
                redraw = true;
                break;
            case '?':
                show_help = !show_help;
                redraw = true;
                break;
            case 'q':
                emit_event("Stopping miner at user request.");
                shutdown.store(true, std::memory_order_relaxed);
                redraw = true;
                break;
            default:
                if ((*key >= '0' && *key <= '9') ||
                    (*key >= 'a' && *key <= 'f')) {
                    const auto gpu_index = *key <= '9'
                        ? static_cast<std::size_t>(*key - '0')
                        : static_cast<std::size_t>(*key - 'a' + 10);
                    const auto available_gpus = static_cast<std::size_t>(
                        std::count_if(
                            gpu_devices.begin(), gpu_devices.end(),
                            [](const DeviceInfo& device) {
                                return device.gpu.has_value();
                            }));
                    if (gpu_index < available_gpus &&
                        gpu_workers.device_enabled(gpu_index)) {
                        const auto paused =
                            gpu_workers.toggle_device(gpu_index);
                        emit_event(
                            "GPU " + std::to_string(gpu_index) +
                            (paused ? " paused." : " resumed."));
                    } else {
                        emit_event("No GPU worker is assigned to that key.");
                    }
                    redraw = true;
                }
                break;
            }
        }
        refresh_dashboard(redraw);
        if (shutdown.load(std::memory_order_relaxed)) {
            break;
        }

        if (logged_in && !session_id.empty()) {
            while (auto share = workers.take_share(std::chrono::milliseconds(0))) {
                const auto request_id = next_request_id++;
                send_share(
                    connection, *share, session_id, request_id, pending);
                ++submitted_shares;
            }
            while (auto share =
                       gpu_workers.take_share(std::chrono::milliseconds(0))) {
                const auto request_id = next_request_id++;
                send_share(
                    connection, *share, session_id, request_id, pending);
                ++submitted_shares;
            }
        }

        const auto line = connection.read_line(std::chrono::milliseconds(100));
        if (!line.has_value()) {
            refresh_dashboard(false);
            continue;
        }
        if (line->empty()) {
            continue;
        }

        const auto json = parse_json(*line);
        const auto message = parse_pool_message(json, options.nonce_offset);
        if (message.has_error) {
            if (message.request_id == 1) {
                throw std::runtime_error("Pool login rejected: " +
                                         message.error_message);
            }
            if (message.request_id.has_value()) {
                const auto id = static_cast<std::uint64_t>(*message.request_id);
                if (const auto it = pending.find(id); it != pending.end()) {
                    const bool ignored =
                        is_ignored_response(message.error_message);
                    if (ignored) {
                        ++ignored_shares;
                    } else {
                        ++rejected_shares;
                    }
                    last_share_latency = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() -
                            it->second.submitted_at);
                    emit_event(
                        std::string(ignored ? "Share ignored: " : "Share rejected: ") +
                        message.error_message);
                    pending.erase(it);
                } else {
                    emit_event("Pool error: " + message.error_message);
                }
            } else {
                emit_event("Pool error: " + message.error_message);
            }
            refresh_dashboard(false);
            continue;
        }

        if (message.request_id == 1 && message.has_result) {
            logged_in = true;
            session_id = message.session_id;
            emit_event("Pool login accepted; waiting for a mining job.");
        }

        if (message.request_id.has_value() &&
            *message.request_id > 1 && message.has_result) {
            const auto id = static_cast<std::uint64_t>(*message.request_id);
            if (const auto it = pending.find(id); it != pending.end()) {
                const auto status = lowercase_ascii(message.status);
                if (status == "ok" || status == "accepted") {
                    ++accepted_shares;
                    emit_event("Pool accepted a share.");
                } else if (is_ignored_response(status)) {
                    ++ignored_shares;
                    emit_event(
                        "Pool ignored a share: " +
                        (status.empty() ? "unspecified status" : status));
                } else {
                    ++rejected_shares;
                    emit_event(
                        "Pool share response: " +
                        (status.empty() ? "unspecified status" : status));
                }
                last_share_latency = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() -
                        it->second.submitted_at);
                pending.erase(it);
            }
        }

        if (message.job.has_value()) {
            if (logged_in && session_id.empty()) {
                throw std::runtime_error(
                    "Pool login response omitted its session id.");
            }
            workers.stop();
            gpu_workers.stop();
            nonce_allocator->store(0, std::memory_order_relaxed);
            workers.start(*message.job);
            gpu_workers.start(*message.job);
            current_job_id = message.job->job_id;
            current_difficulty = message.job->target == 0
                ? 0
                : UINT64_MAX / message.job->target;
            ++received_jobs;
            std::ostringstream event;
            event << "Job " << current_job_id << " received (#" << received_jobs
                  << ", " << message.job->hashing_blob.size() << " bytes, "
                  << "approx. difficulty " << current_difficulty << ").";
            emit_event(event.str());
        }
        refresh_dashboard(false);
    }

    workers.stop();
    gpu_workers.stop();
}

}

int run_pool_miner(
    const PoolMinerOptions& options,
    std::atomic<bool>& shutdown_requested,
    PoolMinerStatusCallback status_callback,
    PoolMinerControl* control) {
    if (options.host.empty() || options.port.empty() || options.wallet.empty()) {
        throw std::invalid_argument("Pool host, port, and wallet are required.");
    }
    if (options.backend != MiningBackend::gpu && options.threads == 0) {
        throw std::invalid_argument("Miner thread count must be at least one.");
    }
    auto effective_options = options;
    if (effective_options.max_threads == 0) {
        effective_options.max_threads = detect_devices().front().logical_processors;
    }
    if (effective_options.backend != MiningBackend::gpu &&
        effective_options.threads > effective_options.max_threads) {
        throw std::invalid_argument(
            "Configured CPU thread count must be within the detected processor limit.");
    }
    if (options.nonce_offset > max_line_size) {
        throw std::invalid_argument("Nonce offset is outside supported bounds.");
    }
    if (options.backend != MiningBackend::cpu && options.nonce_offset != 39) {
        throw std::invalid_argument(
            "OpenCL GPU mining currently requires --nonce-offset 39.");
    }
#if !defined(BCNMINER_HAS_OPENSSL)
    if (options.tls) {
        throw std::runtime_error(
            "TLS was requested, but this build does not include OpenSSL.");
    }
#endif

    SocketRuntime socket_runtime;
    auto nonce_allocator =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    GpuWorkers gpu_workers(options.backend, nonce_allocator);
    std::uint64_t reconnections = 0;
    while (!shutdown_requested.load(std::memory_order_relaxed)) {
        try {
            run_connected_session(
                effective_options, shutdown_requested, reconnections,
                gpu_workers, nonce_allocator, status_callback, control);
        } catch (const std::exception& error) {
            const auto gpu_failure = gpu_workers.failure();
            if (!gpu_failure.empty()) {
                gpu_workers.stop();
                throw std::runtime_error(gpu_failure);
            }
            gpu_workers.stop();
            if (shutdown_requested.load(std::memory_order_relaxed)) {
                break;
            }
            ++reconnections;
            if (status_callback) {
                PoolMinerStatus status;
                status.pool = effective_options.host + ':' +
                    effective_options.port;
                status.connection_status = "RECONNECTING";
                status.event = error.what();
                status.reconnections = reconnections;
                status.gpu_backend_enabled =
                    effective_options.backend != MiningBackend::cpu;
                status.pause_all = control != nullptr &&
                    control->pause_all.load(std::memory_order_relaxed);
                status.cpu_paused = status.pause_all ||
                    (control != nullptr &&
                     control->pause_cpu.load(std::memory_order_relaxed));
                status.gpu_paused = status.pause_all;
                status_callback(status);
            }
            std::cerr << "Pool connection failed: " << error.what()
                      << "\nRetrying in 5 seconds.\n";
            for (int i = 0; i < 50 &&
                            !shutdown_requested.load(std::memory_order_relaxed);
                 ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    std::cout << "Miner stopped.\n";
    if (status_callback) {
        PoolMinerStatus status;
        status.pool = effective_options.host + ':' +
            effective_options.port;
        status.connection_status = "STOPPED";
        status.event = "Miner stopped.";
        status.reconnections = reconnections;
        status.gpu_backend_enabled =
            effective_options.backend != MiningBackend::cpu;
        status.pause_all = control != nullptr &&
            control->pause_all.load(std::memory_order_relaxed);
        status_callback(status);
    }
    return 0;
}

}
