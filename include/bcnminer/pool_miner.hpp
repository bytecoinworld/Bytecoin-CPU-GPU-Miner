#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace bcnminer {

enum class MiningBackend {
    cpu,
    gpu,
    both
};

struct PoolMinerOptions {
    std::string host;
    std::string port;
    std::string wallet;
    std::string password = "x";
    std::size_t threads = 0;
    std::size_t max_threads = 0;
    std::size_t physical_cores = 0;
    std::vector<std::size_t> cpu_ids;
    std::size_t nonce_offset = 39;
    bool tls = false;
    bool cpu_multi_hash = false;
    bool gui_controlled = false;
    MiningBackend backend = MiningBackend::cpu;
};

struct PoolMinerGpuStatus {
    std::string name;
    std::string vendor;
    std::string processor;
    std::string pci_address;
    double hash_rate = 0;
    bool enabled = false;
    bool paused = false;
};

struct PoolMinerStatus {
    std::string pool;
    std::string connection_status = "IDLE";
    std::string job_id;
    std::string event;
    std::vector<PoolMinerGpuStatus> gpus;
    std::uint64_t uptime_seconds = 0;
    std::uint64_t hashes = 0;
    std::uint64_t difficulty = 0;
    std::uint64_t accepted = 0;
    std::uint64_t ignored = 0;
    std::uint64_t rejected = 0;
    std::uint64_t pending = 0;
    std::uint64_t reconnections = 0;
    std::uint64_t response_ms = 0;
    double hash_rate = 0;
    double cpu_hash_rate = 0;
    double gpu_hash_rate = 0;
    std::size_t threads = 0;
    std::size_t max_threads = 0;
    std::size_t physical_cores = 0;
    bool mining = false;
    bool pause_all = false;
    bool cpu_paused = false;
    bool gpu_paused = false;
    bool gpu_backend_enabled = false;
};

struct PoolMinerControl {
    std::atomic<bool> pause_all{false};
    std::atomic<bool> pause_cpu{false};

    void set_gpu_paused(const std::size_t index, const bool paused) {
        std::scoped_lock lock(gpu_pause_mutex_);
        if (paused) {
            paused_gpus_.insert(index);
        } else {
            paused_gpus_.erase(index);
        }
    }

    bool gpu_paused(const std::size_t index) const {
        std::scoped_lock lock(gpu_pause_mutex_);
        return paused_gpus_.contains(index);
    }

    void clear_gpu_pauses() {
        std::scoped_lock lock(gpu_pause_mutex_);
        paused_gpus_.clear();
    }

private:
    mutable std::mutex gpu_pause_mutex_;
    std::unordered_set<std::size_t> paused_gpus_;
};

using PoolMinerStatusCallback =
    std::function<void(const PoolMinerStatus& status)>;

int run_pool_miner(
    const PoolMinerOptions& options,
    std::atomic<bool>& shutdown_requested,
    PoolMinerStatusCallback status_callback = {},
    PoolMinerControl* control = nullptr);

}
