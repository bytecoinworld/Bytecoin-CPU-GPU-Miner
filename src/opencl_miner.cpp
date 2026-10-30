#include "bcnminer/opencl_miner.hpp"

#include "bcnminer/pool_protocol.hpp"
#include "bcnminer/slow_hash.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define BCN_CL_CALL __stdcall
#else
#include <dlfcn.h>
#if defined(__linux__)
#include <unistd.h>
#endif
#define BCN_CL_CALL
#endif

namespace bcnminer {
namespace {

using ClInt = std::int32_t;
using ClUInt = std::uint32_t;
using ClULong = std::uint64_t;
using ClDeviceType = std::uint64_t;
using ClMemFlags = std::uint64_t;
using ClBool = ClUInt;
using ClPlatformId = void*;
using ClDeviceId = void*;
using ClContext = void*;
using ClCommandQueue = void*;
using ClMem = void*;
using ClProgram = void*;
using ClKernel = void*;

constexpr ClInt cl_success = 0;
constexpr ClInt cl_device_not_found = -1;
constexpr ClUInt cl_context_platform = 0x1084;
constexpr ClUInt cl_platform_name = 0x0902;
constexpr ClUInt cl_device_name = 0x102b;
constexpr ClUInt cl_device_vendor = 0x102c;
constexpr ClUInt cl_device_version = 0x102f;
constexpr ClUInt cl_device_driver_version = 0x102d;
constexpr ClUInt cl_device_opencl_c_version = 0x103d;
constexpr ClUInt cl_device_max_compute_units = 0x1002;
constexpr ClUInt cl_device_max_work_group_size = 0x1004;
constexpr ClUInt cl_device_max_mem_alloc_size = 0x1010;
constexpr ClUInt cl_device_global_mem_size = 0x101f;
constexpr ClDeviceType cl_device_type_gpu = ClDeviceType{1} << 2;
constexpr ClMemFlags cl_mem_read_write = ClMemFlags{1} << 0;
constexpr ClMemFlags cl_mem_read_only = ClMemFlags{1} << 2;
constexpr ClBool cl_true = 1;
constexpr ClUInt cl_program_build_log = 0x1183;
constexpr std::size_t crypto_night_memory = 2 * 1024 * 1024;
constexpr std::size_t work_group_size = 8;
constexpr std::size_t default_hashes_per_compute_unit = 32;
constexpr std::size_t nvidia_hashes_per_compute_unit = 1;
constexpr std::size_t maximum_intensity = 1024;
constexpr std::size_t unknown_device_intensity = 256;
constexpr std::size_t unknown_nvidia_device_intensity = 16;

struct DeviceRef {
    OpenClGpuInfo info;
    ClDeviceId id = nullptr;
    ClPlatformId platform = nullptr;
};

class OpenClApi {
public:
    OpenClApi() {
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

        get_platform_ids = load<GetPlatformIds>("clGetPlatformIDs");
        get_platform_info = load<GetPlatformInfo>("clGetPlatformInfo");
        get_device_ids = load<GetDeviceIds>("clGetDeviceIDs");
        get_device_info = load<GetDeviceInfo>("clGetDeviceInfo");
        create_context = load<CreateContext>("clCreateContext");
        release_context = load<ReleaseContext>("clReleaseContext");
        create_command_queue =
            load<CreateCommandQueue>("clCreateCommandQueue");
        release_command_queue =
            load<ReleaseCommandQueue>("clReleaseCommandQueue");
        create_buffer = load<CreateBuffer>("clCreateBuffer");
        release_mem_object = load<ReleaseMemObject>("clReleaseMemObject");
        create_program_with_source =
            load<CreateProgramWithSource>("clCreateProgramWithSource");
        build_program = load<BuildProgram>("clBuildProgram");
        get_program_build_info =
            load<GetProgramBuildInfo>("clGetProgramBuildInfo");
        release_program = load<ReleaseProgram>("clReleaseProgram");
        create_kernel = load<CreateKernel>("clCreateKernel");
        set_kernel_arg = load<SetKernelArg>("clSetKernelArg");
        release_kernel = load<ReleaseKernel>("clReleaseKernel");
        enqueue_write_buffer = load<EnqueueWriteBuffer>("clEnqueueWriteBuffer");
        enqueue_read_buffer = load<EnqueueReadBuffer>("clEnqueueReadBuffer");
        enqueue_nd_range_kernel =
            load<EnqueueNdRangeKernel>("clEnqueueNDRangeKernel");
        finish = load<Finish>("clFinish");

        if (!complete()) {
            close();
        }
    }

