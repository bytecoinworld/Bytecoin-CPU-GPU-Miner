#include "bcnminer/auto_config.hpp"
#include "bcnminer/gui_server.hpp"
#include "bcnminer/opencl_miner.hpp"
#include "bcnminer/pool_miner.hpp"
#include "bcnminer/slow_hash.hpp"

#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <thread>
#include <vector>

#if defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

constexpr std::string_view version = "0.1.0";
volatile std::sig_atomic_t shutdown_signal = 0;

void handle_shutdown_signal(int) {
    shutdown_signal = 1;
}

void print_help(const char* executable) {
    std::cout
        << "BCN Miner " << version << "\n"
        << "Bytecoin CryptoNight miner with CPU and OpenCL GPU backends\n\n"
        << "Usage:\n"
        << "  " << executable << " [--gui]\n"
        << "  " << executable << " --devices\n"
        << "  " << executable << " --test-gpu\n"
        << "  " << executable << " --auto-config\n"
        << "  " << executable << " --hash-hex <hex-bytes>\n"
        << "  " << executable
        << " --mine --pool <host:port> --wallet <address> [options]\n"
        << "  " << executable << " --help | -h | help\n"
        << "  " << executable << " --version\n\n"
        << "Examples (run from the executable's directory in PowerShell):\n"
        << "  .\\bcnminer.exe --help\n"
        << "  .\\bcnminer.exe --devices\n"
        << "  .\\bcnminer.exe --auto-config\n"
        << "  .\\bcnminer.exe --hash-hex 6465206f6d6e69627573\n"
        << "  .\\bcnminer.exe --mine --pool pool.example:3333 --wallet <address> --threads auto\n\n"
        << "Commands:\n"
        << "  --gui                   Open the local browser dashboard\n"
        << "  --config                Configure, save, and start from myconfig.txt\n"
        << "  --devices               List detected CPU and GPU backends\n"
        << "  --test-gpu              Compile and verify OpenCL GPU kernels without a pool\n"
        << "  --auto-config           Benchmark and select CPU worker count\n"
        << "  --hash-hex <hex-bytes>  Hash non-empty hexadecimal input\n"
        << "  --mine                  Mine using a compatible CryptoNote Stratum pool\n\n"
        << "Mining options (used with --mine):\n"
        << "  --pool <host:port>      Pool endpoint (required; IPv6: [host]:port)\n"
        << "  --wallet <address>      Bytecoin wallet address (required)\n"
        << "  --password <text>       Pool password (default: x)\n"
        << "  --threads <count|auto>  CPU worker count (default: auto-tuned)\n"
        << "  --nonce-offset <bytes>  Nonce offset in pool blob (default: 39)\n"
        << "  --backend <cpu|gpu|both> Devices to mine with (default: cpu)\n"
        << "  --tls                   Require verified TLS (OpenSSL build required)\n\n"
        << "Mining keyboard shortcuts (interactive console):\n"
        << "  H                     Toggle per-thread hashrates\n"
        << "  R                     Toggle full device/worker report\n"
        << "  P / U                 Pause/resume active CPU mining\n"
        << "  0-F                   Pause/resume GPU worker 0-15\n"
        << "  T                     Show temperature sensor status\n"
        << "  + / -                 Increase/decrease CPU worker count\n"
        << "  ?                     Show keyboard help\n"
        << "  Q or Ctrl+C           Stop mining\n\n"
        << "Running without arguments opens the local browser dashboard. Settings are\n"
        << "saved in myconfig.txt beside the executable; its pool password is plain text.\n"
        << "Use --help, -h, or help to show this message.\n";
}

