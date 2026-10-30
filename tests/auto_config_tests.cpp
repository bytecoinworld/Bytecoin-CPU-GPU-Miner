#include "bcnminer/auto_config.hpp"
#include "bcnminer/slow_hash.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    const auto topology = bcnminer::detect_processor_topology();
    require(topology.logical_processors >= 1,
            "processor topology should report at least one logical CPU");
    require(topology.physical_cores >= 1 &&
                topology.physical_cores <= topology.logical_processors,
            "processor topology should report a plausible physical core count");
    require(topology.preferred_cpu_ids.size() == topology.logical_processors,
            "processor topology should order every logical CPU for worker placement");

    const auto devices = bcnminer::detect_devices();
    require(devices.size() >= 3,
            "expected CPU, OpenCL, and CUDA backend entries");
    require(devices.front().backend == bcnminer::Backend::cpu,
            "CPU entry should be first");
    require(devices.front().logical_processors >= 1,
            "CPU detection should provide at least one logical processor");
    require(devices.front().physical_cores == topology.physical_cores,
            "CPU device entry should report discovered physical cores");
    require(devices.back().backend == bcnminer::Backend::cuda,
            "CUDA backend status should remain available in the device report");
    require(std::any_of(
                devices.begin(), devices.end(),
                [](const bcnminer::DeviceInfo& device) {
                    return device.backend == bcnminer::Backend::opencl;
                }),
            "device report should include OpenCL status or discovered GPUs");

    const auto single_core = bcnminer::suggest_configuration(1);
    require(single_core.cpu_threads == 1,
            "single-core configuration should use one worker");

    const auto four_core = bcnminer::suggest_configuration(4);
    require(four_core.cpu_threads == 3,
            "responsiveness-oriented configuration should reserve one worker");

    const auto zero_reported = bcnminer::suggest_configuration(0);
    require(zero_reported.cpu_threads == 1,
            "zero reported processors should safely fall back to one worker");

    const auto measured =
        bcnminer::auto_tune_cpu(1, std::chrono::milliseconds(50));
    require(measured.cpu_threads == 1,
            "single-thread tuning should select the only candidate");
    const auto expected_modes =
        bcnminer::bytecoin_slow_hash_multi_hash_supported() ? 2U : 1U;
    require(measured.measurements.size() == expected_modes &&
                std::all_of(
                    measured.measurements.begin(),
                    measured.measurements.end(),
                    [](const bcnminer::CpuBenchmarkResult& result) {
                        return result.hashes_per_second > 0;
                    }),
            "CPU tuning should measure each available hash path");
    const auto best_measurement = std::max_element(
        measured.measurements.begin(), measured.measurements.end(),
        [](const auto& left, const auto& right) {
            return left.hashes_per_second < right.hashes_per_second;
        });
    require(best_measurement->multi_hash == measured.multi_hash,
            "CPU tuning should select the fastest measured hash path");

    std::cout << "All auto-configuration tests passed.\n";
    return EXIT_SUCCESS;
}