    OpenClApi(const OpenClApi&) = delete;
    OpenClApi& operator=(const OpenClApi&) = delete;

    ~OpenClApi() {
        close();
    }

    explicit operator bool() const {
        return handle_ != nullptr;
    }

    std::vector<DeviceRef> devices() const {
        std::vector<DeviceRef> result;
        if (!*this) {
            return result;
        }

        ClUInt platform_count = 0;
        if (get_platform_ids(0, nullptr, &platform_count) != cl_success ||
            platform_count == 0) {
            return result;
        }
        std::vector<ClPlatformId> platforms(platform_count);
        if (get_platform_ids(
                platform_count, platforms.data(), nullptr) != cl_success) {
            return result;
        }

        for (std::size_t platform_index = 0;
             platform_index < platforms.size(); ++platform_index) {
            ClUInt count = 0;
            const auto status = get_device_ids(
                platforms[platform_index], cl_device_type_gpu, 0, nullptr,
                &count);
            if (status == cl_device_not_found || count == 0) {
                continue;
            }
            if (status != cl_success) {
                continue;
            }

            std::vector<ClDeviceId> platform_devices(count);
            if (get_device_ids(
                    platforms[platform_index], cl_device_type_gpu, count,
                    platform_devices.data(), nullptr) != cl_success) {
                continue;
            }

            for (std::size_t device_index = 0;
                 device_index < platform_devices.size(); ++device_index) {
                const auto device_id = platform_devices[device_index];
                OpenClGpuInfo info;
                info.ordinal = result.size();
                info.platform_index = platform_index;
                info.device_index = device_index;
                info.compute_units =
                    query_value<ClUInt>(device_id, cl_device_max_compute_units);
                info.name = query_string(device_id, cl_device_name);
                info.vendor = query_string(device_id, cl_device_vendor);
                info.platform_name =
                    query_platform_string(platforms[platform_index], cl_platform_name);
                info.version = query_string(device_id, cl_device_version);
                info.driver_version =
                    query_string(device_id, cl_device_driver_version);
                info.opencl_c_version =
                    query_string(device_id, cl_device_opencl_c_version);
                info.global_memory_bytes =
                    query_value<ClULong>(device_id, cl_device_global_mem_size);
                info.max_allocation_bytes =
                    query_value<ClULong>(
                        device_id, cl_device_max_mem_alloc_size);
                result.push_back(DeviceRef{
                    std::move(info), device_id, platforms[platform_index]});
            }
        }
        return result;
    }

    ClContext create_context_for(
        const ClPlatformId platform,
        const ClDeviceId device,
        ClInt* error) const {
        const std::array<std::intptr_t, 3> properties{
            static_cast<std::intptr_t>(cl_context_platform),
            reinterpret_cast<std::intptr_t>(platform),
            0
        };
        return create_context(
            properties.data(), 1, &device, nullptr, nullptr, error);
    }

    ClCommandQueue create_queue(
        const ClContext context,
        const ClDeviceId device,
        ClInt* error) const {
        return create_command_queue(context, device, 0, error);
    }

    ClMem create_memory(
        const ClContext context,
        const ClMemFlags flags,
        const std::size_t bytes,
        ClInt* error) const {
        return create_buffer(context, flags, bytes, nullptr, error);
    }

    std::string build_log(
        const ClProgram program,
        const ClDeviceId device) const {
        std::size_t length = 0;
        if (get_program_build_info(
                program, device, cl_program_build_log, 0, nullptr,
                &length) != cl_success ||
            length == 0 || length > 1024 * 1024) {
            return {};
        }
        std::string log(length, '\0');
        if (get_program_build_info(
                program, device, cl_program_build_log, log.size(), log.data(),
                nullptr) != cl_success) {
            return {};
        }
        if (!log.empty() && log.back() == '\0') {
            log.pop_back();
        }
        return log;
    }

    std::string query_string(
        const ClDeviceId device,
        const ClUInt parameter) const {
        std::size_t size = 0;
        if (get_device_info(
                device, parameter, 0, nullptr, &size) != cl_success ||
            size == 0 || size > 4096) {
            return {};
        }
        std::string value(size, '\0');
        if (get_device_info(
                device, parameter, value.size(), value.data(), nullptr) !=
            cl_success) {
            return {};
        }
        if (!value.empty() && value.back() == '\0') {
            value.pop_back();
        }
        return value;
    }