int hex_nibble(const char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool decode_hex(const std::string_view text, std::vector<std::uint8_t>& bytes) {
    if (text.empty() || text.size() % 2 != 0) {
        return false;
    }

    bytes.clear();
    bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const auto high = hex_nibble(text[i]);
        const auto low = hex_nibble(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return true;
}

int hash_hex(const std::string_view input_hex) {
    std::vector<std::uint8_t> input;
    if (!decode_hex(input_hex, input)) {
        std::cerr << "Input must be non-empty, even-length hexadecimal bytes.\n";
        return 2;
    }

    const auto hash = bcnminer::bytecoin_slow_hash(input);
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : hash) {
        std::cout << digits[byte >> 4] << digits[byte & 0x0f];
    }
    std::cout << '\n';
    return 0;
}

bool parse_unsigned(const std::string_view text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_pool_endpoint(
    const std::string_view endpoint,
    std::string& host,
    std::string& port) {
    const auto separator = endpoint.rfind(':');
    if (separator == std::string_view::npos ||
        separator == 0 || separator + 1 == endpoint.size()) {
        return false;
    }
    if (endpoint.front() == '[') {
        if (endpoint.back() != ']' || separator < 2) {
            return false;
        }
        host = endpoint.substr(1, separator - 2);
    } else {
        if (endpoint.find(':') != separator) {
            return false;
        }
        host = endpoint.substr(0, separator);
    }
    port = endpoint.substr(separator + 1);
    std::size_t port_value = 0;
    return !host.empty() && parse_unsigned(port, port_value) &&
        port_value > 0 && port_value <= 65535;
}

std::string format_memory_bytes(const std::uint64_t bytes) {
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

void print_gpu_inventory(const std::vector<bcnminer::DeviceInfo>& devices) {
    std::size_t gpu_index = 0;
    for (const auto& device : devices) {
        if (device.backend != bcnminer::Backend::opencl || !device.gpu) {
            continue;
        }
        std::cout << "Found GPU " << gpu_index++ << ", with:\n"
                  << "  Vendor:       " << device.gpu->vendor << '\n'
                  << "  Processor:    " << device.gpu->processor << '\n'
                  << "  Device:       " << device.gpu->pci_address << '\n'
                  << "  Compute-Units: " << device.gpu->compute_units << '\n'
                  << "  Cache Memory: "
                  << format_memory_bytes(device.gpu->cache_memory_bytes) << '\n'
                  << "  Local Memory: "
                  << format_memory_bytes(device.gpu->local_memory_bytes) << '\n'
                  << "  Global Memory: "
                  << format_memory_bytes(device.gpu->global_memory_bytes) << '\n'
                  << "  Addressing:   " << device.gpu->address_bits << "-bits\n"
                  << "  Mining:       OpenCL CryptoNight available (runtime verification "
                     "required)\n";
    }
    if (gpu_index == 0) {
        std::cout << "No OpenCL GPU devices were detected.\n";
    }
}

void print_cpu_thread_configuration(
    const bcnminer::PoolMinerOptions& options) {
    if (options.backend == bcnminer::MiningBackend::gpu) {
        std::cout << "CPU mining disabled; OpenCL GPU workers will be "
                     "initialized and verified.\n";
        std::cout << "CryptoNight Variation: Original CryptoNight\n";
        std::cout << "During mining, press H for thread rates, R for full "
                     "report, P to pause all, T for temperatures, and Q to "
                     "quit. Use 0-F to pause/resume individual GPUs.\n";
        return;
    }
    std::cout << "Preparing " << options.threads << " mining thread(s)...\n";
    const auto multi_hash_enabled =
        options.cpu_multi_hash &&
        bcnminer::bytecoin_slow_hash_multi_hash_supported();
    for (std::size_t thread = 0; thread < options.threads; ++thread) {
        const auto cpu = thread < options.cpu_ids.size()
            ? options.cpu_ids[thread]
            : thread;
        std::cout << "+-- Thread " << thread
                  << " config --------------------------------+\n"
                  << "| Run on CPU:            " << cpu << " [SELECTED]\n"
                  << "| Scratchpads:           "
                  << (multi_hash_enabled
                          ? "2 x 2 MiB"
                          : "1 x 2 MiB")
                  << '\n'
                  << "| Multi-hash:            "
                  << (multi_hash_enabled ? "yes (2-way AES-NI)" : "no")
                  << '\n'
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
                  << "| Assembly module:       x86 AES-NI runtime\n"
#else
                  << "| Assembly module:       portable Bytecoin backend\n"
#endif
                  << "+------------------------------------------------+\n";
    }
    std::cout << "CryptoNight Variation: Original CryptoNight\n";
    std::cout << "During mining, press H for thread rates, R for full report, "
                 "P to pause all, U to pause CPUs, T for temperatures, "
                 "and Q to quit. Use 0-F to pause/resume individual GPUs.\n";
}

int run_miner(const int argc, char** argv) {
    bcnminer::PoolMinerOptions options;
    bool pool_specified = false;
    bool wallet_specified = false;
    bool auto_threads = true;

    for (int i = 2; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        const auto read_value = [&](const char* option_name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << option_name << ".\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (argument == "--pool") {
            const auto* value = read_value("--pool");
            if (value == nullptr ||
                !parse_pool_endpoint(value, options.host, options.port)) {
                std::cerr << "Pool endpoint must be host:port or [IPv6]:port.\n";
                return 2;
            }
            pool_specified = true;
        } else if (argument == "--wallet") {
            const auto* value = read_value("--wallet");
            if (value == nullptr || *value == '\0') {
                std::cerr << "Wallet address cannot be empty.\n";
                return 2;
            }
            options.wallet = value;
            wallet_specified = true;
        } else if (argument == "--password") {
            const auto* value = read_value("--password");
            if (value == nullptr) {
                return 2;
            }
            options.password = value;
        } else if (argument == "--threads") {
            const auto* value = read_value("--threads");
            if (value == nullptr) {
                return 2;
            }
            if (std::string_view(value) == "auto") {
                auto_threads = true;
                options.threads = 0;
            } else if (!parse_unsigned(value, options.threads) ||
                       options.threads == 0 || options.threads > 256) {
                std::cerr << "Thread count must be between 1 and 256, or 'auto'.\n";
                return 2;
            } else {
                auto_threads = false;
            }
        } else if (argument == "--nonce-offset") {
            const auto* value = read_value("--nonce-offset");
            if (value == nullptr ||
                !parse_unsigned(value, options.nonce_offset) ||
                options.nonce_offset > 1024) {
                std::cerr << "Nonce offset must be between 0 and 1024.\n";
                return 2;
            }
        } else if (argument == "--tls") {
            options.tls = true;
        } else if (argument == "--backend") {
            const auto* value = read_value("--backend");
            if (value == nullptr) {
                return 2;
            }
            const std::string_view backend(value);
            if (backend == "cpu") {
                options.backend = bcnminer::MiningBackend::cpu;
            } else if (backend == "gpu") {
                options.backend = bcnminer::MiningBackend::gpu;
            } else if (backend == "both") {
                options.backend = bcnminer::MiningBackend::both;
            } else {
                std::cerr << "Backend must be cpu, gpu, or both.\n";
                return 2;
            }
        } else {
            std::cerr << "Unknown miner option: " << argument << '\n';
            return 2;
        }
    }

    if (!pool_specified || !wallet_specified) {
        std::cerr << "--mine requires both --pool host:port and --wallet address.\n";
        return 2;
    }

    if (options.backend != bcnminer::MiningBackend::cpu &&
        options.nonce_offset != 39) {
        std::cerr
            << "OpenCL GPU mining currently requires --nonce-offset 39.\n";
        return 2;
    }

    std::cout << "Analyzing processor topology...\n";
    const auto topology = bcnminer::detect_processor_topology();
    options.max_threads = topology.logical_processors;
    options.physical_cores = topology.physical_cores;
    options.cpu_ids = topology.preferred_cpu_ids;
    std::cout << "  Physical cores: " << topology.physical_cores
              << " | Logical processors: " << topology.logical_processors
              << '\n';
    std::cout << "Auto-configuration, selected CPUs will be highlighted...\n";
    if (!auto_threads && options.threads > options.max_threads) {
        std::cerr << "Thread count cannot exceed the detected "
                  << options.max_threads << " logical CPU processors.\n";
        return 2;
    }

    if (options.backend == bcnminer::MiningBackend::gpu) {
        options.threads = 0;
        std::cout << "CPU mining disabled for GPU-only mode.\n";
    } else if (auto_threads) {
        const auto safe_thread_limit = std::max<std::size_t>(
            1, (topology.logical_processors * 3) / 4);
        std::cout << "Measuring CryptoNight throughput; automatic mode limits workers "
                  << "to " << safe_thread_limit << " of "
                  << topology.logical_processors
                  << " logical processors to leave system headroom.\n";
        try {
            const auto configuration =
                bcnminer::auto_tune_cpu(safe_thread_limit);
            options.threads = configuration.cpu_threads;
            options.cpu_multi_hash = configuration.multi_hash;
            std::cout << "Auto-selected " << options.threads
                      << " CPU worker(s) from measured CryptoNight throughput; "
                      << "selected "
                      << (options.cpu_multi_hash
                              ? "two-way AES-NI multi-hash."
                              : "single-hash mode.")
                      << '\n';
        } catch (const std::exception& error) {
            std::cerr << "CPU auto-configuration failed: " << error.what() << '\n';
            return 2;
        }
    }

    std::cout << "Detecting GPUs...\n";
    const auto devices = bcnminer::detect_devices();
    print_gpu_inventory(devices);
    print_cpu_thread_configuration(options);

    shutdown_signal = 0;
    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);
    std::atomic<bool> shutdown_requested{false};
    std::jthread signal_monitor([&](const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (shutdown_signal != 0) {
                shutdown_requested.store(true, std::memory_order_relaxed);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    try {
        const auto result =
            bcnminer::run_pool_miner(options, shutdown_requested);
        signal_monitor.request_stop();
        return result;
    } catch (const std::exception& error) {
        signal_monitor.request_stop();
        std::cerr << "Miner failed: " << error.what() << '\n';
        return 2;
    }
}

struct MinerConfig {
    std::string pool = "pool.bytecoin.network:3333";
    std::string wallet;
    std::string password = "x";
    std::string threads = "auto";
    std::size_t nonce_offset = 39;
    bool tls = false;
    bcnminer::MiningBackend backend = bcnminer::MiningBackend::cpu;
};

std::string trim_ascii(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::filesystem::path config_file_path(const char* executable) {
    std::error_code error;
    const auto executable_path =
        std::filesystem::absolute(std::filesystem::path(executable), error);
    if (error) {
        throw std::runtime_error(
            "Unable to resolve the executable path for myconfig.txt: " +
            error.message());
    }
    return executable_path.parent_path() / "myconfig.txt";
}

bool valid_config(const MinerConfig& config, std::string& error) {
    std::string host;
    std::string port;
    if (!parse_pool_endpoint(config.pool, host, port)) {
        error = "pool must be host:port or [IPv6]:port";
        return false;
    }
    if (trim_ascii(config.wallet).empty()) {
        error = "wallet address is required";
        return false;
    }
    if (config.threads != "auto") {
        std::size_t threads = 0;
        if (!parse_unsigned(config.threads, threads) ||
            threads == 0 || threads > 256) {
            error = "threads must be 'auto' or a number from 1 to 256";
            return false;
        }
    }
    if (config.nonce_offset > 1024) {
        error = "nonce_offset must be between 0 and 1024";
        return false;
    }
    if (config.backend != bcnminer::MiningBackend::cpu &&
        config.nonce_offset != 39) {
        error = "GPU mining currently requires nonce_offset=39";
        return false;
    }
    return true;
}

bool load_config(
    const std::filesystem::path& path,
    MinerConfig& config,
    std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "could not open configuration file";
        return false;
    }

    MinerConfig loaded;
    bool pool_found = false;
    bool wallet_found = false;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            error = "missing '=' on line " + std::to_string(line_number);
            return false;
        }
        const auto key = lowercase_ascii(trim_ascii(line.substr(0, separator)));
        const auto value = line.substr(separator + 1);
        if (key == "version") {
            if (trim_ascii(value) != "1") {
                error = "unsupported configuration version on line " +
                    std::to_string(line_number);
                return false;
            }
        } else if (key == "pool") {
            loaded.pool = trim_ascii(value);
            pool_found = true;
        } else if (key == "wallet") {
            loaded.wallet = trim_ascii(value);
            wallet_found = true;
        } else if (key == "password") {
            loaded.password = value;
        } else if (key == "threads") {
            loaded.threads = lowercase_ascii(trim_ascii(value));
        } else if (key == "nonce_offset") {
            if (!parse_unsigned(trim_ascii(value), loaded.nonce_offset)) {
                error = "invalid nonce_offset on line " +
                    std::to_string(line_number);
                return false;
            }
        } else if (key == "tls") {
            const auto tls_value = lowercase_ascii(trim_ascii(value));
            if (tls_value == "true" || tls_value == "yes") {
                loaded.tls = true;
            } else if (tls_value == "false" || tls_value == "no") {
                loaded.tls = false;
            } else {
                error = "tls must be true or false on line " +
                    std::to_string(line_number);
                return false;
            }
        } else if (key == "backend") {
            const auto backend =
                lowercase_ascii(trim_ascii(value));
            if (backend == "cpu") {
                loaded.backend = bcnminer::MiningBackend::cpu;
            } else if (backend == "gpu") {
                loaded.backend = bcnminer::MiningBackend::gpu;
            } else if (backend == "both") {
                loaded.backend = bcnminer::MiningBackend::both;
            } else {
                error = "backend must be cpu, gpu, or both on line " +
                    std::to_string(line_number);
                return false;
            }
        } else {
            error = "unknown key '" + key + "' on line " +
                std::to_string(line_number);
            return false;
        }
    }
    if (!input.eof()) {
        error = "failed while reading configuration file";
        return false;
    }
    if (!pool_found || !wallet_found) {
        error = "pool and wallet entries are required";
        return false;
    }
    if (!valid_config(loaded, error)) {
        return false;
    }
    config = std::move(loaded);
    return true;
}

bool save_config(
    const std::filesystem::path& path,
    const MinerConfig& config,
    std::string& error) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "could not create " + path.string();
        return false;
    }
    output << "# BCN Miner configuration version 1\n"
           << "# Pool password is stored as plain text in this file.\n"
           << "version=1\n"
           << "pool=" << config.pool << '\n'
           << "wallet=" << config.wallet << '\n'
           << "password=" << config.password << '\n'
           << "threads=" << config.threads << '\n'
           << "nonce_offset=" << config.nonce_offset << '\n'
           << "tls=" << (config.tls ? "true" : "false") << '\n'
           << "backend="
           << (config.backend == bcnminer::MiningBackend::cpu
                   ? "cpu"
                   : config.backend == bcnminer::MiningBackend::gpu
                       ? "gpu"
                       : "both")
           << '\n';
    output.close();
    if (!output) {
        error = "failed while writing " + path.string();
        return false;
    }
    return true;
}

bool prompt_value(
    const std::string& label,
    const std::string& current,
    std::string& value) {
    std::cout << label;
    if (!current.empty()) {
        std::cout << " [" << current << ']';
    }
    std::cout << ": " << std::flush;
    std::string entered;
    if (!std::getline(std::cin, entered)) {
        return false;
    }
    value = entered.empty() ? current : std::move(entered);
    return true;
}

bool edit_config(MinerConfig& config) {
    std::cout << "\nConfigure pool and CPU/GPU mining settings.\n";
    std::string value;
    for (;;) {
        if (!prompt_value("Pool endpoint", config.pool, value)) {
            return false;
        }
        std::string host;
        std::string port;
        if (parse_pool_endpoint(value, host, port)) {
            config.pool = std::move(value);
            break;
        }
        std::cout << "Enter host:port or [IPv6]:port.\n";
    }

    for (;;) {
        if (!prompt_value("Bytecoin wallet address", config.wallet, value)) {
            return false;
        }
        if (!trim_ascii(value).empty()) {
            config.wallet = std::move(value);
            break;
        }
        std::cout << "A wallet address is required.\n";
    }

    std::cout << "Pool password [press Enter to keep the current value]: "
              << std::flush;
    if (!std::getline(std::cin, value)) {
        return false;
    }
    if (!value.empty()) {
        config.password = std::move(value);
    }

    for (;;) {
        const auto current_backend =
            config.backend == bcnminer::MiningBackend::cpu
                ? "cpu"
                : config.backend == bcnminer::MiningBackend::gpu
                    ? "gpu"
                    : "both";
        if (!prompt_value(
                "Backend (cpu, gpu, or both)", current_backend, value)) {
            return false;
        }
        value = lowercase_ascii(trim_ascii(std::move(value)));
        if (value == "cpu") {
            config.backend = bcnminer::MiningBackend::cpu;
            break;
        }
        if (value == "gpu") {
            config.backend = bcnminer::MiningBackend::gpu;
            break;
        }
        if (value == "both") {
            config.backend = bcnminer::MiningBackend::both;
            break;
        }
        std::cout << "Enter cpu, gpu, or both.\n";
    }

    for (;;) {
        if (!prompt_value("CPU threads (auto or 1-256)", config.threads, value)) {
            return false;
        }
        value = lowercase_ascii(trim_ascii(std::move(value)));
        if (value == "auto") {
            config.threads = std::move(value);
            break;
        }
        std::size_t threads = 0;
        if (parse_unsigned(value, threads) && threads > 0 && threads <= 256) {
            config.threads = std::move(value);
            break;
        }
        std::cout << "Enter 'auto' or a number from 1 to 256.\n";
    }

    for (;;) {
        if (!prompt_value(
                "Use verified TLS? (yes/no)",
                config.tls ? "yes" : "no",
                value)) {
            return false;
        }
        const auto answer = lowercase_ascii(trim_ascii(std::move(value)));
        if (answer == "yes" || answer == "y") {
            config.tls = true;
            break;
        }
        if (answer == "no" || answer == "n") {
            config.tls = false;
            break;
        }
        std::cout << "Enter yes or no.\n";
    }

    for (;;) {
        if (!prompt_value(
                "Nonce offset (bytes)",
                std::to_string(config.nonce_offset),
                value)) {
            return false;
        }
        std::size_t offset = 0;
        if (parse_unsigned(trim_ascii(value), offset) && offset <= 1024) {
            if (config.backend != bcnminer::MiningBackend::cpu &&
                offset != 39) {
                std::cout << "GPU mining currently requires nonce offset 39.\n";
                continue;
            }
            config.nonce_offset = offset;
            break;
        }
        std::cout << "Enter a number from 0 to 1024.\n";
    }
    return true;
}

int start_from_config(const char* executable, const MinerConfig& config) {
    std::vector<std::string> arguments{
        executable,
        "--mine",
        "--pool", config.pool,
        "--wallet", config.wallet,
        "--password", config.password,
        "--threads", config.threads,
        "--nonce-offset", std::to_string(config.nonce_offset),
        "--backend",
        config.backend == bcnminer::MiningBackend::cpu
            ? "cpu"
            : config.backend == bcnminer::MiningBackend::gpu
                ? "gpu"
                : "both"
    };
    if (config.tls) {
        arguments.emplace_back("--tls");
    }
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
        argument_pointers.push_back(argument.data());
    }
    const auto argument_count = static_cast<int>(argument_pointers.size());
    argument_pointers.push_back(nullptr);
    return run_miner(
        argument_count, argument_pointers.data());
}

