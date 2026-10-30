#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::array<char, 8> footer_magic{'B', 'C', 'N', 'S', 'F', 'X', '0', '1'};
constexpr std::size_t footer_size = 18;

void show_error(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"BCN Miner Portable", MB_OK | MB_ICONERROR);
}

std::wstring system_error_message(const wchar_t* action, DWORD error) {
    wchar_t* system_message = nullptr;
    const auto length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&system_message),
        0,
        nullptr
    );
    std::wstring message = action;
    if (length != 0 && system_message != nullptr) {
        message += L": ";
        message.append(system_message, length);
        LocalFree(system_message);
    }
    return message;
}

std::filesystem::path executable_path() {
    std::wstring buffer(1024, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size())
        );
        if (length == 0) {
            throw system_error_message(L"Could not locate the launcher", GetLastError());
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return buffer;
        }
        if (buffer.size() >= 32768) {
            throw std::runtime_error("The launcher path is too long.");
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring utf8_to_wide(const std::string& value) {
    const auto required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0
    );
    if (required <= 0) {
        throw std::runtime_error("The embedded package has an invalid folder name.");
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required
        ) != required) {
        throw std::runtime_error("The embedded package folder name could not be decoded.");
    }
    return result;
}

std::string wide_to_base64(const std::wstring& value) {
    static_assert(sizeof(wchar_t) == 2);
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    const auto byte_count = value.size() * sizeof(wchar_t);
    std::string encoded;
    encoded.reserve(((byte_count + 2) / 3) * 4);

    for (std::size_t index = 0; index < byte_count; index += 3) {
        const auto first = bytes[index];
        const auto second = index + 1 < byte_count ? bytes[index + 1] : 0;
        const auto third = index + 2 < byte_count ? bytes[index + 2] : 0;
        const auto combined =
            (static_cast<std::uint32_t>(first) << 16) |
            (static_cast<std::uint32_t>(second) << 8) |
            static_cast<std::uint32_t>(third);
        encoded.push_back(alphabet[(combined >> 18) & 0x3f]);
        encoded.push_back(alphabet[(combined >> 12) & 0x3f]);
        encoded.push_back(index + 1 < byte_count ? alphabet[(combined >> 6) & 0x3f] : '=');
        encoded.push_back(index + 2 < byte_count ? alphabet[combined & 0x3f] : '=');
    }
    return encoded;
}

std::wstring powershell_literal(const std::wstring& value) {
    std::wstring result = L"'";
    for (const auto character : value) {
        result += character;
        if (character == L'\'') {
            result += L'\'';
        }
    }
    result += L'\'';
    return result;
}

std::filesystem::path copy_embedded_archive(
    const std::filesystem::path& self,
    std::wstring& package_folder
) {
    std::ifstream source(self, std::ios::binary);
    if (!source) {
        throw std::runtime_error("Could not read the portable package.");
    }

    source.seekg(0, std::ios::end);
    const auto file_size = source.tellg();
    if (file_size < static_cast<std::streamoff>(footer_size)) {
        throw std::runtime_error("The portable package is incomplete.");
    }

    std::array<unsigned char, footer_size> footer{};
    source.seekg(file_size - static_cast<std::streamoff>(footer.size()));
    source.read(reinterpret_cast<char*>(footer.data()), footer.size());
    if (!source || !std::equal(footer_magic.begin(), footer_magic.end(), footer.begin() + 10)) {
        throw std::runtime_error("The portable package data is missing or damaged.");
    }

    const auto root_length =
        static_cast<std::uint16_t>(footer[0]) |
        (static_cast<std::uint16_t>(footer[1]) << 8);
    std::uint64_t archive_length = 0;
    for (std::size_t index = 0; index < sizeof(archive_length); ++index) {
        archive_length |= static_cast<std::uint64_t>(footer[index + 2]) << (index * 8);
    }
    const auto payload_end =
        static_cast<std::streamoff>(file_size) - static_cast<std::streamoff>(footer_size);
    if (root_length == 0 || root_length > 256 ||
        root_length > static_cast<std::uint64_t>(payload_end) ||
        archive_length == 0 ||
        archive_length > static_cast<std::uint64_t>(payload_end) - root_length) {
        throw std::runtime_error("The portable package data is invalid.");
    }
    const auto root_offset = payload_end - root_length;
    const auto archive_offset = root_offset - static_cast<std::streamoff>(archive_length);

    std::string folder_utf8(root_length, '\0');
    source.seekg(root_offset);
    source.read(folder_utf8.data(), root_length);
    if (!source) {
        throw std::runtime_error("The portable package folder name could not be read.");
    }
    package_folder = utf8_to_wide(folder_utf8);
    if (package_folder == L"." || package_folder == L".." ||
        package_folder.find_first_of(L"\\/:") != std::wstring::npos) {
        throw std::runtime_error("The portable package folder name is invalid.");
    }

    wchar_t temp_directory[MAX_PATH + 1]{};
    const auto temp_length = GetTempPathW(MAX_PATH, temp_directory);
    if (temp_length == 0 || temp_length > MAX_PATH) {
        throw system_error_message(L"Could not locate the temporary folder", GetLastError());
    }
    wchar_t temp_file[MAX_PATH + 1]{};
    if (GetTempFileNameW(temp_directory, L"bcn", 0, temp_file) == 0) {
        throw system_error_message(L"Could not create a temporary package file", GetLastError());
    }
    DeleteFileW(temp_file);
    std::filesystem::path archive_path = temp_file;
    archive_path += L".zip";

    std::ofstream destination(archive_path, std::ios::binary | std::ios::trunc);
    if (!destination) {
        throw std::runtime_error("Could not prepare the temporary package file.");
    }
    source.clear();
    source.seekg(archive_offset);
    std::array<char, 65536> buffer{};
    auto remaining = archive_length;
    while (remaining > 0) {
        const auto chunk = static_cast<std::streamsize>(
            (remaining < buffer.size()) ? remaining : buffer.size()
        );
        source.read(buffer.data(), chunk);
        if (source.gcount() != chunk) {
            destination.close();
            DeleteFileW(archive_path.c_str());
            throw std::runtime_error("The embedded package could not be extracted.");
        }
        destination.write(buffer.data(), chunk);
        if (!destination) {
            destination.close();
            DeleteFileW(archive_path.c_str());
            throw std::runtime_error("Could not write the temporary package file.");
        }
        remaining -= static_cast<std::uint64_t>(chunk);
    }
    return archive_path;
}