    std::string query_platform_string(
        const ClPlatformId platform,
        const ClUInt parameter) const {
        std::size_t size = 0;
        if (get_platform_info(
                platform, parameter, 0, nullptr, &size) != cl_success ||
            size == 0 || size > 4096) {
            return {};
        }
        std::string value(size, '\0');
        if (get_platform_info(
                platform, parameter, value.size(), value.data(), nullptr) !=
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
        if (get_device_info(
                device, parameter, sizeof(value), &value, nullptr) !=
            cl_success) {
            return {};
        }
        return value;
    }

    using GetPlatformIds =
        ClInt(BCN_CL_CALL*)(ClUInt, ClPlatformId*, ClUInt*);
    using GetPlatformInfo =
        ClInt(BCN_CL_CALL*)(ClPlatformId, ClUInt, std::size_t, void*,
                            std::size_t*);
    using GetDeviceIds =
        ClInt(BCN_CL_CALL*)(ClPlatformId, ClDeviceType, ClUInt, ClDeviceId*,
                            ClUInt*);
    using GetDeviceInfo =
        ClInt(BCN_CL_CALL*)(ClDeviceId, ClUInt, std::size_t, void*,
                            std::size_t*);
    using CreateContext =
        ClContext(BCN_CL_CALL*)(const std::intptr_t*, ClUInt,
                                const ClDeviceId*, void(BCN_CL_CALL*)(
                                    const char*, const void*, std::size_t,
                                    void*),
                                void*, ClInt*);
    using ReleaseContext = ClInt(BCN_CL_CALL*)(ClContext);
    using CreateCommandQueue =
        ClCommandQueue(BCN_CL_CALL*)(ClContext, ClDeviceId, ClULong, ClInt*);
    using ReleaseCommandQueue = ClInt(BCN_CL_CALL*)(ClCommandQueue);
    using CreateBuffer =
        ClMem(BCN_CL_CALL*)(ClContext, ClMemFlags, std::size_t, void*, ClInt*);
    using ReleaseMemObject = ClInt(BCN_CL_CALL*)(ClMem);
    using CreateProgramWithSource =
        ClProgram(BCN_CL_CALL*)(ClContext, ClUInt, const char**,
                                const std::size_t*, ClInt*);
    using BuildProgram =
        ClInt(BCN_CL_CALL*)(ClProgram, ClUInt, const ClDeviceId*, const char*,
                            void(BCN_CL_CALL*)(ClProgram, void*), void*);
    using GetProgramBuildInfo =
        ClInt(BCN_CL_CALL*)(ClProgram, ClDeviceId, ClUInt, std::size_t, void*,
                            std::size_t*);
    using ReleaseProgram = ClInt(BCN_CL_CALL*)(ClProgram);
    using CreateKernel = ClKernel(BCN_CL_CALL*)(ClProgram, const char*, ClInt*);
    using SetKernelArg =
        ClInt(BCN_CL_CALL*)(ClKernel, ClUInt, std::size_t, const void*);
    using ReleaseKernel = ClInt(BCN_CL_CALL*)(ClKernel);
    using EnqueueWriteBuffer =
        ClInt(BCN_CL_CALL*)(ClCommandQueue, ClMem, ClBool, std::size_t,
                            std::size_t, const void*, ClUInt, const void*,
                            void*);
    using EnqueueReadBuffer =
        ClInt(BCN_CL_CALL*)(ClCommandQueue, ClMem, ClBool, std::size_t,
                            std::size_t, void*, ClUInt, const void*, void*);
    using EnqueueNdRangeKernel =
        ClInt(BCN_CL_CALL*)(ClCommandQueue, ClKernel, ClUInt, const std::size_t*,
                            const std::size_t*, const std::size_t*, ClUInt,
                            const void*, void*);
    using Finish = ClInt(BCN_CL_CALL*)(ClCommandQueue);

private:
    template <typename Function>
    Function load(const char* name) const {
#if defined(_WIN32)
        const auto address = GetProcAddress(static_cast<HMODULE>(handle_), name);
#else
        const auto address = dlsym(handle_, name);
#endif
        Function function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }

    bool complete() const {
        return get_platform_ids != nullptr && get_platform_info != nullptr &&
            get_device_ids != nullptr && get_device_info != nullptr &&
            create_context != nullptr && release_context != nullptr &&
            create_command_queue != nullptr &&
            release_command_queue != nullptr && create_buffer != nullptr &&
            release_mem_object != nullptr &&
            create_program_with_source != nullptr && build_program != nullptr &&
            get_program_build_info != nullptr && release_program != nullptr &&
            create_kernel != nullptr && set_kernel_arg != nullptr &&
            release_kernel != nullptr && enqueue_write_buffer != nullptr &&
            enqueue_read_buffer != nullptr &&
            enqueue_nd_range_kernel != nullptr && finish != nullptr;
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
    }

    void* handle_ = nullptr;

public:
    GetPlatformIds get_platform_ids = nullptr;
    GetPlatformInfo get_platform_info = nullptr;
    GetDeviceIds get_device_ids = nullptr;
    GetDeviceInfo get_device_info = nullptr;
    CreateContext create_context = nullptr;
    ReleaseContext release_context = nullptr;
    CreateCommandQueue create_command_queue = nullptr;
    ReleaseCommandQueue release_command_queue = nullptr;
    CreateBuffer create_buffer = nullptr;
    ReleaseMemObject release_mem_object = nullptr;
    CreateProgramWithSource create_program_with_source = nullptr;
    BuildProgram build_program = nullptr;
    GetProgramBuildInfo get_program_build_info = nullptr;
    ReleaseProgram release_program = nullptr;
    CreateKernel create_kernel = nullptr;
    SetKernelArg set_kernel_arg = nullptr;
    ReleaseKernel release_kernel = nullptr;
    EnqueueWriteBuffer enqueue_write_buffer = nullptr;
    EnqueueReadBuffer enqueue_read_buffer = nullptr;
    EnqueueNdRangeKernel enqueue_nd_range_kernel = nullptr;
    Finish finish = nullptr;
};

[[noreturn]] void throw_opencl(const std::string_view operation,
                               const ClInt error) {
    throw std::runtime_error(
        "OpenCL " + std::string(operation) + " failed with error " +
        std::to_string(error) + '.');
}

void check_opencl(const ClInt error, const std::string_view operation) {
    if (error != cl_success) {
        throw_opencl(operation, error);
    }
}

std::string read_kernel_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to read OpenCL kernel: " +
                                 path.string());
    }
    std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    constexpr std::string_view raw_prefix = "R\"===(";
    constexpr std::string_view raw_suffix_head = ")===";
    std::size_t position = 0;
    while ((position = source.find(raw_prefix, position)) !=
           std::string::npos) {
        source.erase(position, raw_prefix.size());
    }
    position = 0;
    while ((position = source.find(raw_suffix_head, position)) !=
           std::string::npos) {
        const auto suffix_end = position + raw_suffix_head.size();
        if (suffix_end < source.size() && source[suffix_end] == '"') {
            source.erase(position, raw_suffix_head.size() + 1);
        } else {
            position = suffix_end;
        }
    }
    return source;
}

