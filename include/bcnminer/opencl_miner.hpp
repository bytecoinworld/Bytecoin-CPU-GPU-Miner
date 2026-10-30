#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bcnminer {

struct OpenClGpuInfo {
    std::size_t ordinal = 0;
    std::size_t platform_index = 0;
    std::size_t device_index = 0;
    std::uint32_t compute_units = 0;
    std::string name;
    std::string vendor;
    std::string platform_name;
    std::string version;
    std::string driver_version;
    std::string opencl_c_version;
    std::uint64_t global_memory_bytes = 0;
    std::uint64_t max_allocation_bytes = 0;
};

std::vector<OpenClGpuInfo> list_opencl_gpu_devices();

class OpenClGpuMiner {
public:
    explicit OpenClGpuMiner(const OpenClGpuInfo& device);
    OpenClGpuMiner(const OpenClGpuMiner&) = delete;
    OpenClGpuMiner& operator=(const OpenClGpuMiner&) = delete;
    ~OpenClGpuMiner();

    std::size_t intensity() const;
    const OpenClGpuInfo& device() const;
    void set_job(
        const std::vector<std::uint8_t>& blob,
        std::size_t nonce_offset,
        std::uint64_t target);
    std::vector<std::uint32_t> scan(
        std::uint32_t first_nonce,
        std::uint32_t nonce_count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