std::wstring build_extraction_script(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& install_root,
    const std::wstring& package_folder
) {
    const auto archive = powershell_literal(archive_path.wstring());
    const auto root = powershell_literal(install_root.wstring());
    const auto folder = powershell_literal(package_folder);
    return L"$ErrorActionPreference = 'Stop'\n"
           L"$archivePath = " + archive + L"\n"
           L"$installRoot = " + root + L"\n"
           L"$appDirectory = Join-Path $installRoot " + folder + L"\n"
           L"$executable = Join-Path $appDirectory 'bcnminer.exe'\n"
           LR"PS(
try {
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        Expand-Archive -LiteralPath $archivePath -DestinationPath $installRoot -Force
    }
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw 'BCN Miner did not extract correctly.'
    }
    Start-Process -FilePath $executable -WorkingDirectory $appDirectory
} catch {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show(
        "BCN Miner could not be started.`n`n$($_.Exception.Message)",
        'BCN Miner Portable',
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error
    ) | Out-Null
    exit 1
} finally {
    Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
}
)PS";
}

int run_launcher() {
    const auto self = executable_path();
    std::wstring package_folder;
    const auto archive_path = copy_embedded_archive(self, package_folder);

    PWSTR local_app_data = nullptr;
    const auto folder_result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData,
        KF_FLAG_CREATE,
        nullptr,
        &local_app_data
    );
    if (FAILED(folder_result) || local_app_data == nullptr) {
        DeleteFileW(archive_path.c_str());
        throw std::runtime_error("Could not locate the user's local application data folder.");
    }

    const std::filesystem::path install_root =
        std::filesystem::path(local_app_data) / L"BCN Miner Portable";
    CoTaskMemFree(local_app_data);

    const auto mutex_name = L"Local\\BCNMinerPortable-" + package_folder;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (mutex == nullptr) {
        DeleteFileW(archive_path.c_str());
        throw system_error_message(L"Could not prepare the portable launcher", GetLastError());
    }
    const auto mutex_result = WaitForSingleObject(mutex, 120000);
    if (mutex_result != WAIT_OBJECT_0 && mutex_result != WAIT_ABANDONED) {
        CloseHandle(mutex);
        DeleteFileW(archive_path.c_str());
        throw std::runtime_error("Timed out while preparing BCN Miner.");
    }

    const auto script = build_extraction_script(archive_path, install_root, package_folder);
    const auto encoded_script = wide_to_base64(script);
    std::wstring command =
        L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
        L"-WindowStyle Hidden -EncodedCommand ";
    command.append(encoded_script.begin(), encoded_script.end());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const auto created = CreateProcessW(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process
    );
    if (!created) {
        const auto message =
            system_error_message(L"Could not start the Windows PowerShell launcher", GetLastError());
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        DeleteFileW(archive_path.c_str());
        throw message;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ReleaseMutex(mutex);
    CloseHandle(mutex);

    if (exit_code != 0) {
        return static_cast<int>(exit_code);
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        return run_launcher();
    } catch (const std::exception& error) {
        show_error(L"BCN Miner could not be started.\n\n" + utf8_to_wide(error.what()));
    } catch (const std::wstring& error) {
        show_error(L"BCN Miner could not be started.\n\n" + error);
    } catch (...) {
        show_error(L"BCN Miner could not be started because of an unexpected error.");
    }
    return 1;
}