void replace_all(
    std::string& source,
    const std::string_view placeholder,
    const std::string_view replacement) {
    std::size_t position = 0;
    while ((position = source.find(placeholder, position)) !=
           std::string::npos) {
        source.replace(position, placeholder.size(), replacement);
        position += replacement.size();
    }
}

std::filesystem::path executable_directory() {
#if defined(_WIN32)
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path).parent_path();
#elif defined(__linux__)
    std::array<char, 4096> path{};
    const auto size = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (size <= 0) {
        return {};
    }
    return std::filesystem::path(
        std::string(path.data(), static_cast<std::size_t>(size))).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path kernel_directory() {
    const auto adjacent = executable_directory() / "kernels";
    if (std::filesystem::exists(adjacent / "cryptonight.cl")) {
        return adjacent;
    }
#if defined(BCNMINER_OPENCL_SOURCE_DIR)
    const std::filesystem::path source(BCNMINER_OPENCL_SOURCE_DIR);
    if (std::filesystem::exists(source / "cryptonight.cl")) {
        return source;
    }
#endif
    throw std::runtime_error(
        "OpenCL kernel files were not found beside the miner. Restore the "
        "kernels directory from the release package.");
}

std::string assemble_kernel_source(const std::filesystem::path& directory) {
    auto source = read_kernel_file(directory / "cryptonight.cl");
    const std::array<std::pair<std::string_view, std::string_view>, 8> parts{{
        {"XMRIG_INCLUDE_WOLF_AES", "wolf-aes.cl"},
        {"XMRIG_INCLUDE_WOLF_SKEIN", "wolf-skein.cl"},
        {"XMRIG_INCLUDE_JH", "jh.cl"},
        {"XMRIG_INCLUDE_BLAKE256", "blake256.cl"},
        {"XMRIG_INCLUDE_GROESTL256", "groestl256.cl"},
        {"XMRIG_INCLUDE_FAST_INT_MATH_V2", "fast_int_math_v2.cl"},
        {"XMRIG_INCLUDE_FAST_DIV_HEAVY", "fast_div_heavy.cl"},
        {"XMRIG_INCLUDE_CN_GPU", "cryptonight_gpu.cl"}
    }};
    for (const auto& [placeholder, file] : parts) {
        replace_all(source, placeholder, read_kernel_file(directory / file));
        source.push_back('\n');
    }
    source += read_kernel_file(directory / "cryptonight2.cl");
    return source;
}

