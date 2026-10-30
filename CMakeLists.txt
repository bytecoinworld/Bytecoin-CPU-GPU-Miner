cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(bcnminer
    VERSION 0.1.0
    DESCRIPTION "Cross-platform Bytecoin CPU/GPU miner"
    LANGUAGES C CXX
)

include(CTest)
find_package(OpenSSL QUIET)

add_library(bcnminer_core
    src/auto_config.cpp
    src/gui_server.cpp
    src/json.cpp
    src/opencl_miner.cpp
    src/pow_worker.cpp
    src/pool_miner.cpp
    src/pool_protocol.cpp
    src/slow_hash.cpp
)
target_include_directories(bcnminer_core PUBLIC include)
target_compile_features(bcnminer_core PUBLIC cxx_std_20)
target_compile_definitions(
    bcnminer_core PRIVATE
    "BCNMINER_OPENCL_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/third_party/xmrig-amd/opencl\""
)

set(BYTECOIN_CRYPTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bytecoin/src/crypto")
add_library(bytecoin_cryptonight STATIC
    "${BYTECOIN_CRYPTO_DIR}/slow-hash_stdc.c"
    "${BYTECOIN_CRYPTO_DIR}/hash-blake.c"
    "${BYTECOIN_CRYPTO_DIR}/hash-groestl.c"
    "${BYTECOIN_CRYPTO_DIR}/hash-jh.c"
    "${BYTECOIN_CRYPTO_DIR}/hash-keccak.c"
    "${BYTECOIN_CRYPTO_DIR}/hash-skein.c"
    "${BYTECOIN_CRYPTO_DIR}/blake/blake256.c"
    "${BYTECOIN_CRYPTO_DIR}/groestl/Groestl-opt.c"
    "${BYTECOIN_CRYPTO_DIR}/jh/jh.c"
    "${BYTECOIN_CRYPTO_DIR}/keccak/Keccak-readable-and-compact.c"
    "${BYTECOIN_CRYPTO_DIR}/oaes/oaes_lib.c"
    "${BYTECOIN_CRYPTO_DIR}/skein/skein.c"
)
target_include_directories(bytecoin_cryptonight PUBLIC "${BYTECOIN_CRYPTO_DIR}")
target_compile_features(bytecoin_cryptonight PRIVATE c_std_99)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    target_sources(bytecoin_cryptonight PRIVATE
        "${BYTECOIN_CRYPTO_DIR}/slow-hash_x86.c"
    )
    if(NOT MSVC AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        # This file selects its AES-NI or software implementation at runtime.
        set_source_files_properties(
            "${BYTECOIN_CRYPTO_DIR}/slow-hash_x86.c"
            PROPERTIES COMPILE_OPTIONS "-maes"
        )
    endif()
    target_compile_definitions(bcnminer_core PRIVATE BCNMINER_USE_BYTECOIN_X86_HASH=1)
endif()

target_link_libraries(bcnminer_core PRIVATE bytecoin_cryptonight)
if(CMAKE_DL_LIBS)
    target_link_libraries(bcnminer_core PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(bcnminer_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(bcnminer_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(bcnminer src/main.cpp)
target_link_libraries(bcnminer PRIVATE bcnminer_core)
if(WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/bcnminer.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/bcnminer.rc"
        @ONLY
    )
    target_sources(bcnminer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bcnminer.rc")

    add_executable(
        bcnminer_portable_launcher
        WIN32
        packaging/portable_launcher.cpp
    )
    target_compile_features(bcnminer_portable_launcher PRIVATE cxx_std_20)
    target_link_libraries(bcnminer_portable_launcher PRIVATE ole32 shell32)
    if(MSVC)
        target_compile_options(bcnminer_portable_launcher PRIVATE /W4 /permissive-)
        set_property(
            TARGET bcnminer_portable_launcher
            PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/bcnminer_portable_launcher.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/bcnminer_portable_launcher.rc"
        @ONLY
    )
    target_sources(
        bcnminer_portable_launcher
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bcnminer_portable_launcher.rc"
    )
endif()
add_custom_command(
    TARGET bcnminer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xmrig-amd/opencl"
        "$<TARGET_FILE_DIR:bcnminer>/kernels"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/gui"
        "$<TARGET_FILE_DIR:bcnminer>/web"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xmrig-amd/LICENSE"
        "$<TARGET_FILE_DIR:bcnminer>/GPL-3.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/bytecoin.ico"
        "$<TARGET_FILE_DIR:bcnminer>/web/favicon.ico"
    VERBATIM
)

if(WIN32)
    target_link_libraries(bcnminer_core PRIVATE ws2_32 shell32)
endif()

if(OpenSSL_FOUND)
    target_compile_definitions(bcnminer_core PRIVATE BCNMINER_HAS_OPENSSL=1)
    target_link_libraries(bcnminer_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "Verified TLS pool connections are enabled.")
else()
    message(STATUS "OpenSSL not found; TLS pool connections will be unavailable.")
endif()

if(BUILD_TESTING)
    add_executable(bcnminer_tests tests/auto_config_tests.cpp)
    target_link_libraries(bcnminer_tests PRIVATE bcnminer_core)
    add_test(NAME bcnminer_tests COMMAND bcnminer_tests)

    add_executable(bcnminer_slow_hash_tests tests/slow_hash_tests.cpp)
    target_link_libraries(bcnminer_slow_hash_tests PRIVATE bcnminer_core)
    target_include_directories(bcnminer_slow_hash_tests PRIVATE "${BYTECOIN_CRYPTO_DIR}")
    add_test(
        NAME bytecoin_slow_hash_vectors
        COMMAND bcnminer_slow_hash_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bytecoin/tests/hash/tests-slow.txt"
    )

    add_executable(bcnminer_pow_worker_tests tests/pow_worker_tests.cpp)
    target_link_libraries(bcnminer_pow_worker_tests PRIVATE bcnminer_core)
    add_test(NAME pow_worker_tests COMMAND bcnminer_pow_worker_tests)

    add_executable(bcnminer_pool_protocol_tests tests/pool_protocol_tests.cpp)
    target_link_libraries(bcnminer_pool_protocol_tests PRIVATE bcnminer_core)
    add_test(NAME pool_protocol_tests COMMAND bcnminer_pool_protocol_tests)

    function(add_cli_test test_name expected_result expected_output)
        set(arguments ${ARGN})
        add_test(
            NAME "cli_${test_name}"
            COMMAND ${CMAKE_COMMAND}
                "-DMINER=$<TARGET_FILE:bcnminer>"
                "-DARGS=${arguments}"
                "-DEXPECTED_RESULT=${expected_result}"
                "-DEXPECTED_OUTPUT=${expected_output}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.cmake"
        )
    endfunction()

    add_cli_test(help 0 "Usage:" --help)
    add_cli_test(
        keyboard_help 0
        "Mining keyboard shortcuts (interactive console):"
        --help
    )
    add_cli_test(help_short 0 "Usage:" -h)
    add_cli_test(help_command 0 "Usage:" help)
    add_cli_test(devices 0 "OpenCL:" --devices)
    add_cli_test(
        gpu_test_help 0
        "Compile and verify OpenCL GPU kernels without a pool"
        --help
    )
    add_cli_test(auto_config 0 "Selected CPU worker threads:" --auto-config)
    add_cli_test(
        hash_vector 0
        "2f8e3df40bd11f9ac90c743ca8e32bb391da4fb98612aa3b6cdc639ee00b31f5"
        --hash-hex 6465206f6d6e69627573206475626974616e64756d
    )
    add_cli_test(
        mine 2
        "--mine requires both --pool host:port and --wallet address."
        --mine
    )
    add_cli_test(
        gpu_nonce_offset 2
        "OpenCL GPU mining currently requires --nonce-offset 39"
        --mine --pool example.invalid:3333 --wallet test-wallet --backend gpu
            --nonce-offset 38
    )
    add_cli_test(invalid 2 "Unknown or invalid arguments." --not-a-command)
endif()

if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
    include(InstallRequiredSystemLibraries)
endif()

include(GNUInstallDirs)
if(WIN32)
    install(
        TARGETS bcnminer
        RUNTIME_DEPENDENCY_SET bcnminer_runtime_deps
        RUNTIME DESTINATION .
    )
    install(
        RUNTIME_DEPENDENCY_SET bcnminer_runtime_deps
        PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-"
        POST_EXCLUDE_REGEXES ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
        RUNTIME DESTINATION .
    )
else()
    install(TARGETS bcnminer RUNTIME DESTINATION .)
endif()
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/gui/" DESTINATION web)
install(DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xmrig-amd/opencl/"
    DESTINATION kernels
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/bytecoin.ico"
    DESTINATION web RENAME favicon.ico
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION .
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xmrig-amd/LICENSE"
    DESTINATION . RENAME GPL-3.0.txt
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bytecoin/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bytecoin/README.md"
    DESTINATION licenses/bytecoin
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xmrig-amd/README.md"
    DESTINATION licenses/xmrig-amd
)

set(CPACK_PACKAGE_NAME "bcnminer")
set(CPACK_PACKAGE_VENDOR "bytecoin.world community")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Bytecoin CPU and OpenCL GPU miner")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "bcnminer-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "BCN Miner")
set(CPACK_PACKAGE_EXECUTABLES "bcnminer" "BCN Miner")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_BINARY_DIR}/CPackLicense.txt"
    COPYONLY
)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/CPackLicense.txt")
set(CPACK_WIX_VERSION "4")
set(CPACK_WIX_UPGRADE_GUID "CBDD5C31-F967-4BA6-A16D-DEFAE12B6571")
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/bytecoin.ico")
set(CPACK_WIX_INSTALL_SCOPE "perUser")
set(CPACK_WIX_EXTENSIONS "WixToolset.Util.wixext")
set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/bcnminer-wix-patch.xml")
set(CPACK_WIX_EXTRA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/bcnminer-wix-shortcut.wxs")
set(CPACK_GENERATOR "ZIP;WIX")
include(CPack)
