#include "bcnminer/auto_config.hpp"
#include "bcnminer/slow_hash.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <dlfcn.h>
#else
#include <dlfcn.h>
#endif

namespace bcnminer {
namespace {

constexpr std::size_t max_tuning_threads = 64;

using ClInt = std::int32_t;
using ClUInt = std::uint32_t;
using ClDeviceType = std::uint64_t;
using ClPlatformId = void*;
using ClDeviceId = void*;

constexpr ClInt cl_success = 0;
constexpr ClInt cl_device_not_found = -1;
constexpr ClUInt cl_device_name = 0x102b;
constexpr ClUInt cl_device_vendor = 0x102c;
constexpr ClUInt cl_platform_name = 0x0902;
constexpr ClUInt cl_device_max_compute_units = 0x1002;
constexpr ClUInt cl_device_address_bits = 0x100d;
constexpr ClUInt cl_device_global_mem_cache_size = 0x101e;
constexpr ClUInt cl_device_global_mem_size = 0x101f;
constexpr ClUInt cl_device_local_mem_size = 0x1023;
constexpr ClUInt cl_device_pci_bus_info_khr = 0x410f;
constexpr ClDeviceType cl_device_type_gpu = ClDeviceType{1} << 2;

class OpenClRuntime {
public:
    OpenClRuntime() {
#if defined(_WIN32)
        handle_ = LoadLibraryW(L"OpenCL.dll");
#elif defined(__APPLE__)
        handle_ = dlopen(
            "/System/Library/Frameworks/OpenCL.framework/OpenCL",
            RTLD_NOW | RTLD_LOCAL);
#else
        handle_ = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            handle_ = dlopen("libOpenCL.so", RTLD_NOW | RTLD_LOCAL);
        }
#endif
        if (handle_ == nullptr) {
            return;
        }
        get_platform_ids_ = load_symbol<GetPlatformIds>("clGetPlatformIDs");
        get_platform_info_ = load_symbol<GetPlatformInfo>("clGetPlatformInfo");
        get_device_ids_ = load_symbol<GetDeviceIds>("clGetDeviceIDs");
        get_device_info_ = load_symbol<GetDeviceInfo>("clGetDeviceInfo");
        if (get_platform_ids_ == nullptr || get_platform_info_ == nullptr ||
            get_device_ids_ == nullptr || get_device_info_ == nullptr) {
            close();
        }
    }

    OpenClRuntime(const OpenClRuntime&) = delete;
    OpenClRuntime& operator=(const OpenClRuntime&) = delete;

    ~OpenClRuntime() {
        close();
    }

    explicit operator bool() const {
        return handle_ != nullptr;
    }

    std::vector<DeviceInfo> gpu_devices() const {
        if (!*this) {
            return {};
        }

        ClUInt platform_count = 0;
        if (get_platform_ids_(0, nullptr, &platform_count) != cl_success ||
            platform_count == 0) {
            return {};
        }
        std::vector<ClPlatformId> platforms(platform_count);
        if (get_platform_ids_(platform_count, platforms.data(), nullptr) !=
            cl_success) {
            return {};
        }

        std::vector<DeviceInfo> devices;
        for (const auto platform : platforms) {
            ClUInt device_count = 0;
            const auto result = get_device_ids_(
                platform, cl_device_type_gpu, 0, nullptr, &device_count);
            if (result == cl_device_not_found || device_count == 0) {
                continue;
            }
            if (result != cl_success) {
                continue;
            }

            std::vector<ClDeviceId> platform_devices(device_count);
            if (get_device_ids_(
                    platform, cl_device_type_gpu, device_count,
                    platform_devices.data(), nullptr) != cl_success) {
                continue;
            }
            const auto platform_name =
                query_string(get_platform_info_, platform, cl_platform_name);
            for (const auto device : platform_devices) {
                auto processor =
                    query_string(get_device_info_, device, cl_device_name);
                auto name = processor;
                if (!platform_name.empty()) {
                    name += " (" + platform_name + ")";
                }
                DeviceInfo info{
                    Backend::opencl,
                    std::move(name),
                    DeviceStatus::detected,
                    "OpenCL GPU found. CryptoNight kernels are included; "
                    "device compilation and startup hash verification happen "
                    "when GPU mining starts."
                };
                DeviceInfo::GpuDetails details;
                details.vendor =
                    query_string(get_device_info_, device, cl_device_vendor);
                details.processor = std::move(processor);
                details.compute_units =
                    query_value<ClUInt>(device, cl_device_max_compute_units);
                details.address_bits =
                    query_value<ClUInt>(device, cl_device_address_bits);
                details.cache_memory_bytes =
                    query_value<std::uint64_t>(
                        device, cl_device_global_mem_cache_size);
                details.local_memory_bytes =
                    query_value<std::uint64_t>(
                        device, cl_device_local_mem_size);
                details.global_memory_bytes =
                    query_value<std::uint64_t>(
                        device, cl_device_global_mem_size);
                details.pci_address = query_pci_address(device);
                info.gpu = std::move(details);
                devices.push_back(std::move(info));
            }
        }
        return devices;
    }

private:
    using GetPlatformIds =
        ClInt (*)(ClUInt, ClPlatformId*, ClUInt*);
    using GetPlatformInfo =
        ClInt (*)(ClPlatformId, ClUInt, std::size_t, void*, std::size_t*);
    using GetDeviceIds =
        ClInt (*)(ClPlatformId, ClDeviceType, ClUInt, ClDeviceId*, ClUInt*);
    using GetDeviceInfo =
        ClInt (*)(ClDeviceId, ClUInt, std::size_t, void*, std::size_t*);