std::uint64_t hash_target(const SlowHash& hash) {
    std::uint64_t target = 0;
    for (std::size_t i = 0; i < sizeof(target); ++i) {
        target |= static_cast<std::uint64_t>(hash[24 + i]) << (8 * i);
    }
    return target;
}

}

struct OpenClGpuMiner::Impl {
    explicit Impl(const OpenClGpuInfo& requested_device)
        : device_info(requested_device) {
        if (!api) {
            throw std::runtime_error(
                "No usable OpenCL runtime was found. Install a GPU vendor "
                "driver that provides OpenCL.");
        }
        const auto available_devices = api.devices();
        const auto found = std::find_if(
            available_devices.begin(), available_devices.end(),
            [&](const DeviceRef& ref) {
                return ref.info.ordinal == requested_device.ordinal;
            });
        if (found == available_devices.end()) {
            throw std::runtime_error(
                "The selected OpenCL GPU is no longer available.");
        }
        device_id = found->id;
        platform_id = found->platform;
        device_info = found->info;
        try {
            initialize();
        } catch (...) {
            release();
            throw;
        }
    }

    ~Impl() {
        release();
    }

    void initialize() {
        if (device_info.max_allocation_bytes < crypto_night_memory * 8 ||
            device_info.global_memory_bytes < crypto_night_memory * 16) {
            throw std::runtime_error(
                "OpenCL GPU '" + device_info.name +
                "' has insufficient memory for a safe CryptoNight work batch.");
        }
        const auto alloc_limit =
            static_cast<std::size_t>(device_info.max_allocation_bytes /
                                     crypto_night_memory);
        const auto global_limit =
            static_cast<std::size_t>(device_info.global_memory_bytes /
                                     (crypto_night_memory * 2));
        const bool nvidia_device =
            device_info.vendor.find("NVIDIA") != std::string::npos;
        const auto hashes_per_compute_unit = nvidia_device
            ? nvidia_hashes_per_compute_unit
            : default_hashes_per_compute_unit;
        const auto compute_unit_limit = device_info.compute_units == 0
            ? (nvidia_device
                   ? unknown_nvidia_device_intensity
                   : unknown_device_intensity)
            : std::min(
                  static_cast<std::size_t>(device_info.compute_units),
                  maximum_intensity / hashes_per_compute_unit) *
                  hashes_per_compute_unit;
        intensity_value = std::min(
            {maximum_intensity, compute_unit_limit, alloc_limit, global_limit});
        intensity_value =
            (intensity_value / work_group_size) * work_group_size;
        if (intensity_value < work_group_size) {
            throw std::runtime_error(
                "OpenCL GPU '" + device_info.name +
                "' cannot allocate even one CryptoNight work group.");
        }

        ClInt error = cl_success;
        context = api.create_context_for(platform_id, device_id, &error);
        check_opencl(error, "clCreateContext");
        queue = api.create_queue(context, device_id, &error);
        check_opencl(error, "clCreateCommandQueue");

        input_buffer = create_buffer(cl_mem_read_only, 128);
        scratchpad_buffer = create_buffer(
            cl_mem_read_write, crypto_night_memory * intensity_value);
        state_buffer = create_buffer(cl_mem_read_write, 200 * intensity_value);
        for (auto& branch : branch_buffers) {
            branch = create_buffer(
                cl_mem_read_write,
                sizeof(ClUInt) * (intensity_value + 2));
        }
        output_buffer = create_buffer(
            cl_mem_read_write,
            sizeof(ClUInt) * (maximum_intensity + 1));

        build_kernels();
        bind_fixed_arguments();
        run_self_test();
    }

    ClMem create_buffer(const ClMemFlags flags, const std::size_t size) {
        ClInt error = cl_success;
        auto buffer = api.create_memory(context, flags, size, &error);
        if (error != cl_success || buffer == nullptr) {
            throw_opencl("clCreateBuffer", error);
        }
        return buffer;
    }

