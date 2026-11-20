#
#  Copyright (c) 2026, The OTNS Authors.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are met:
#  1. Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
#  2. Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#  3. Neither the name of the copyright holder nor the
#     names of its contributors may be used to endorse or promote products
#     derived from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
#  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
#  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
#  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
#  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
#  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
#  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
#  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
#  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
#  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
#  POSSIBILITY OF SUCH DAMAGE.
#

cmake_minimum_required(VERSION 3.20)

project(ot-ns
    VERSION 3.0.0
    DESCRIPTION "OpenThread Network Simulator (OTNS v3)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OTNS_BUILD_TESTS "Build OTNS v3 C++ unit and integration tests" ON)
option(OTNS_BUILD_WEB "Build and embed web visualizer assets (requires Node.js)" OFF)
option(OTNS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(OTNS_WERROR "Treat compiler warnings as errors on OTNS targets" ON)

# Common compiler warning / sanitizer / OpenThread C++ flags for OTNS-owned targets
add_library(otns_build_options INTERFACE)
target_compile_options(otns_build_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
if(OTNS_WERROR)
    target_compile_options(otns_build_options INTERFACE -Werror)
endif()

if(OTNS_ENABLE_SANITIZERS)
    set(OTNS_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(otns_build_options INTERFACE ${OTNS_SANITIZER_FLAGS})
    target_link_options(otns_build_options INTERFACE ${OTNS_SANITIZER_FLAGS})
endif()

# Vendored header-only libraries (system include suppresses third-party warnings)
add_library(otns_nlohmann_json INTERFACE)
target_include_directories(otns_nlohmann_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(otns_nlohmann_json INTERFACE JSON_NOEXCEPTION)

add_library(otns_doctest INTERFACE)
target_include_directories(otns_doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(otns_doctest INTERFACE DOCTEST_CONFIG_NO_EXCEPTIONS)

# -----------------------------------------------------------------------------
# 1. Standalone RF Physics Library (libotns_rf — zero OS/OpenThread deps)
# -----------------------------------------------------------------------------
add_library(otns_rf STATIC
    src/rf/prng.cpp
    src/rf/spatial_index.cpp
    src/rf/pathloss_fading.cpp
    src/rf/ber_model.cpp
    src/rf/radio_model.cpp
)
target_include_directories(otns_rf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(otns_rf PRIVATE otns_build_options)

# -----------------------------------------------------------------------------
# 2. ot-rfsim v3 Binary/TLV Wire Protocol Codec (zero OS/OpenThread deps)
# -----------------------------------------------------------------------------
add_library(otns_protocol STATIC
    src/protocol/rfsim_v3_codec.cpp
)
target_include_directories(otns_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(otns_protocol PRIVATE otns_build_options)

# -----------------------------------------------------------------------------
# 3. Embedded OpenThread Multi-Instance Core (for In-Process Hybrid Nodes)
# -----------------------------------------------------------------------------
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OT_PLATFORM "external" CACHE STRING "" FORCE)
set(OT_THREAD_VERSION "1.4" CACHE STRING "" FORCE)
set(OT_APP_CLI OFF CACHE BOOL "" FORCE)
set(OT_APP_NCP OFF CACHE BOOL "" FORCE)
set(OT_APP_RCP OFF CACHE BOOL "" FORCE)
set(OT_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(OT_FTD ON CACHE BOOL "" FORCE)
set(OT_MTD OFF CACHE BOOL "" FORCE)
set(OT_RCP OFF CACHE BOOL "" FORCE)
set(OT_15_4 ON CACHE BOOL "" FORCE)
set(OT_PING_SENDER ON CACHE BOOL "" FORCE)
set(OT_BUILTIN_MBEDTLS ON CACHE BOOL "" FORCE)
set(OT_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
set(OT_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/src/nodes/openthread-core-otns-config.h" CACHE STRING "" FORCE)

if(OTNS_ENABLE_SANITIZERS)
    add_compile_options(${OTNS_SANITIZER_FLAGS})
    add_link_options(${OTNS_SANITIZER_FLAGS})
endif()

add_subdirectory(openthread EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# 4. Core Simulator Library (libotns)
# -----------------------------------------------------------------------------
add_library(otns_core STATIC
    src/core/scheduler.cpp
    src/core/simulation.cpp
    src/nodes/inproc_node.cpp
    openthread/examples/platforms/utils/mac_frame.cpp
)
target_include_directories(otns_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/openthread/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/openthread/src
        ${CMAKE_CURRENT_SOURCE_DIR}/openthread/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/openthread/examples/platforms/utils
)
# mac_frame.cpp is OpenThread code, so suppress our stricter conversion warnings on it
set_source_files_properties(openthread/examples/platforms/utils/mac_frame.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-conversion;-Wno-sign-conversion;-Wno-shadow;-Wno-pedantic"
)

add_library(otns_inproc_platform STATIC
    src/nodes/inproc_platform.cpp
)
target_include_directories(otns_inproc_platform
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/openthread/include
        ${CMAKE_CURRENT_SOURCE_DIR}/openthread/examples/platforms/utils
)
target_link_libraries(otns_inproc_platform
    PRIVATE
        otns_build_options
        ot-config
)
# OpenThread core calls into the external platform library (otPlat*)
target_link_libraries(openthread-ftd PUBLIC otns_inproc_platform)

target_link_libraries(otns_core
    PUBLIC
        otns_rf
        otns_protocol
        otns_nlohmann_json
        openthread-cli-ftd
        openthread-ftd
        otns_inproc_platform
        ot-config
        mbedtls
    PRIVATE
        otns_build_options
)
# Break the static archive cycle cleanly on GNU ld
target_link_libraries(otns_inproc_platform PUBLIC otns_core)

# -----------------------------------------------------------------------------
# 5. Tests (CTest + doctest)
# -----------------------------------------------------------------------------
if(OTNS_BUILD_TESTS)
    enable_testing()

    add_executable(test_rf
        tests/test_main.cpp
        tests/test_rf.cpp
    )
    target_link_libraries(test_rf PRIVATE otns_rf otns_doctest otns_build_options)
    add_test(NAME test_rf COMMAND test_rf)

    add_executable(test_protocol
        tests/test_main.cpp
        tests/test_protocol.cpp
    )
    target_link_libraries(test_protocol PRIVATE otns_protocol otns_doctest otns_build_options)
    add_test(NAME test_protocol COMMAND test_protocol)

    add_executable(test_simulation
        tests/test_main.cpp
        tests/test_simulation.cpp
    )
    target_link_libraries(test_simulation PRIVATE otns_core otns_doctest otns_build_options)
    add_test(NAME test_simulation COMMAND test_simulation)

    add_executable(bench_scale
        tests/bench_scale.cpp
    )
    target_link_libraries(bench_scale PRIVATE otns_core otns_build_options)
    add_test(NAME test_scale_100_nodes COMMAND bench_scale 100 30)
endif()