    template <typename Function>
    Function load_symbol(const char* name) const {
#if defined(_WIN32)
        const auto address = GetProcAddress(
            static_cast<HMODULE>(handle_), name);
#else
        const auto address = dlsym(handle_, name);
#endif
        Function function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }

    template <typename GetInfo, typename Object>
    static std::string query_string(
        const GetInfo get_info,
        const Object object,
        const ClUInt parameter) {
        std::size_t size = 0;
        if (get_info(object, parameter, 0, nullptr, &size) != cl_success ||
            size == 0 || size > 4096) {
            return {};
        }
        std::string value(size, '\0');
        if (get_info(object, parameter, value.size(), value.data(), nullptr) !=
            cl_success) {
            return {};
        }
        if (!value.empty() && value.back() == '\0') {
            value.pop_back();
        }
        return value;
    }

    template <typename Value>
    Value query_value(const ClDeviceId device, const ClUInt parameter) const {
        Value value{};
        if (get_device_info_(
                device, parameter, sizeof(value), &value, nullptr) != cl_success) {
            return {};
        }
        return value;
    }

    std::string query_pci_address(const ClDeviceId device) const {
        struct PciAddress {
            ClUInt domain;
            ClUInt bus;
            ClUInt device;
            ClUInt function;
        };
        PciAddress address{};
        if (get_device_info_(
                device, cl_device_pci_bus_info_khr, sizeof(address),
                &address, nullptr) != cl_success) {
            return "N/A";
        }
        std::ostringstream result;
        result << std::hex << std::setfill('0') << std::setw(4)
               << address.domain << ':' << std::setw(2) << address.bus << ':'
               << std::setw(2) << address.device << '.'
               << std::dec << address.function;
        return result.str();
    }

    void close() {
        if (handle_ == nullptr) {
            return;
        }
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
        get_platform_ids_ = nullptr;
        get_platform_info_ = nullptr;
        get_device_ids_ = nullptr;
        get_device_info_ = nullptr;
    }