int configure_miner(const char* executable) {
    std::filesystem::path config_path;
    try {
        config_path = config_file_path(executable);
    } catch (const std::exception& error) {
        std::cerr << "Configuration path error: " << error.what() << '\n';
        return 2;
    }

    std::error_code filesystem_error;
    const bool config_exists = std::filesystem::exists(config_path, filesystem_error);
    if (filesystem_error) {
        std::cerr << "Unable to check configuration file: "
                  << filesystem_error.message() << '\n';
        return 2;
    }

    MinerConfig config;
    bool config_ready = false;
    std::string error;
    if (config_exists) {
        config_ready = load_config(config_path, config, error);
        if (config_ready) {
            std::cout << "Loaded configuration from " << config_path.string()
                      << ".\n";
        } else {
            std::cout << "Configuration could not be loaded: " << error << '\n'
                      << "Choose Configure to replace or repair it.\n";
        }
    } else {
        std::cout << "First run: create " << config_path.string() << ".\n";
        if (!edit_config(config)) {
            std::cout << "\nConfiguration cancelled.\n";
            return 0;
        }
        if (!save_config(config_path, config, error)) {
            std::cerr << "Could not save configuration: " << error << '\n';
            return 2;
        }
        config_ready = true;
        std::cout << "Configuration saved to " << config_path.string() << ".\n";
    }

    for (;;) {
        std::cout << "\nBCN Miner Setup\n"
                  << "  Pool: " << config.pool << '\n'
                  << "  Wallet: "
                  << (config.wallet.empty() ? "not configured" : "configured")
                  << '\n'
                  << "  CPU threads: " << config.threads << '\n'
                  << "  Verified TLS: " << (config.tls ? "yes" : "no") << '\n'
                  << "  Backend: "
                  << (config.backend == bcnminer::MiningBackend::cpu
                          ? "CPU"
                          : config.backend == bcnminer::MiningBackend::gpu
                              ? "GPU"
                              : "CPU + GPU")
                  << '\n'
                  << "  Config file: " << config_path.string() << '\n'
                  << "  Pool password is stored in plain text.\n"
                  << "\n[S] Start mining  [C] Configure/save  [Q] Quit\n"
                  << "Choice: " << std::flush;
        std::string choice;
        if (!std::getline(std::cin, choice)) {
            return 0;
        }
        choice = lowercase_ascii(trim_ascii(std::move(choice)));
        if (choice == "q" || choice == "quit") {
            return 0;
        }
        if (choice == "s" || choice == "start") {
            if (!config_ready) {
                std::cout << "No valid saved configuration. Choose Configure first.\n";
                continue;
            }
            return start_from_config(executable, config);
        }
        if (choice == "c" || choice == "configure") {
            auto edited = config;
            if (!edit_config(edited)) {
                std::cout << "\nConfiguration cancelled; previous settings kept.\n";
                continue;
            }
            if (!save_config(config_path, edited, error)) {
                std::cerr << "Could not save configuration: " << error << '\n';
                continue;
            }
            config = std::move(edited);
            config_ready = true;
            std::cout << "Configuration saved to " << config_path.string() << ".\n";
            continue;
        }
        std::cout << "Choose S, C, or Q.\n";
    }
}