    void build_kernels() {
        const auto source = assemble_kernel_source(kernel_directory());
        const char* source_pointer = source.c_str();
        ClInt error = cl_success;
        program = api.create_program_with_source(
            context, 1, &source_pointer, nullptr, &error);
        check_opencl(error, "clCreateProgramWithSource");
        constexpr char build_options[] =
            "-DITERATIONS=524288 -DMASK=2097136 -DWORKSIZE=8 "
            "-DSTRIDED_INDEX=0 -DMEM_CHUNK_EXPONENT=4 -DCOMP_MODE=1 "
            "-DMEMORY=2097152 -DALGO=0 -DUNROLL_FACTOR=8 "
            "-DOPENCL_DRIVER_MAJOR=0 -DWORKSIZE_GPU=8 "
            "-DOUTPUT_SIZE=1024";
        error = api.build_program(program, 1, &device_id, build_options,
                                  nullptr, nullptr);
        if (error != cl_success) {
            const auto log = api.build_log(program, device_id);
            throw std::runtime_error(
                "OpenCL CryptoNight kernel compilation failed for '" +
                device_info.name + "' (error " + std::to_string(error) +
                "). Build log:\n" + log);
        }

        constexpr std::array<const char*, 7> names{
            "cn0", "cn1", "cn2", "Blake", "Groestl", "JH", "Skein"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            kernels[i] = api.create_kernel(program, names[i], &error);
            if (error != cl_success || kernels[i] == nullptr) {
                throw_opencl(
                    std::string("clCreateKernel(") + names[i] + ')', error);
            }
        }
    }

    void bind_argument(
        const ClKernel kernel,
        const ClUInt index,
        const std::size_t size,
        const void* value,
        const std::string_view name) {
        check_opencl(
            api.set_kernel_arg(kernel, index, size, value),
            std::string("clSetKernelArg(") + std::string(name) + ')');
    }

    void bind_memory(
        const ClKernel kernel,
        const ClUInt index,
        const ClMem* memory,
        const std::string_view name) {
        bind_argument(kernel, index, sizeof(ClMem), memory, name);
    }

    void bind_fixed_arguments() {
        bind_memory(kernels[0], 0, &input_buffer, "cn0");
        bind_memory(kernels[0], 1, &scratchpad_buffer, "cn0");
        bind_memory(kernels[0], 2, &state_buffer, "cn0");

        bind_memory(kernels[1], 0, &scratchpad_buffer, "cn1");
        bind_memory(kernels[1], 1, &state_buffer, "cn1");
        const ClUInt variant = 0;
        bind_argument(kernels[1], 2, sizeof(variant), &variant, "cn1");
        bind_memory(kernels[1], 3, &input_buffer, "cn1");

        bind_memory(kernels[2], 0, &scratchpad_buffer, "cn2");
        bind_memory(kernels[2], 1, &state_buffer, "cn2");
        for (std::size_t i = 0; i < branch_buffers.size(); ++i) {
            bind_memory(
                kernels[2], static_cast<ClUInt>(i + 2),
                &branch_buffers[i], "cn2");
        }

        constexpr std::array<std::size_t, 4> finalizer_indices{3, 4, 5, 6};
        for (std::size_t i = 0; i < finalizer_indices.size(); ++i) {
            const auto kernel = kernels[finalizer_indices[i]];
            bind_memory(kernel, 0, &state_buffer, "finalizer");
            bind_memory(kernel, 1, &branch_buffers[i], "finalizer");
            bind_memory(kernel, 2, &output_buffer, "finalizer");
        }
    }

    void set_job(
        const std::vector<std::uint8_t>& blob,
        const std::size_t nonce_offset,
        const std::uint64_t target) {
        if (nonce_offset != 39) {
            throw std::invalid_argument(
                "The OpenCL CryptoNight kernel currently requires "
                "--nonce-offset 39.");
        }
        if (blob.size() < nonce_offset + sizeof(std::uint32_t) ||
            blob.size() > 124) {
            throw std::invalid_argument(
                "OpenCL CryptoNight jobs require a blob of 43 to 124 bytes.");
        }
        std::array<std::uint8_t, 128> input{};
        std::copy(blob.begin(), blob.end(), input.begin());
        input[blob.size()] = 1;
        check_opencl(
            api.enqueue_write_buffer(
                queue, input_buffer, cl_true, 0, input.size(), input.data(),
                0, nullptr, nullptr),
            "clEnqueueWriteBuffer(input)");

        target_value = target;
        for (const auto kernel_index : {std::size_t{3}, std::size_t{4},
                                        std::size_t{5}, std::size_t{6}}) {
            bind_argument(
                kernels[kernel_index], 3, sizeof(target_value), &target_value,
                "finalizer");
        }
        job_loaded = true;
    }