    void* handle_ = nullptr;
    GetPlatformIds get_platform_ids_ = nullptr;
    GetPlatformInfo get_platform_info_ = nullptr;
    GetDeviceIds get_device_ids_ = nullptr;
    GetDeviceInfo get_device_info_ = nullptr;
};

double benchmark_cpu_threads(
    const std::size_t thread_count,
    const std::chrono::milliseconds sample_duration,
    const std::vector<std::size_t>& cpu_ids,
    const bool use_multi_hash) {
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> ready{0};
    std::exception_ptr worker_error;
    std::mutex worker_error_mutex;
    std::vector<std::uint64_t> thread_hash_counts(thread_count, 0);
    std::vector<std::jthread> workers;
    workers.reserve(thread_count);
    const auto record_worker_error = [&] {
        std::scoped_lock lock(worker_error_mutex);
        if (!worker_error) {
            worker_error = std::current_exception();
        }
        stop.store(true, std::memory_order_relaxed);
    };

    for (std::size_t thread_index = 0;
         thread_index < thread_count;
         ++thread_index) {
        workers.emplace_back([&, thread_index](const std::stop_token stop_token) {
            if (thread_index < cpu_ids.size()) {
                (void)pin_current_thread_to_cpu(cpu_ids[thread_index]);
            }
            std::array<std::uint8_t, 76> input{};
            input[0] = static_cast<std::uint8_t>(thread_index);
            auto second_input = input;
            second_input[1] = 1;
            try {
                if (use_multi_hash) {
                    (void)bytecoin_slow_hash_pair(input, second_input);
                } else {
                    (void)bytecoin_slow_hash(input);
                }
            } catch (const std::exception&) {
                record_worker_error();
                ready.fetch_add(1, std::memory_order_release);
                return;
            }
            ready.fetch_add(1, std::memory_order_release);

            while (!start.load(std::memory_order_acquire) &&
                   !stop_token.stop_requested()) {
                std::this_thread::yield();
            }

            std::uint64_t count = 0;
            try {
                while (!stop.load(std::memory_order_relaxed) &&
                       !stop_token.stop_requested()) {
                    if (use_multi_hash) {
                        (void)bytecoin_slow_hash_pair(input, second_input);
                        count += 2;
                    } else {
                        (void)bytecoin_slow_hash(input);
                        ++count;
                    }
                }
            } catch (const std::exception&) {
                record_worker_error();
            }
            thread_hash_counts[thread_index] = count;
        });
    }

    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }

    const auto start_time = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    if (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(sample_duration);
    }
    stop.store(true, std::memory_order_relaxed);

    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    {
        std::scoped_lock lock(worker_error_mutex);
        if (worker_error) {
            std::rethrow_exception(worker_error);
        }
    }

    std::uint64_t total_hashes = 0;
    for (const auto count : thread_hash_counts) {
        total_hashes += count;
    }
    if (total_hashes == 0) {
        throw std::runtime_error(
            "CPU benchmark interval produced no completed hashes.");
    }

    return static_cast<double>(total_hashes) /
        std::chrono::duration<double>(elapsed).count();
}

}

ProcessorTopology detect_processor_topology() {
    ProcessorTopology topology;
    topology.logical_processors = std::max(
        1u, std::thread::hardware_concurrency());
#if defined(_WIN32)
    const auto active_logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (active_logical != 0) {
        topology.logical_processors = active_logical;
    }
    const auto group_count = GetActiveProcessorGroupCount();
    std::vector<std::size_t> group_offsets(group_count);
    std::size_t next_flat_cpu = 0;
    for (WORD group = 0; group < group_count; ++group) {
        group_offsets[group] = next_flat_cpu;
        next_flat_cpu += GetActiveProcessorCount(group);
    }

    DWORD required_bytes = 0;
    GetLogicalProcessorInformationEx(
        RelationProcessorCore, nullptr, &required_bytes);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && required_bytes != 0) {
        std::vector<std::max_align_t> storage(
            (required_bytes + sizeof(std::max_align_t) - 1) /
            sizeof(std::max_align_t));
        auto* const buffer =
            reinterpret_cast<unsigned char*>(storage.data());
        if (GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer),
                &required_bytes) != 0) {
            std::size_t offset = 0;
            std::vector<std::vector<std::size_t>> core_cpu_ids;
            while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <=
                   required_bytes) {
                const auto* entry =
                    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        buffer + offset);
                if (entry->Size == 0 || offset + entry->Size > required_bytes) {
                    break;
                }
                if (entry->Relationship == RelationProcessorCore) {
                    std::vector<std::size_t> core_cpus;
                    for (WORD mask_index = 0;
                         mask_index < entry->Processor.GroupCount;
                         ++mask_index) {
                        const auto& group_mask =
                            entry->Processor.GroupMask[mask_index];
                        if (group_mask.Group >= group_offsets.size()) {
                            continue;
                        }
                        for (std::size_t bit = 0;
                             bit < sizeof(KAFFINITY) * 8;
                             ++bit) {
                            if ((group_mask.Mask &
                                 (static_cast<KAFFINITY>(1) << bit)) != 0) {
                                core_cpus.push_back(
                                    group_offsets[group_mask.Group] + bit);
                            }
                        }
                    }
                    if (!core_cpus.empty()) {
                        core_cpu_ids.push_back(std::move(core_cpus));
                    }
                }
                offset += entry->Size;
            }
            topology.physical_cores = core_cpu_ids.size();
            std::size_t max_smt_threads = 0;
            for (const auto& core : core_cpu_ids) {
                max_smt_threads = std::max(max_smt_threads, core.size());
            }
            for (std::size_t sibling = 0;
                 sibling < max_smt_threads;
                 ++sibling) {
                for (const auto& core : core_cpu_ids) {
                    if (sibling < core.size()) {
                        topology.preferred_cpu_ids.push_back(core[sibling]);
                    }
                }
            }
        }
    }