int list_devices() {
    for (const auto& device : bcnminer::detect_devices()) {
        std::cout << bcnminer::backend_name(device.backend) << ": "
                  << device.name << '\n';
        std::cout << "  Status: "
                  << (device.status == bcnminer::DeviceStatus::detected
                          ? "detected"
                          : "backend not built")
                  << '\n';
        if (device.logical_processors != 0) {
            std::cout << "  Logical processors: " << device.logical_processors << '\n';
        }
        if (device.physical_cores != 0) {
            std::cout << "  Physical cores: " << device.physical_cores << '\n';
        }
        std::cout << "  " << device.detail << '\n';
        if (device.gpu.has_value()) {
            std::cout << "  Vendor: " << device.gpu->vendor
                      << " | Processor: " << device.gpu->processor
                      << " | PCI: " << device.gpu->pci_address << '\n'
                      << "  Compute units: " << device.gpu->compute_units
                      << " | Cache: "
                      << format_memory_bytes(device.gpu->cache_memory_bytes)
                      << " | Local memory: "
                      << format_memory_bytes(device.gpu->local_memory_bytes)
                      << " | Global memory: "
                      << format_memory_bytes(device.gpu->global_memory_bytes)
                      << " | Addressing: " << device.gpu->address_bits
                      << "-bit\n";
        }
    }

    return 0;
}