    std::vector<std::uint32_t> scan(
        const std::uint32_t first_nonce,
        const std::uint32_t nonce_count) {
        if (!job_loaded) {
            throw std::logic_error("OpenCL mining job has not been initialized.");
        }
        if (nonce_count == 0 || nonce_count > intensity_value) {
            throw std::invalid_argument("Invalid OpenCL nonce batch size.");
        }
        const auto padded_count =
            ((static_cast<std::size_t>(nonce_count) + work_group_size - 1) /
             work_group_size) * work_group_size;
        const std::array<std::size_t, 2> offset{
            static_cast<std::size_t>(first_nonce), 0};
        const std::array<std::size_t, 2> global{padded_count, 8};
        const std::array<std::size_t, 2> local{8, 8};
        const std::array<std::size_t, 1> global_1d{padded_count};
        const std::array<std::size_t, 1> local_1d{8};
        const ClUInt threads = nonce_count;

        std::vector<ClUInt> zero_branch(intensity_value + 2, 0);
        for (const auto branch : branch_buffers) {
            check_opencl(
                api.enqueue_write_buffer(
                    queue, branch, cl_true, 0,
                    zero_branch.size() * sizeof(ClUInt), zero_branch.data(),
                    0, nullptr, nullptr),
                "clEnqueueWriteBuffer(branch counters)");
        }
        std::vector<ClUInt> zero_output(maximum_intensity + 1, 0);
        check_opencl(
            api.enqueue_write_buffer(
                queue, output_buffer, cl_true, 0,
                zero_output.size() * sizeof(ClUInt), zero_output.data(), 0,
                nullptr, nullptr),
            "clEnqueueWriteBuffer(output counter)");

        bind_argument(kernels[0], 3, sizeof(threads), &threads, "cn0");
        check_opencl(
            api.enqueue_nd_range_kernel(
                queue, kernels[0], 2, offset.data(), global.data(), local.data(),
                0, nullptr, nullptr),
            "clEnqueueNDRangeKernel(cn0)");

        bind_argument(kernels[1], 4, sizeof(threads), &threads, "cn1");
        const auto nonce_offset = offset[0];
        check_opencl(
            api.enqueue_nd_range_kernel(
                queue, kernels[1], 1, &nonce_offset, global_1d.data(),
                local_1d.data(), 0, nullptr, nullptr),
            "clEnqueueNDRangeKernel(cn1)");

        bind_argument(kernels[2], 6, sizeof(threads), &threads, "cn2");
        check_opencl(
            api.enqueue_nd_range_kernel(
                queue, kernels[2], 2, offset.data(), global.data(), local.data(),
                0, nullptr, nullptr),
            "clEnqueueNDRangeKernel(cn2)");
        check_opencl(
            api.finish(queue),
            "clFinish(core kernels, " + std::to_string(nonce_count) +
                " hashes)");

        std::array<ClUInt, 4> branch_counts{};
        for (std::size_t i = 0; i < branch_buffers.size(); ++i) {
            check_opencl(
                api.enqueue_read_buffer(
                    queue, branch_buffers[i], cl_true,
                    sizeof(ClUInt) * nonce_count, sizeof(ClUInt),
                    &branch_counts[i], 0, nullptr, nullptr),
                "clEnqueueReadBuffer(branch count)");
        }

        constexpr std::array<std::size_t, 4> finalizer_indices{3, 4, 5, 6};
        for (std::size_t i = 0; i < branch_counts.size(); ++i) {
            const auto count = std::min<ClUInt>(
                branch_counts[i], nonce_count);
            if (count == 0) {
                continue;
            }
            bind_argument(
                kernels[finalizer_indices[i]], 4, sizeof(count), &count,
                "finalizer");
            const std::array<std::size_t, 1> final_global{
                ((static_cast<std::size_t>(count) + 7) / 8) * 8};
            check_opencl(
                api.enqueue_nd_range_kernel(
                    queue, kernels[finalizer_indices[i]], 1, &nonce_offset,
                    final_global.data(), local_1d.data(), 0, nullptr, nullptr),
                "clEnqueueNDRangeKernel(finalizer)");
        }
        check_opencl(
            api.finish(queue),
            "clFinish(finalizer kernels, " + std::to_string(nonce_count) +
                " hashes)");

        std::vector<ClUInt> output(maximum_intensity + 1);
        check_opencl(
            api.enqueue_read_buffer(
                queue, output_buffer, cl_true, 0,
                output.size() * sizeof(ClUInt), output.data(), 0, nullptr,
                nullptr),
            "clEnqueueReadBuffer(output)");
        const auto count = std::min<ClUInt>(
            output[maximum_intensity],
            static_cast<ClUInt>(maximum_intensity));
        std::vector<std::uint32_t> candidates;
        candidates.reserve(count);
        for (ClUInt i = 0; i < count; ++i) {
            const auto nonce = output[i];
            if (static_cast<std::uint64_t>(nonce) >=
                    static_cast<std::uint64_t>(first_nonce) + nonce_count ||
                nonce < first_nonce) {
                continue;
            }
            candidates.push_back(nonce);
        }
        return candidates;
    }