#elif defined(__linux__)
    std::map<std::pair<unsigned, unsigned>, std::vector<std::size_t>> cores;
    for (std::size_t cpu = 0; cpu < topology.logical_processors; ++cpu) {
        unsigned package_id = 0;
        unsigned core_id = 0;
        const auto base = "/sys/devices/system/cpu/cpu" +
            std::to_string(cpu) + "/topology/";
        std::ifstream package_file(base + "physical_package_id");
        std::ifstream core_file(base + "core_id");
        if (package_file >> package_id && core_file >> core_id) {
            cores[{package_id, core_id}].push_back(cpu);
        }
    }
    topology.physical_cores = cores.empty()
        ? topology.logical_processors
        : cores.size();
    std::size_t max_smt_threads = 0;
    for (const auto& [core_id, cpu_ids] : cores) {
        (void)core_id;
        max_smt_threads = std::max(max_smt_threads, cpu_ids.size());
    }
    for (std::size_t sibling = 0;
         sibling < max_smt_threads;
         ++sibling) {
        for (const auto& [core_id, cpu_ids] : cores) {
            (void)core_id;
            if (sibling < cpu_ids.size()) {
                topology.preferred_cpu_ids.push_back(cpu_ids[sibling]);
            }
        }
    }
#else
    topology.physical_cores = topology.logical_processors;
#endif
    if (topology.physical_cores == 0) {
        topology.physical_cores = topology.logical_processors;
    }
    std::vector<bool> cpu_seen(topology.logical_processors, false);
    for (const auto cpu : topology.preferred_cpu_ids) {
        if (cpu < cpu_seen.size()) {
            cpu_seen[cpu] = true;
        }
    }
    for (std::size_t cpu = 0; cpu < cpu_seen.size(); ++cpu) {
        if (!cpu_seen[cpu]) {
            topology.preferred_cpu_ids.push_back(cpu);
        }
    }
    return topology;
}

bool pin_current_thread_to_cpu(const std::size_t logical_cpu) {
#if defined(_WIN32)
    auto cpu_index = logical_cpu;
    const auto group_count = GetActiveProcessorGroupCount();
    for (WORD group = 0; group < group_count; ++group) {
        const auto processors_in_group = GetActiveProcessorCount(group);
        if (cpu_index < processors_in_group) {
            if (cpu_index >= sizeof(KAFFINITY) * 8) {
                return false;
            }
            GROUP_AFFINITY affinity{};
            affinity.Group = group;
            affinity.Mask = static_cast<KAFFINITY>(1) << cpu_index;
            return SetThreadGroupAffinity(
                       GetCurrentThread(), &affinity, nullptr) != 0;
        }
        cpu_index -= processors_in_group;
    }
    return false;
#elif defined(__linux__)
    if (logical_cpu >= CPU_SETSIZE) {
        return false;
    }
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(logical_cpu, &cpu_set);
    return pthread_setaffinity_np(
               pthread_self(), sizeof(cpu_set), &cpu_set) == 0;
#else
    (void)logical_cpu;
    return false;
#endif
}