int test_opencl_gpus() {
    const auto devices = bcnminer::list_opencl_gpu_devices();
    if (devices.empty()) {
        std::cerr << "No OpenCL GPU devices were detected. Check the GPU driver "
                     "and OpenCL runtime.\n";
        return 2;
    }

    std::cout << "Running OpenCL CryptoNight kernel compilation and startup "
                 "hash/target checks.\n"
              << std::flush;
    bool all_passed = true;
    for (const auto& device : devices) {
        std::cout << "\nGPU " << device.ordinal << ": " << device.name << '\n'
                  << "  Vendor: " << device.vendor << '\n'
                  << "  Platform: " << device.platform_name << '\n'
                  << "  OpenCL: " << device.version << '\n'
                  << "  OpenCL C: " << device.opencl_c_version << '\n'
                  << "  Driver: " << device.driver_version << '\n'
                  << "  Compute units: " << device.compute_units << '\n'
                  << std::flush;
        try {
            const bcnminer::OpenClGpuMiner miner(device);
            std::cout << "  PASS: kernel compiled and CryptoNight checks passed; "
                      << "batch " << miner.intensity() << " hashes.\n";
        } catch (const std::exception& error) {
            all_passed = false;
            std::cerr << "  FAIL: " << error.what() << '\n';
        }
    }
    return all_passed ? 0 : 2;
}

