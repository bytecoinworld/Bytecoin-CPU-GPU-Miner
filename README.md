<div align="center">

# BCN Miner

### Bytecoin CPU + OpenCL GPU miner

**Presented by the bytecoin.world community**

[![Latest release](https://img.shields.io/github/v/release/bytecoinworld/Bytecoin-CPU-GPU-Miner?display_name=tag)](https://github.com/bytecoinworld/Bytecoin-CPU-GPU-Miner/releases/latest)
[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](https://isocpp.org/)

**[Download the latest release](https://github.com/bytecoinworld/Bytecoin-CPU-GPU-Miner/releases/latest)**
· [Report an issue](https://github.com/bytecoinworld/Bytecoin-CPU-GPU-Miner/issues)
· [bytecoin.world](https://bytecoin.world/)

</div>

BCN Miner is an open-source Bytecoin miner with CPU and OpenCL GPU backends, automatic CPU tuning, a local browser dashboard, and a CryptoNote Stratum pool client. It is developed by the bytecoin.world community.

> **Project status:** Active development. Windows is the primary target. Pool compatibility and GPU support depend on the pool, OpenCL driver, and device. CUDA is not currently supported.

## Contents

- [Highlights](#highlights)
- [Download](#download)
- [Requirements](#requirements)
- [Build from source](#build-from-source)
- [Run the miner](#run-the-miner)
- [Browser dashboard](#browser-dashboard)
- [Pool statistics API](#pool-statistics-api)
- [Command-line reference](#command-line-reference)
- [Troubleshooting](#troubleshooting)
- [Security and responsible use](#security-and-responsible-use)
- [License and third-party notices](#license-and-third-party-notices)
- [Contributing](#contributing)

## Highlights

- **🧠 CPU mining:** Bytecoin's original CryptoNight hash implementation, including runtime AES-NI dispatch on supported x86 CPUs.
- **⚙️ Automatic CPU tuning:** `--threads auto` measures supported CPU worker/hash paths and chooses a fast configuration. Workers are assigned to distinct physical cores before SMT siblings where the operating system exposes that topology.
- **🎮 OpenCL GPU mining:** Detects OpenCL devices, compiles the bundled CryptoNight kernels, verifies the hash implementation before enabling a device, and bounds work allocation using device limits.
- **🟩 NVIDIA OpenCL:** Uses a more conservative per-batch workload on NVIDIA GPUs to reduce the chance of Windows resetting a long-running kernel.
- **🔄 CPU + GPU together:** CPU and verified GPU workers share a nonce allocator, avoiding overlapping nonce ranges for a pool job.
- **✅ Candidate verification:** GPU candidates are checked again by the CPU implementation before pool submission.
- **📊 Local browser dashboard:** Configure mining, view live rates and charts, pause/resume workers, inspect startup stages, and quit the miner from a responsive interface served on `127.0.0.1`.
- **📡 Pool and session visibility:** Reports accepted/rejected shares, job state, uptime, reconnects, and per-device rates. Pool network hashrate is shown only when supplied by the pool.
- **✨ No remote dashboard assets:** The web UI and particle effect are bundled locally.

This miner targets the original Bytecoin CryptoNight variant (variant 0). It is not a general-purpose miner for every CryptoNight coin or pool protocol. GPU mode currently requires the pool blob nonce at byte offset `39`; use the pool's documented value and verify compatibility before mining.

## Download

Use the **[GitHub Releases page](https://github.com/bytecoinworld/Bytecoin-CPU-GPU-Miner/releases/latest)** for prebuilt downloads. If no release is published yet, build the project from source using the instructions below.

Release artifacts should be downloaded only from the official `bytecoinworld/Bytecoin-CPU-GPU-Miner` repository. Verify release checksums when provided.

Windows releases are intended to include:

- **MSI installer** — per-user installation with Start Menu and desktop shortcuts. The completion screen includes an optional **Launch BCN Miner now** checkbox.
- **Portable ZIP** — extract the archive and run `bcnminer.exe`; no installation is required.
- **Portable EXE** — a self-extracting launcher that starts BCN Miner from one downloaded file. It uses Windows PowerShell (included with Windows 10/11) and, on first run, extracts the program under `%LOCALAPPDATA%\BCN Miner Portable`. The miner's configuration stays with those extracted files. No administrator access is required.

An experimental **Linux x86_64 tarball** is also available as
[`bcnminer-0.1.0-linux-x64.tar.gz`](https://github.com/bytecoinworld/Bytecoin-CPU-GPU-Miner/releases/latest/download/bcnminer-0.1.0-linux-x64.tar.gz).
It is built and tested on Ubuntu 26.04. Other Linux distributions are not yet
release-certified; the package requires a compatible glibc and OpenSSL 3
runtime. GPU mining also requires a working vendor OpenCL driver.

The GitHub repository contains source code. Generated packages are written to the local `dist` directory and should be attached to a GitHub Release rather than committed to the source repository.

## Requirements

### For CPU mining

- Windows 10/11 (primary target), or a compatible CMake/C++20 development environment for building from source.
- A modern C++20 compiler and CMake 3.21 or newer to build.

### For GPU mining

- A graphics device and driver exposing a compatible OpenCL implementation.
- Install the vendor's current graphics driver and OpenCL runtime. OpenCL availability can vary between integrated and discrete GPUs.
- On hybrid-graphics Windows laptops, BCN Miner requests the high-performance NVIDIA/AMD adapter. If the driver or Windows graphics preference overrides the request, set `bcnminer.exe` to **High performance** in Windows **Settings → System → Display → Graphics**, then restart the miner.
- GPU mode requires nonce offset `39` with the current implementation.
- **CUDA is not supported.**

### For verified TLS pool connections

OpenSSL must be available when building the miner. If CMake does not find OpenSSL, the miner still builds, but TLS pool connections are unavailable.

## Build from source

Install CMake 3.21+ and a C++20 toolchain. The build uses CMake and includes the required Bytecoin hash source and OpenCL kernels in `third_party`.

### Windows (PowerShell)

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cpack --config build/CPackConfig.cmake -G ZIP
cpack --config build/CPackConfig.cmake -G WIX
```

The executable is created at `build\Release\bcnminer.exe`. The portable ZIP and MSI installer are written to `dist\`.
To create the self-extracting portable launcher after building, run:

```powershell
.\packaging\build-portable-exe.ps1
```

The script rebuilds the portable ZIP and uses the Windows IExpress utility to create `dist\bcnminer-<version>-portable.exe`.

To generate the MSI locally, install the WiX .NET tool and CPack's UI and utility extensions:

```powershell
dotnet tool install --tool-path "$env:USERPROFILE\.dotnet\tools" wix --version 4.0.4
wix extension add --global WixToolset.UI.wixext/4.0.4
wix extension add --global WixToolset.Util.wixext/4.0.4
$env:PATH = "$env:USERPROFILE\.dotnet\tools;$env:PATH"
```

The executable includes the Bytecoin application icon, and the dashboard serves the matching favicon.

### Linux or other CMake platforms

Windows is the primary supported target; other platforms are not currently release-certified. On a compatible platform, build and run the test suite with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

If OpenSSL is installed and discoverable by CMake, verified TLS support is enabled automatically. On Ubuntu, install the build dependencies with `sudo apt install build-essential cmake ninja-build libssl-dev`, then create the Linux x86_64 archive with:

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
cmake --install build-linux --prefix build-linux/bcnminer-0.1.0-linux-x64
mkdir -p dist
tar -czf dist/bcnminer-0.1.0-linux-x64.tar.gz \
  -C build-linux bcnminer-0.1.0-linux-x64
```

The archive contains the miner, dashboard, OpenCL kernels, and license notices.
It uses the host's OpenSSL 3 and standard C/C++ runtime libraries rather than
bundling system libraries.

## Run the miner

### Open the dashboard

From PowerShell in the repository root, after building:

```powershell
.\build\Release\bcnminer.exe --gui
```

### Start CPU mining from the command line

Replace the example endpoint and wallet with the values supplied by your pool:

```powershell
.\build\Release\bcnminer.exe --mine `
  --pool pool.example:3333 `
  --wallet <Bytecoin-wallet-address> `
  --threads auto `
  --backend cpu
```

### Start GPU or mixed CPU/GPU mining

```powershell
.\build\Release\bcnminer.exe --mine `
  --pool pool.example:3333 `
  --wallet <Bytecoin-wallet-address> `
  --backend gpu `
  --nonce-offset 39
```

```powershell
.\build\Release\bcnminer.exe --mine `
  --pool pool.example:3333 `
  --wallet <Bytecoin-wallet-address> `
  --threads auto `
  --backend both `
  --nonce-offset 39
```

Use only pool endpoints, wallet formats, passwords, and nonce offsets documented by your chosen pool. `--backend both` runs CPU workers and every GPU device that passes the startup verification; it does not mean every OpenCL device is guaranteed to work.

## Browser dashboard

The dashboard is served locally by the miner and opens in your default browser. It listens on `127.0.0.1`; it is not intended to be exposed to a public network.

From the dashboard you can:

1. Enter the pool endpoint and wallet, choose CPU/GPU/both, and configure CPU workers and TLS.
2. Start mining and follow the current startup stage (tuning, connection, authentication, or waiting for the first job). Startup time varies with hardware and pool response, so the progress bar is milestone-based and does not promise a fixed ETA.
3. View live hashrate charts, share totals, device state, and pool events.
4. Pause or resume all mining, CPU workers, or individual GPUs, and quit the miner.
5. Optionally enter a pool statistics API URL for public pool/network statistics.

Settings are saved as `myconfig.txt` beside the executable and restored on the next launch. **The pool password is stored as plain text in this file.** Keep it private and do not commit or share it.

Console shortcuts while mining: `H` toggles worker rates, `R` toggles the detailed report, `P` pauses/resumes all mining, `U` pauses/resumes CPU workers, `0`–`F` toggles the corresponding GPU, `T` reports sensor availability, `+`/`-` adjusts CPU worker count, `?` displays help, and `Q` or Ctrl+C stops mining. Temperature reporting currently shows unavailable where no sensor interface is integrated.

## Pool statistics API

The dashboard can display public pool and network statistics from an API that returns a supported JSON structure. For the exact host `pool.bytecoin.network`, it automatically fills:

```text
https://bytecoin.network/api/stats
```

For other pool hosts, enter the pool's API URL yourself. The browser requests the configured URL directly, so the API must allow cross-origin (CORS) requests. The request is read-only and does not include the configured wallet address. The panel shows aggregate pool/network information and local-session metrics; it does not provide per-wallet balance or payout history.

## Command-line reference

Show all options:

```powershell
.\build\Release\bcnminer.exe --help
```

Useful diagnostic commands:

```powershell
.\build\Release\bcnminer.exe --devices
.\build\Release\bcnminer.exe --test-gpu
.\build\Release\bcnminer.exe --auto-config
.\build\Release\bcnminer.exe --hash-hex 6465206f6d6e69627573206475626974616e64756d
```

`--devices` only lists GPUs visible through OpenCL. `--test-gpu` separately
compiles the bundled CryptoNight kernels and checks their output against the
CPU implementation, without connecting to a pool. If GPU mining fails, run
this command from the miner folder and share its complete output along with
the GPU model and NVIDIA driver version.

The last command hashes the supplied hexadecimal bytes with the Bytecoin CryptoNight implementation. Its expected output is:

```text
2f8e3df40bd11f9ac90c743ca8e32bb391da4fb98612aa3b6cdc639ee00b31f5
```

Run `--help` for the current full list of options and defaults.

## Troubleshooting

| Symptom | Checks |
|---|---|
| PowerShell says the executable is not recognized | PowerShell does not search the current directory. Use `.\build\Release\bcnminer.exe` or change to the executable directory and use `.\bcnminer.exe`. |
| Mining does not start | Confirm the pool host/port and wallet with the pool, check firewall/network access, and read the startup stage and event text in the dashboard or console. |
| GPU is missing or skipped | Run `--devices` and `--test-gpu`, update the GPU driver/OpenCL runtime, and confirm the device supports the required OpenCL features. A device is skipped if its startup hash check fails. On Windows, `CL_INVALID_COMMAND_QUEUE` (`-36`) during a GPU kernel can indicate that the graphics watchdog reset a long-running workload. |
| GPU mode rejects the pool settings | Set the pool blob nonce offset to `39`, if that is the value documented by the pool. CPU-only mining does not require the GPU nonce restriction. |
| TLS cannot be enabled | Reconfigure/rebuild with OpenSSL installed and detected by CMake. |
| Pool API panel reports a fetch/CORS error | Confirm the API URL is reachable and that its server permits browser CORS requests. |
| Settings do not load or contain unexpected values | Inspect `myconfig.txt` beside the executable. It contains pool settings and a plain-text pool password; do not share it publicly. |
| Low or unstable hashrate | Allow CPU tuning to finish, check that the intended backend/devices are active, and compare rates only under the same pool job, device, and driver conditions. Mining performance varies by hardware, thermals, and pool difficulty. |

## Security and responsible use

- Never put a wallet **private key** or seed phrase into the miner; mining requires only the pool's documented payout address.
- Treat `myconfig.txt` and any pool credentials as private because the password is stored in plain text.
- The dashboard is designed for local access. Do not expose its local server to the internet or an untrusted network.
- Pool API URLs are fetched by your browser; use only endpoints you trust.
- Mining consumes electricity and can create sustained heat. Monitor your hardware and follow the device manufacturer's thermal and power guidance.
- Mine only on systems and accounts you own or are authorized to use.

## License and third-party notices

The BCN Miner project is licensed under **GNU GPL-3.0-only**; see [`LICENSE`](LICENSE). Third-party components retain their own licenses and notices:

- Bytecoin cryptographic source: LGPL-3.0, with additional component notices preserved under [`third_party/bytecoin`](third_party/bytecoin/README.md).
- XMRig AMD-derived OpenCL kernels: GPL-3.0-or-later, documented under [`third_party/xmrig-amd`](third_party/xmrig-amd/README.md).

See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for source provenance and distribution notes. Because the Bytecoin hash library is statically linked, distributors of executable binaries must also meet the LGPL-3.0 requirements for relinking or provide another compliant distribution arrangement. A source upload alone is not a substitute for those binary-distribution obligations. This documentation is not legal advice.

## Contributing

Contributions from the Bytecoin and bytecoin.world communities are welcome. Before opening a pull request:

1. Build with the documented CMake commands.
2. Run `ctest --test-dir build -C Release --output-on-failure`.
3. Include focused tests for behavior changes and preserve relevant third-party license notices.
4. Do not include generated build output, `myconfig.txt`, wallet credentials, or other local machine data.

Please include a clear description of the change, how it was tested, and any relevant hardware/pool details that are safe to share.

---

<div align="center">

Built for Bytecoin by the **bytecoin.world community** · [GitHub](https://github.com/bytecoinworld)

</div>