    void run_self_test() {
        std::vector<std::uint8_t> test_blob(76);
        for (std::size_t i = 0; i < test_blob.size(); ++i) {
            test_blob[i] = static_cast<std::uint8_t>((i * 37 + 11) & 0xff);
        }
        std::fill_n(test_blob.begin() + 39, 4, 0);
        const auto expected = bytecoin_slow_hash(test_blob);
        set_job(test_blob, 39, UINT64_MAX);
        const auto candidates_at_max_target =
            scan(0, static_cast<std::uint32_t>(intensity_value));
        if (std::find(candidates_at_max_target.begin(),
                      candidates_at_max_target.end(), 0) ==
            candidates_at_max_target.end()) {
            throw std::runtime_error(
                "OpenCL CryptoNight self-test produced no candidate for GPU '" +
                device_info.name + "'.");
        }
        const auto expected_target = hash_target(expected);
        set_job(test_blob, 39, expected_target);
        const auto candidates_at_hash_target =
            scan(0, static_cast<std::uint32_t>(intensity_value));
        if (std::find(candidates_at_hash_target.begin(),
                      candidates_at_hash_target.end(), 0) ==
            candidates_at_hash_target.end()) {
            throw std::runtime_error(
                "OpenCL CryptoNight target self-test failed for GPU '" +
                device_info.name +
                "'. GPU mining remains disabled for this device.");
        }
        if (!hash_meets_pool_target(expected, expected_target)) {
            throw std::runtime_error(
                "Internal CryptoNight self-test target check failed.");
        }
    }

    void release() {
        for (auto& kernel : kernels) {
            if (kernel != nullptr) {
                api.release_kernel(kernel);
                kernel = nullptr;
            }
        }
        if (program != nullptr) {
            api.release_program(program);
            program = nullptr;
        }
        if (output_buffer != nullptr) {
            api.release_mem_object(output_buffer);
            output_buffer = nullptr;
        }
        for (auto& branch : branch_buffers) {
            if (branch != nullptr) {
                api.release_mem_object(branch);
                branch = nullptr;
            }
        }
        if (state_buffer != nullptr) {
            api.release_mem_object(state_buffer);
            state_buffer = nullptr;
        }
        if (scratchpad_buffer != nullptr) {
            api.release_mem_object(scratchpad_buffer);
            scratchpad_buffer = nullptr;
        }
        if (input_buffer != nullptr) {
            api.release_mem_object(input_buffer);
            input_buffer = nullptr;
        }
        if (queue != nullptr) {
            api.release_command_queue(queue);
            queue = nullptr;
        }
        if (context != nullptr) {
            api.release_context(context);
            context = nullptr;
        }
    }

    OpenClApi api;
    OpenClGpuInfo device_info;
    ClDeviceId device_id = nullptr;
    ClPlatformId platform_id = nullptr;
    ClContext context = nullptr;
    ClCommandQueue queue = nullptr;
    ClMem input_buffer = nullptr;
    ClMem scratchpad_buffer = nullptr;
    ClMem state_buffer = nullptr;
    std::array<ClMem, 4> branch_buffers{};
    ClMem output_buffer = nullptr;
    ClProgram program = nullptr;
    std::array<ClKernel, 7> kernels{};
    std::size_t intensity_value = 0;
    std::uint64_t target_value = 0;
    bool job_loaded = false;
};

std::vector<OpenClGpuInfo> list_opencl_gpu_devices() {
    OpenClApi api;
    if (!api) {
        return {};
    }
    std::vector<OpenClGpuInfo> devices;
    for (auto& device : api.devices()) {
        devices.push_back(std::move(device.info));
    }
    return devices;
}

OpenClGpuMiner::OpenClGpuMiner(const OpenClGpuInfo& device)
    : impl_(std::make_unique<Impl>(device)) {}

OpenClGpuMiner::~OpenClGpuMiner() = default;

std::size_t OpenClGpuMiner::intensity() const {
    return impl_->intensity_value;
}

const OpenClGpuInfo& OpenClGpuMiner::device() const {
    return impl_->device_info;
}

void OpenClGpuMiner::set_job(
    const std::vector<std::uint8_t>& blob,
    const std::size_t nonce_offset,
    const std::uint64_t target) {
    impl_->set_job(blob, nonce_offset, target);
}

std::vector<std::uint32_t> OpenClGpuMiner::scan(
    const std::uint32_t first_nonce,
    const std::uint32_t nonce_count) {
    return impl_->scan(first_nonce, nonce_count);
}

}