std::vector<DeviceInfo> detect_devices() {
    const auto topology = detect_processor_topology();

    std::vector<DeviceInfo> devices{
        {
            Backend::cpu,
            "CPU",
            DeviceStatus::detected,
            "Bytecoin CryptoNight CPU hash backend is available.",
            topology.logical_processors,
            topology.physical_cores
        }
    };

    OpenClRuntime opencl;
    auto opencl_devices = opencl.gpu_devices();
    if (opencl_devices.empty()) {
        devices.push_back({
            Backend::opencl,
            "OpenCL GPUs",
            DeviceStatus::backend_not_built,
            opencl
                ? "OpenCL runtime loaded; no GPU devices were reported."
                : "OpenCL runtime not found; install a GPU vendor driver with "
                  "OpenCL support."
        });
    } else {
        devices.insert(
            devices.end(),
            std::make_move_iterator(opencl_devices.begin()),
            std::make_move_iterator(opencl_devices.end()));
    }

    devices.push_back({
            Backend::cuda,
            "CUDA GPUs",
            DeviceStatus::backend_not_built,
            "CUDA discovery and Bytecoin CryptoNight kernels are not implemented."
        });

    return devices;
}

AutoConfiguration suggest_configuration(const std::size_t logical_processors) {
    const auto available_processors = std::max<std::size_t>(1, logical_processors);
    const auto threads = available_processors >= 4
        ? available_processors - 1
        : available_processors;

    return {
        threads,
        "Fallback responsiveness-oriented estimate from logical processor count; "
        "prefer the measured auto-tuning result."
    };
}

AutoConfiguration auto_tune_cpu(
    const std::size_t logical_processors,
    const std::chrono::milliseconds sample_duration) {
    if (sample_duration <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Benchmark sample duration must be positive.");
    }

    const auto max_threads = std::min(
        std::max<std::size_t>(1, logical_processors),
        max_tuning_threads);
    auto topology = detect_processor_topology();
    if (topology.preferred_cpu_ids.size() < max_threads) {
        topology.preferred_cpu_ids.resize(max_threads);
        for (std::size_t cpu = 0; cpu < max_threads; ++cpu) {
            topology.preferred_cpu_ids[cpu] = cpu;
        }
    }
    const bool multi_hash_supported =
        bytecoin_slow_hash_multi_hash_supported();
    AutoConfiguration configuration;
    configuration.measurements.reserve(
        max_threads * (multi_hash_supported ? 2 : 1));

    for (std::size_t threads = 1; threads <= max_threads; ++threads) {
        const auto modes_to_measure = multi_hash_supported ? 2 : 1;
        const bool start_with_multi_hash = threads % 2 == 0;
        for (std::size_t mode_index = 0;
             mode_index < modes_to_measure;
             ++mode_index) {
            const bool use_multi_hash = modes_to_measure == 2
                ? (mode_index == 0
                       ? start_with_multi_hash
                       : !start_with_multi_hash)
                : false;
            configuration.measurements.push_back({
                threads,
                benchmark_cpu_threads(
                    threads,
                    sample_duration,
                    topology.preferred_cpu_ids,
                    use_multi_hash),
                use_multi_hash
            });
        }
    }

    const auto best = std::max_element(
        configuration.measurements.begin(),
        configuration.measurements.end(),
        [](const CpuBenchmarkResult& left, const CpuBenchmarkResult& right) {
            return left.hashes_per_second < right.hashes_per_second;
        });
    configuration.cpu_threads = best->threads;
    configuration.multi_hash = best->multi_hash;
    configuration.rationale =
        "Selected by measured Bytecoin CryptoNight throughput across " +
        std::to_string(max_threads) +
        " CPU thread configurations and the available hash paths.";
    if (logical_processors > max_tuning_threads) {
        configuration.rationale +=
            " Tuning was capped at 64 threads to limit benchmark resource use.";
    }
    return configuration;
}

const char* backend_name(const Backend backend) {
    switch (backend) {
    case Backend::cpu:
        return "CPU";
    case Backend::opencl:
        return "OpenCL";
    case Backend::cuda:
        return "CUDA";
    }

    return "Unknown";
}

}