int print_auto_configuration() {
    const auto devices = bcnminer::detect_devices();
    const auto cpu = devices.front();
    std::cout << "Benchmarking Bytecoin CryptoNight on "
              << cpu.logical_processors
              << " logical processors; this can take several seconds.\n"
              << std::flush;
    bcnminer::AutoConfiguration configuration;
    try {
        configuration = bcnminer::auto_tune_cpu(cpu.logical_processors);
    } catch (const std::exception& error) {
        std::cerr << "CPU auto-configuration failed: " << error.what() << '\n';
        return 2;
    }

    std::cout << std::fixed << std::setprecision(2);
    for (const auto& measurement : configuration.measurements) {
        std::cout << "  " << measurement.threads << " thread(s): "
                  << (measurement.multi_hash
                          ? "two-way AES-NI "
                          : "single-hash ")
                  << measurement.hashes_per_second << " H/s\n";
    }
    std::cout << "Selected CPU worker threads: " << configuration.cpu_threads << '\n'
              << "Selected CPU hash path: "
              << (configuration.multi_hash
                      ? "two-way AES-NI multi-hash"
                      : "single-hash")
              << '\n'
              << configuration.rationale << '\n';
    return 0;
}

}

int main(const int argc, char** argv) {
    if (argc == 1) {
        return bcnminer::run_gui_server(argv[0]);
    }

    if (argc == 2) {
        const std::string_view command(argv[1]);
        if (command == "--help" || command == "-h" ||
            command == "-help" || command == "help" || command == "/?") {
            print_help(argv[0]);
            return 0;
        }
    }

    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << version << '\n';
        return 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--config") {
        return configure_miner(argv[0]);
    }

    if (argc == 2 && std::string_view(argv[1]) == "--gui") {
        return bcnminer::run_gui_server(argv[0]);
    }

    if (argc == 2 && std::string_view(argv[1]) == "--devices") {
        return list_devices();
    }

    if (argc == 2 && std::string_view(argv[1]) == "--test-gpu") {
        return test_opencl_gpus();
    }

    if (argc == 2 && std::string_view(argv[1]) == "--auto-config") {
        return print_auto_configuration();
    }

    if (argc == 3 && std::string_view(argv[1]) == "--hash-hex") {
        return hash_hex(argv[2]);
    }

    if (argc >= 2 && std::string_view(argv[1]) == "--mine") {
        return run_miner(argc, argv);
    }

    std::cerr << "Unknown or invalid arguments.\n";
    print_help(argv[0]);
    return 2;
}
