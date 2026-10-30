#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bcnminer {

enum class Backend {
    cpu,
    opencl,
    cuda
};

enum class DeviceStatus {
    detected,
    backend_not_built
};

struct DeviceInfo {
    Backend backend;
    std::string name;
    DeviceStatus status;
    std::string detail;
    std::size_t logical_processors = 0;
    std::size_t physical_cores = 0;
    struct GpuDetails {
        std::string vendor;
        std::string processor;
        std::string pci_address;
        std::uint32_t compute_units = 0;
        std::uint32_t address_bits = 0;
        std::uint64_t cache_memory_bytes = 0;
        std::uint64_t local_memory_bytes = 0;
        std::uint64_t global_memory_bytes = 0;
    };
    std::optional<GpuDetails> gpu;
};

struct ProcessorTopology {
    std::size_t logical_processors = 0;
    std::size_t physical_cores = 0;
    std::vector<std::size_t> preferred_cpu_ids;
};

struct CpuBenchmarkResult {
    std::size_t threads = 0;
    double hashes_per_second = 0;
    bool multi_hash = false;
};

struct AutoConfiguration {
    std::size_t cpu_threads = 0;
    std::string rationale;
    std::vector<CpuBenchmarkResult> measurements;
    bool multi_hash = false;
};

std::vector<DeviceInfo> detect_devices();
ProcessorTopology detect_processor_topology();
bool pin_current_thread_to_cpu(std::size_t logical_cpu);
AutoConfiguration suggest_configuration(std::size_t logical_processors);
AutoConfiguration auto_tune_cpu(
    std::size_t logical_processors,
    std::chrono::milliseconds sample_duration = std::chrono::milliseconds(250));
const char* backend_name(Backend backend);

}
